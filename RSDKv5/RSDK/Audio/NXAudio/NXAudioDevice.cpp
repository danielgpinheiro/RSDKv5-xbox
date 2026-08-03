#include <xboxkrnl/xboxkrnl.h>
extern "C" {
#include <nxaudio.h>
}

uint8 AudioDevice::contextInitialized;

// Music streaming voice: ~185ms per buffer at 44.1kHz stereo S16; two buffers =
// ~370ms of headroom so a stalled frame (the pump runs from FrameInit) doesn't
// underrun. NX_CHUNK_FRAMES must stay an integer multiple of the engine's Vorbis
// ring (MIX_BUFFER_SIZE/2 stereo frames) so FillMusicChunk consumes whole rings.
#define NX_CHUNK_FRAMES (8192)
#define NX_NUM_BUFFERS  (2)
#define NX_CHUNK_BYTES  (NX_CHUNK_FRAMES * AUDIO_CHANNELS * (int32)sizeof(int16))

#define NX_RING_FLOATS  (MIX_BUFFER_SIZE)         // interleaved-stereo floats per ring refill (0x800)
#define NX_RING_FRAMES  (MIX_BUFFER_SIZE / 2)     // stereo frames per ring refill (1024)
#define NX_RING_ITERS   (NX_CHUNK_FRAMES / NX_RING_FRAMES) // rings per music chunk (8)

// Per-slot SFX voice lifecycle phase.
enum { NX_SLOT_FREE, NX_SLOT_PENDING, NX_SLOT_PLAYING };

