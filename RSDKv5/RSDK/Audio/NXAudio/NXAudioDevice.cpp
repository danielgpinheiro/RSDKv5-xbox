#include <xboxkrnl/xboxkrnl.h>
extern "C" {
#include <nxaudio.h>
}

uint8 AudioDevice::contextInitialized;

// ~185ms per buffer at 44.1kHz stereo S16; two buffers = ~370ms of headroom so a
// stalled frame (the pump runs from FrameInit) doesn't underrun. Matches the old
// SDL_XBOXAUDIO_BUFFER_FRAMES=8192 latency.
#define NX_CHUNK_FRAMES (8192)
#define NX_NUM_BUFFERS  (2)
#define NX_CHUNK_BYTES  (NX_CHUNK_FRAMES * AUDIO_CHANNELS * (int32)sizeof(int16))

namespace RSDK
{
static nxAudioVoice nxVoice;
static nxAudioBuffer nxBuffers[NX_NUM_BUFFERS];
static int16 *nxChunks[NX_NUM_BUFFERS];
static volatile LONG nxBuffersCompleted; // incremented by the APU DPC, drained by FrameInit
static int32 nxNextFill;
static bool32 nxReady;

// Fill one chunk from the engine mixer (F32) and convert to interleaved S16.
static void FillChunk(int32 idx)
{
    static float mix[NX_CHUNK_FRAMES * AUDIO_CHANNELS];
    AudioDevice::ProcessAudioMixing(mix, NX_CHUNK_FRAMES * AUDIO_CHANNELS); // zeroes + mixes all channels

    int16 *out = nxChunks[idx];
    for (int32 i = 0; i < NX_CHUNK_FRAMES * AUDIO_CHANNELS; ++i) {
        float s = mix[i];
        if (s > 1.0f)
            s = 1.0f;
        else if (s < -1.0f)
            s = -1.0f;
        out[i] = (int16)(s * 32767.0f);
    }

    nxAudioBufferInitialize(&nxBuffers[idx], nxChunks[idx], NX_CHUNK_BYTES);
    nxAudioBufferQueue(&nxVoice, &nxBuffers[idx]);
}

// APU DPC context (DISPATCH_LEVEL): flag only — no mixing/decoding here.
static void NXVoiceCallback(struct nxAudioVoice *voice, void *user_context)
{
    (void)voice;
    (void)user_context;
    InterlockedIncrement(&nxBuffersCompleted);
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

    nxAudioFormat format    = {};
    format.sample_rate      = AUDIO_FREQUENCY; // APU hardware-resamples to the 48kHz device rate
    format.channels         = AUDIO_CHANNELS;
    format.bytes_per_sample = sizeof(int16);
    format.codec            = NX_AUDIO_CODEC_PCM;
    format.type             = NX_VOICE_TYPE_2D_STREAM;

    if (!nxAudioVoiceCreate(&nxVoice, &format)) {
        PrintLog(PRINT_NORMAL, "ERROR: nxAudioVoiceCreate failed");
        nxAudioShutdown();
        return true;
    }
    nxAudioVoiceSetMasterGain(&nxVoice, 1.0f);

    nxBuffersCompleted = 0;
    nxNextFill         = 0;
    for (int32 i = 0; i < NX_NUM_BUFFERS; ++i) {
        nxChunks[i] = (int16 *)MmAllocateContiguousMemory(NX_CHUNK_BYTES);
        if (!nxChunks[i]) {
            PrintLog(PRINT_NORMAL, "ERROR: nxdk-audio chunk alloc failed");
            nxAudioVoiceDestroy(&nxVoice);
            nxAudioShutdown();
            return true;
        }
    }

    nxAudioBufferSetCallback(&nxVoice, NXVoiceCallback, NULL);

    // Prime both buffers, then start.
    for (int32 i = 0; i < NX_NUM_BUFFERS; ++i) FillChunk(i);

    nxReady    = true;
    audioState = nxAudioVoiceStart(&nxVoice) ? true : false;
    debugPrint("NXAUDIO: voice started, audioState=%d\n", (int)audioState);
    return true;
}

void AudioDevice::FrameInit()
{
    if (!nxReady)
        return;

    // Underrun recovery: a long scene load starves this pump (FrameInit doesn't
    // tick), both queued buffers drain, and the streaming voice STOPS — muted for
    // the rest of the session, and queueing to a stopped voice can wedge the APU
    // (suspected Blue Spheres freeze). If stopped, re-prime and restart instead of
    // queueing. Check state BEFORE refilling so we never queue to a dead voice.
    if (nxAudioVoiceGetState(&nxVoice) == NX_STOPPED) {
        debugPrint("NXAUDIO: voice underran/stopped — restarting\n");
        nxBuffersCompleted = 0;
        nxNextFill         = 0;
        FillChunk(0);
        FillChunk(1);
        nxAudioVoiceStart(&nxVoice);
        return;
    }

    while (nxBuffersCompleted > 0) {
        InterlockedDecrement(&nxBuffersCompleted);
        FillChunk(nxNextFill);
        nxNextFill = (nxNextFill + 1) % NX_NUM_BUFFERS;
    }
}

void AudioDevice::Release()
{
    if (nxReady) {
        nxReady = false;
        nxAudioVoiceStop(&nxVoice);
        nxAudioVoiceDestroy(&nxVoice);

        AudioDeviceBase::Release();

        for (int32 i = 0; i < NX_NUM_BUFFERS; ++i) {
            if (nxChunks[i]) {
                MmFreeContiguousMemory(nxChunks[i]);
                nxChunks[i] = NULL;
            }
        }
        nxAudioShutdown();
    }
    audioState = false;
}

void AudioDevice::InitAudioChannels() { AudioDeviceBase::InitAudioChannels(); }