namespace RSDK
{
// --- SFX: one static hardware voice per engine channel ------------------------
static nxAudioVoice sfxVoices[CHANNEL_COUNT];
static nxAudioBuffer sfxBufs[CHANNEL_COUNT]; // persistent: the voice holds a pointer to this
static bool32 sfxVoiceOk[CHANNEL_COUNT];
static uint8 sfxPhase[CHANNEL_COUNT];
static int32 sfxCachePlay[CHANNEL_COUNT];  // last-seen channel->playIndex
static int16 sfxCacheSound[CHANNEL_COUNT]; // last-seen channel->soundID

// --- Music: one streaming hardware voice for the (single) stream channel ------
// Brought up lazily on the first stream, then kept ALWAYS running (feeding
// silence when no stream is active). A streaming voice that is stopped can leave
// its two hardware buffer slots occupied, so restarting it risks QUEUE_FULL;
// keeping it always-on sidesteps that. Only a genuine underrun (self-stop) or
// Release ever stops it.
static nxAudioVoice musicVoice;
static bool32 musicVoiceOk;
static bool32 musicStarted;
static int16 *musicChunks[NX_NUM_BUFFERS];
static nxAudioBuffer musicBuffers[NX_NUM_BUFFERS];
static volatile LONG musicCompleted; // incremented by the APU DPC, drained by FrameInit
static int32 musicNextFill;

static bool32 nxReady;

// Map channel volume/pan/speed onto the hardware voice. Matches the old software
// mixer's formulas exactly so levels and panning are unchanged: per-side gain =
// channel->volume * pan-factor, scaled by the global SFX/stream volume.
static void ApplyChannelParams(nxAudioVoice *v, ChannelInfo *ch, bool32 isSfx)
{
    float globalVol = isSfx ? engine.soundFXVolume : engine.streamVolume;

    float vol  = ch->volume;
    float volL = vol, volR = vol;
    if (ch->pan < 0.0f)
        volR = (1.0f + ch->pan) * vol;
    else
        volL = (1.0f - ch->pan) * vol;

    nxAudioVoiceSetMasterGain(v, globalVol);
    nxAudioVoiceSetChannelGain(v, volL, volR, 0.0f, 0.0f, 0.0f, 0.0f);

    float pitch = (float)ch->speed / (float)TO_FIXED(1);
    if (pitch <= 0.0f)
        pitch = 1.0f;
    nxAudioVoiceSetPitch(v, pitch);
}

// Submit the channel's SFX buffer to its (stopped) voice and start it. The caller
// guarantees the voice is NX_STOPPED (Submit requires it).
static void SubmitSfx(int32 i, ChannelInfo *ch)
{
    int16 sfx = ch->soundID;
    if (sfx < 0 || sfx >= SFX_COUNT || !sfxList[sfx].buffer || !sfxList[sfx].length) {
        ch->state         = CHANNEL_IDLE;
        ch->soundID       = -1;
        sfxPhase[i]       = NX_SLOT_FREE;
        sfxCacheSound[i]  = -1;
        return;
    }

    // DMA straight from the paged SFX pool (mono S16); no copy.
    nxAudioBufferInitialize(&sfxBufs[i], sfxList[sfx].buffer, (uint32)(sfxList[sfx].length * sizeof(int16)));
    nxAudioVoiceSetLooping(&sfxVoices[i], ch->loop != (uint32)-1);
    ApplyChannelParams(&sfxVoices[i], ch, true);

    if (!nxAudioBufferSubmit(&sfxVoices[i], &sfxBufs[i])) {
        ch->state        = CHANNEL_IDLE;
        ch->soundID      = -1;
        sfxPhase[i]      = NX_SLOT_FREE;
        sfxCacheSound[i] = -1;
        return;
    }
    nxAudioVoiceStart(&sfxVoices[i]);

    sfxPhase[i]      = NX_SLOT_PLAYING;
    sfxCachePlay[i]  = ch->playIndex;
    sfxCacheSound[i] = ch->soundID;
}

// Fill one music chunk (S16 stereo) and queue it. With a live stream channel the
// data comes from its Vorbis ring (already 0.5-attenuated by UpdateStreamBuffer;
// hardware applies vol/pan/pitch, so this only converts F32->S16 and consumes
// whole rings, refilling each). With no stream (ch == NULL) the chunk is silence,
// which keeps the always-on voice fed between tracks.
static void FillMusicChunk(int32 idx, ChannelInfo *ch)
{
    int16 *out = musicChunks[idx];

    if (!ch || (ch->state & 0x3F) != CHANNEL_STREAM) {
        memset(out, 0, NX_CHUNK_BYTES);
    }
    else {
        for (int32 k = 0; k < NX_RING_ITERS; ++k) {
            if ((ch->state & 0x3F) != CHANNEL_STREAM) {
                // Stream ended mid-chunk (UpdateStreamBuffer set it idle) — silence the rest.
                memset(out, 0, (NX_RING_ITERS - k) * NX_RING_FLOATS * sizeof(int16));
                break;
            }

            const float *ring = ch->samplePtr;
            for (int32 f = 0; f < NX_RING_FLOATS; ++f) {
                float s = ring[f];
                if (s > 1.0f)
                    s = 1.0f;
                else if (s < -1.0f)
                    s = -1.0f;
                out[f] = (int16)(s * 32767.0f);
            }
            out += NX_RING_FLOATS;

            UpdateStreamBuffer(ch); // decode the next ring (may set state idle at EOF)
        }
    }

    nxAudioBufferInitialize(&musicBuffers[idx], musicChunks[idx], NX_CHUNK_BYTES);
    nxAudioBufferQueue(&musicVoice, &musicBuffers[idx]);
}

// APU DPC context (DISPATCH_LEVEL): flag only — no mixing/decoding here.
static void NXMusicCallback(struct nxAudioVoice *voice, void *user_context)
{
    (void)voice;
    (void)user_context;
    InterlockedIncrement(&musicCompleted);
}

// Reconcile the (single) stream channel with the always-on music voice. Brought
// up on the first stream (immediate, no silence pre-roll), then kept running:
// pump completed buffers with decoded audio when a stream is present or silence
// otherwise, self-heal on underrun, pause/resume with the stream.
static void ReconcileMusic()
{
    int32 streamCh = -1;
    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if ((channels[c].state & 0x3F) == CHANNEL_STREAM) {
            streamCh = c;
            break;
        }
    }

    ChannelInfo *ch = (streamCh >= 0) ? &channels[streamCh] : NULL;
    bool32 paused   = ch && (ch->state & CHANNEL_PAUSED) != 0;

    if (!musicStarted) {
        // Wait for the first real stream so music starts without a silence pre-roll.
        if (!ch)
            return;
        musicCompleted = 0;
        musicNextFill  = 0;
        FillMusicChunk(0, ch);
        FillMusicChunk(1, ch);
        ApplyChannelParams(&musicVoice, ch, false);
        nxAudioVoiceStart(&musicVoice);
        musicStarted = true;
        return;
    }

    nxAudioVoiceState st = nxAudioVoiceGetState(&musicVoice);

    if (st == NX_STOPPED) {
        // Underrun (both buffers drained during a long stall) — re-prime and restart.
        // Completed buffers were released by the DPC, so the queue slots are free.
        musicCompleted = 0;
        musicNextFill  = 0;
        FillMusicChunk(0, ch);
        FillMusicChunk(1, ch);
        if (ch)
            ApplyChannelParams(&musicVoice, ch, false);
        nxAudioVoiceStart(&musicVoice);
        return;
    }

    if (paused) {
        if (st == NX_PLAYING)
            nxAudioVoicePause(&musicVoice);
        return;
    }

    if (st == NX_PAUSED)
        nxAudioVoiceStart(&musicVoice);

    while (musicCompleted > 0) {
        InterlockedDecrement(&musicCompleted);
        FillMusicChunk(musicNextFill, ch);
        musicNextFill = (musicNextFill + 1) % NX_NUM_BUFFERS;
    }

    if (ch)
        ApplyChannelParams(&musicVoice, ch, false);
}
} // namespace RSDK

bool32 AudioDevice::Init()
{
    if (!contextInitialized) {
        contextInitialized = true;
        InitAudioChannels();
    }

    audioState = false;

    // The Xbox reset button does a WARM reset: the MCPX APU keeps running with the
    // previous session's config and nxAudioShutdown never ran, so nxAudioInit would
    // start on a dirty APU (-> muted after reset). Scrub it first. Safe on a cold
    // boot too: nxAudioShutdown's frees are NULL-guarded and it only writes APU/AC97
    // reset registers.
    nxAudioShutdown();

    nxAudioInitParams params = { 0 };
    if (!nxAudioInit(&params)) {
        // debugPrint survives the perf build (logging compiled out) -> xbwatson,
        // so the APU bring-up on real hardware isn't blind.
        debugPrint("NXAUDIO: nxAudioInit FAILED (err=%d)\n", (int)nxAudioGetLastError());
        PrintLog(PRINT_NORMAL, "ERROR: nxAudioInit failed");
        return true;
    }
    debugPrint("NXAUDIO: nxAudioInit OK (post-scrub)\n");

    // One static (mono) voice per engine channel — the APU mixes them.
    nxAudioFormat sfxFmt    = {};
    sfxFmt.sample_rate      = AUDIO_FREQUENCY; // SFX are authored at the output rate; speed -> pitch
    sfxFmt.channels         = 1;
    sfxFmt.bytes_per_sample = sizeof(int16);
    sfxFmt.codec            = NX_AUDIO_CODEC_PCM;
    sfxFmt.type             = NX_VOICE_TYPE_2D_STATIC;

    int32 sfxCreated = 0;
    for (int32 i = 0; i < CHANNEL_COUNT; ++i) {
        sfxVoiceOk[i]    = nxAudioVoiceCreate(&sfxVoices[i], &sfxFmt) ? true : false;
        sfxPhase[i]      = NX_SLOT_FREE;
        sfxCachePlay[i]  = -1;
        sfxCacheSound[i] = -1;
        if (sfxVoiceOk[i])
            ++sfxCreated;
    }

    // One streaming (stereo) voice for music/video audio.
    nxAudioFormat musFmt    = {};
    musFmt.sample_rate      = AUDIO_FREQUENCY;
    musFmt.channels         = AUDIO_CHANNELS;
    musFmt.bytes_per_sample = sizeof(int16);
    musFmt.codec            = NX_AUDIO_CODEC_PCM;
    musFmt.type             = NX_VOICE_TYPE_2D_STREAM;

    musicVoiceOk = nxAudioVoiceCreate(&musicVoice, &musFmt) ? true : false;
    musicStarted = false;
    if (musicVoiceOk) {
        nxAudioBufferSetCallback(&musicVoice, NXMusicCallback, NULL);
        for (int32 i = 0; i < NX_NUM_BUFFERS; ++i) {
            musicChunks[i] = (int16 *)MmAllocateContiguousMemory(NX_CHUNK_BYTES);
            if (!musicChunks[i]) {
                PrintLog(PRINT_NORMAL, "ERROR: nxdk-audio music chunk alloc failed");
                musicVoiceOk = false;
                break;
            }
        }
    }

    debugPrint("NXAUDIO: Plan B up — %d/%d sfx voices, music=%d\n", (int)sfxCreated, (int)CHANNEL_COUNT, (int)musicVoiceOk);

    nxReady    = true;
    audioState = true;
    return true;
}

void AudioDevice::FrameInit()
{
    if (!nxReady)
        return;

    for (int32 i = 0; i < CHANNEL_COUNT; ++i) {
        if (!sfxVoiceOk[i])
            continue;

        ChannelInfo *ch = &channels[i];
        uint8 base      = ch->state & 0x3F;

        if (base != CHANNEL_SFX) {
            // Channel is idle / a stream / loading — make sure this static voice is
            // stopped (covers SFX->stream reuse of a slot, StopChannel, etc.).
            if (sfxPhase[i] != NX_SLOT_FREE) {
                if (nxAudioVoiceGetState(&sfxVoices[i]) != NX_STOPPED)
                    nxAudioVoiceStop(&sfxVoices[i]);
                sfxPhase[i] = NX_SLOT_FREE;
            }
            sfxCacheSound[i] = -1;
            continue;
        }

        bool32 paused = (ch->state & CHANNEL_PAUSED) != 0;

        // A different (soundID, playIndex) than cached means a fresh PlaySfx landed
        // on this slot. playIndex is bumped on every play, so replays of the same
        // sound are detected too.
        bool32 changed = (ch->playIndex != sfxCachePlay[i]) || (ch->soundID != sfxCacheSound[i]);

        if (changed) {
            sfxCachePlay[i]  = ch->playIndex;
            sfxCacheSound[i] = ch->soundID;
            if (nxAudioVoiceGetState(&sfxVoices[i]) != NX_STOPPED) {
                // Voice still busy (steal): stop now, submit once it reaches STOPPED.
                nxAudioVoiceStop(&sfxVoices[i]);
                sfxPhase[i] = NX_SLOT_PENDING;
            }
            else {
                SubmitSfx(i, ch);
            }
            continue;
        }

        if (sfxPhase[i] == NX_SLOT_PENDING) {
            if (nxAudioVoiceGetState(&sfxVoices[i]) == NX_STOPPED)
                SubmitSfx(i, ch);
            continue;
        }

        if (sfxPhase[i] == NX_SLOT_PLAYING) {
            nxAudioVoiceState st = nxAudioVoiceGetState(&sfxVoices[i]);
            if (st == NX_STOPPED) {
                // Finished (non-looping) — mirror the mixer's end-of-sample transition
                // so SfxPlaying()/ChannelActive() stay correct for game logic.
                ch->state        = CHANNEL_IDLE;
                ch->soundID      = -1;
                sfxPhase[i]      = NX_SLOT_FREE;
                sfxCacheSound[i] = -1;
            }
            else {
                ApplyChannelParams(&sfxVoices[i], ch, true);
                if (paused && st == NX_PLAYING)
                    nxAudioVoicePause(&sfxVoices[i]);
                else if (!paused && st == NX_PAUSED)
                    nxAudioVoiceStart(&sfxVoices[i]);
            }
        }
    }

    if (musicVoiceOk)
        ReconcileMusic();
}

void AudioDevice::StopSfxVoices()
{
    if (!nxReady)
        return;

    for (int32 i = 0; i < CHANNEL_COUNT; ++i) {
        if (!sfxVoiceOk[i])
            continue;
        if (nxAudioVoiceGetState(&sfxVoices[i]) != NX_STOPPED)
            nxAudioVoiceStop(&sfxVoices[i]);
        sfxPhase[i]      = NX_SLOT_FREE;
        sfxCacheSound[i] = -1;
    }
}

uint32 AudioDevice::GetSfxPlaybackSamples(uint32 channel)
{
    if (channel < CHANNEL_COUNT && sfxVoiceOk[channel] && sfxPhase[channel] == NX_SLOT_PLAYING)
        return nxAudioVoiceGetPlaybackOffset(&sfxVoices[channel]) / (uint32)sizeof(int16);
    return 0;
}

void AudioDevice::Release()
{
    if (nxReady) {
        nxReady = false;

        for (int32 i = 0; i < CHANNEL_COUNT; ++i) {
            if (sfxVoiceOk[i]) {
                nxAudioVoiceStop(&sfxVoices[i]);
                nxAudioVoiceDestroy(&sfxVoices[i]);
                sfxVoiceOk[i] = false;
            }
        }

        if (musicVoiceOk) {
            nxAudioVoiceStop(&musicVoice);
            nxAudioVoiceDestroy(&musicVoice);
        }
        musicStarted = false;

        AudioDeviceBase::Release();

        for (int32 i = 0; i < NX_NUM_BUFFERS; ++i) {
            if (musicChunks[i]) {
                MmFreeContiguousMemory(musicChunks[i]);
                musicChunks[i] = NULL;
            }
        }
        nxAudioShutdown();
    }
    audioState = false;
}

void AudioDevice::InitAudioChannels() { AudioDeviceBase::InitAudioChannels(); }
