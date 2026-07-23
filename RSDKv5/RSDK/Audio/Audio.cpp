#include "RSDK/Core/RetroEngine.hpp"

using namespace RSDK;

#if RETRO_REV0U
#include "Legacy/AudioLegacy.cpp"
#endif

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis/stb_vorbis.c"

stb_vorbis *vorbisInfo = NULL;
stb_vorbis_alloc vorbisAlloc;

SFXInfo RSDK::sfxList[SFX_COUNT];
ChannelInfo RSDK::channels[CHANNEL_COUNT];

char streamFilePath[0x40];
uint8 *streamBuffer    = NULL;
int32 streamBufferSize = 0;
uint32 streamStartPos  = 0;
int32 streamLoopPoint  = 0;

#define LINEAR_INTERPOLATION_LOOKUP_DIVISOR 0x40 // Determines the 'resolution' of the lookup table.
#define LINEAR_INTERPOLATION_LOOKUP_LENGTH  (TO_FIXED(1) / LINEAR_INTERPOLATION_LOOKUP_DIVISOR)

float linearInterpolationLookup[LINEAR_INTERPOLATION_LOOKUP_LENGTH];

#if RETRO_AUDIODEVICE_XAUDIO
#include "XAudio/XAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_SDL2
#include "SDL2/SDL2AudioDevice.cpp"
#elif RETRO_AUDIODEVICE_SDL3
#include "SDL3/SDL3AudioDevice.cpp"
#elif RETRO_AUDIODEVICE_PORT
#include "PortAudio/PortAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_MINI
#include "MiniAudio/MiniAudioDevice.cpp"
#elif RETRO_AUDIODEVICE_OBOE
#include "Oboe/OboeAudioDevice.cpp"
#endif

uint8 AudioDeviceBase::initializedAudioChannels = false;
uint8 AudioDeviceBase::audioState               = 0;
uint8 AudioDeviceBase::audioFocus               = 0;

void AudioDeviceBase::Release()
{
    // This is missing, meaning that the garbage collector will never reclaim stb_vorbis's buffer.
#if !RETRO_USE_ORIGINAL_CODE
    stb_vorbis_close(vorbisInfo);
    vorbisInfo = NULL;
#endif
}

void AudioDeviceBase::ProcessAudioMixing(void *stream, int32 length)
{
    SAMPLE_FORMAT *streamF    = (SAMPLE_FORMAT *)stream;
    SAMPLE_FORMAT *streamEndF = ((SAMPLE_FORMAT *)stream) + length;

    memset(stream, 0, length * sizeof(SAMPLE_FORMAT));

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        ChannelInfo *channel = &channels[c];

        switch (channel->state) {
            default:
            case CHANNEL_IDLE: break;

            case CHANNEL_SFX: {
#ifdef RETRO_SFX_USE_S16
                // SFX samples are stored as S16 (see LoadSfxToSlot); interpolate in
                // integer space and scale to float once per output sample
                const int16 *sfxBuffer = (const int16 *)channel->samplePtr + channel->bufferPos;
#else
                SAMPLE_FORMAT *sfxBuffer = &channel->samplePtr[channel->bufferPos];
#endif

                float volL = channel->volume, volR = channel->volume;
                if (channel->pan < 0.0f)
                    volR = (1.0f + channel->pan) * channel->volume;
                else
                    volL = (1.0f - channel->pan) * channel->volume;

                float panL = volL * engine.soundFXVolume;
                float panR = volR * engine.soundFXVolume;

                uint32 speedPercent       = 0;
                SAMPLE_FORMAT *curStreamF = streamF;
                while (curStreamF < streamEndF && streamF < streamEndF) {
                    // Perform linear interpolation.
                    SAMPLE_FORMAT sample;
#if !RETRO_USE_ORIGINAL_CODE
                    if (!sfxBuffer) // PROTECTION FOR v5U (and other mysterious crashes 👻)
                        sample = 0;
                    else
#endif
#ifdef RETRO_SFX_USE_S16
                        sample = ((float)(sfxBuffer[1] - sfxBuffer[0]) * linearInterpolationLookup[speedPercent / LINEAR_INTERPOLATION_LOOKUP_DIVISOR]
                                  + (float)sfxBuffer[0])
                                 * (1.0f / 32768.0f);
#else
                    sample =
                        (sfxBuffer[1] - sfxBuffer[0]) * linearInterpolationLookup[speedPercent / LINEAR_INTERPOLATION_LOOKUP_DIVISOR] + sfxBuffer[0];
#endif

                    speedPercent += channel->speed;
                    sfxBuffer += FROM_FIXED(speedPercent);
                    channel->bufferPos += FROM_FIXED(speedPercent);
                    speedPercent %= TO_FIXED(1);

                    curStreamF[0] += sample * panL;
                    curStreamF[1] += sample * panR;
                    curStreamF += 2;

                    if (channel->bufferPos >= channel->sampleLength) {
                        if (channel->loop == (uint32)-1) {
                            channel->state   = CHANNEL_IDLE;
                            channel->soundID = -1;
                            break;
                        }
                        else {
                            channel->bufferPos -= (uint32)channel->sampleLength;
                            channel->bufferPos += channel->loop;

#ifdef RETRO_SFX_USE_S16
                            sfxBuffer = (const int16 *)channel->samplePtr + channel->bufferPos;
#else
                            sfxBuffer = &channel->samplePtr[channel->bufferPos];
#endif
                        }
                    }
                }

                break;
            }

            case CHANNEL_STREAM: {
                SAMPLE_FORMAT *streamBuffer = &channel->samplePtr[channel->bufferPos];

                float volL = channel->volume, volR = channel->volume;
                if (channel->pan < 0.0f)
                    volR = (1.0f + channel->pan) * channel->volume;
                else
                    volL = (1.0f - channel->pan) * channel->volume;

                float panL = volL * engine.streamVolume;
                float panR = volR * engine.streamVolume;

                uint32 speedPercent       = 0;
                SAMPLE_FORMAT *curStreamF = streamF;
                while (curStreamF < streamEndF && streamF < streamEndF) {
                    speedPercent += channel->speed;
                    int32 next = FROM_FIXED(speedPercent);
                    speedPercent %= TO_FIXED(1);

                    curStreamF[0] += streamBuffer[0] * panL;
                    curStreamF[1] += streamBuffer[1] * panR;
                    curStreamF += 2;

                    streamBuffer += next * 2;
                    channel->bufferPos += next * 2;

                    if (channel->bufferPos >= channel->sampleLength) {
                        channel->bufferPos -= (uint32)channel->sampleLength;

                        streamBuffer = &channel->samplePtr[channel->bufferPos];

                        UpdateStreamBuffer(channel);
                    }
                }
                break;
            }

            case CHANNEL_LOADING_STREAM: break;
        }
    }
}

void AudioDeviceBase::InitAudioChannels()
{
    for (int32 i = 0; i < CHANNEL_COUNT; ++i) {
        channels[i].soundID = -1;
        channels[i].state   = CHANNEL_IDLE;
    }

    // Compute a lookup table of floating-point linear interpolation delta scales,
    // to speed-up the process of converting from fixed-point to floating-point.
    for (int32 i = 0; i < LINEAR_INTERPOLATION_LOOKUP_LENGTH; ++i) linearInterpolationLookup[i] = i / (float)LINEAR_INTERPOLATION_LOOKUP_LENGTH;

    GEN_HASH_MD5("Stream Channel 0", sfxList[SFX_COUNT - 1].hash);
    sfxList[SFX_COUNT - 1].scope              = SCOPE_GLOBAL;
    sfxList[SFX_COUNT - 1].maxConcurrentPlays = 1;
    sfxList[SFX_COUNT - 1].length             = MIX_BUFFER_SIZE;
    AllocateStorage((void **)&sfxList[SFX_COUNT - 1].buffer, MIX_BUFFER_SIZE * sizeof(SAMPLE_FORMAT), DATASET_MUS, false);

#if RETRO_PLATFORM == RETRO_XBOX
    // Allocate the vorbis work buffer at boot so the MUS pool layout is
    // permanently [mix][vorbis][track]: track changes then free/alloc only the
    // LAST block, and pool compaction reclaims it without ever moving live
    // memory (stb_vorbis and the loader thread hold raw pointers into it)
    vorbisAlloc.alloc_buffer_length_in_bytes = 512 * 1024;
    AllocateStorage((void **)&vorbisAlloc.alloc_buffer, 512 * 1024, DATASET_MUS, false);
#endif

    initializedAudioChannels = true;
}

void RSDK::UpdateStreamBuffer(ChannelInfo *channel)
{
    int32 bufferRemaining = MIX_BUFFER_SIZE;
    float *buffer         = channel->samplePtr;

    for (int32 s = 0; s < MIX_BUFFER_SIZE;) {
        int32 samples = stb_vorbis_get_samples_float_interleaved(vorbisInfo, 2, buffer, bufferRemaining) * 2;
        if (!samples) {
            if (channel->loop == 1 && stb_vorbis_seek_frame(vorbisInfo, streamLoopPoint)) {
                // we're looping & the seek was successful, get more samples
            }
            else {
                channel->state   = CHANNEL_IDLE;
                channel->soundID = -1;
                memset(buffer, 0, sizeof(float) * bufferRemaining);

                break;
            }
        }

        s += samples;
        buffer += samples;
        bufferRemaining = MIX_BUFFER_SIZE - s;
    }

    for (int32 i = 0; i < MIX_BUFFER_SIZE; ++i) channel->samplePtr[i] *= 0.5f;
}

void RSDK::LoadStream(ChannelInfo *channel)
{
    if (channel->state != CHANNEL_LOADING_STREAM)
        return;

    stb_vorbis_close(vorbisInfo);

    // Free the previous track's buffer to avoid leaking MUS pool memory.
    // vorbisAlloc is deliberately persistent (allocated once below): freeing it
    // per track churned the pool layout and forced defrag compaction, which
    // moved live buffers under stb_vorbis and the loader thread.
    if (streamBuffer) {
        RemoveStorageEntry((void **)&streamBuffer);
        streamBuffer = NULL;
    }

    FileInfo info;
    InitFileInfo(&info);

    if (LoadFile(&info, streamFilePath, FMODE_RB)) {
        streamBufferSize = info.fileSize;
        AllocateStorage((void **)&streamBuffer, info.fileSize, DATASET_MUS, false);
        if (!streamBuffer) {
            CloseFile(&info);
            channel->state = CHANNEL_IDLE;
            return;
        }
        ReadBytes(&info, streamBuffer, streamBufferSize);
        CloseFile(&info);

        if (streamBufferSize > 0) {
            vorbisAlloc.alloc_buffer_length_in_bytes = 512 * 1024; // 512KiB
            if (!vorbisAlloc.alloc_buffer)                         // persistent: allocated once, reused for every track
                AllocateStorage((void **)&vorbisAlloc.alloc_buffer, 512 * 1024, DATASET_MUS, false);

            if (vorbisAlloc.alloc_buffer) {
                vorbisInfo = stb_vorbis_open_memory(streamBuffer, streamBufferSize, NULL, &vorbisAlloc);
                if (vorbisInfo) {
                    if (streamStartPos)
                        stb_vorbis_seek(vorbisInfo, streamStartPos);
                    UpdateStreamBuffer(channel);

                    channel->state = CHANNEL_STREAM;
                }
            }
        }
    }

    if (channel->state == CHANNEL_LOADING_STREAM)
        channel->state = CHANNEL_IDLE;
}

int32 RSDK::PlayStream(const char *filename, uint32 slot, uint32 startPos, uint32 loopPoint, bool32 loadASync)
{
    if (!engine.streamsEnabled)
        return -1;

    if (slot >= CHANNEL_COUNT) {
        for (int32 c = 0; c < CHANNEL_COUNT && slot >= CHANNEL_COUNT; ++c) {
            if (channels[c].soundID == -1 && channels[c].state != CHANNEL_LOADING_STREAM) {
                slot = c;
            }
        }

        // as a last resort, run through all channels
        // pick the channel closest to being finished
        if (slot >= CHANNEL_COUNT) {
            uint32 len = 0xFFFFFFFF;
            for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
                if (channels[c].sampleLength < len && channels[c].state != CHANNEL_LOADING_STREAM) {
                    slot = c;
                    len  = (uint32)channels[c].sampleLength;
                }
            }
        }
    }

    if (slot >= CHANNEL_COUNT)
        return -1;

    ChannelInfo *channel = &channels[slot];

    LockAudioDevice();

    channel->soundID      = 0xFF;
    channel->loop         = loopPoint != 0;
    channel->priority     = 0xFF;
    channel->state        = CHANNEL_LOADING_STREAM;
    channel->pan          = 0.0f;
    channel->volume       = 1.0f;
    channel->sampleLength = sfxList[SFX_COUNT - 1].length;
    channel->samplePtr    = sfxList[SFX_COUNT - 1].buffer;
    channel->bufferPos    = 0;
    channel->speed        = TO_FIXED(1);

    sprintf_s(streamFilePath, sizeof(streamFilePath), "Data/Music/%s", filename);
    streamStartPos  = startPos;
    streamLoopPoint = loopPoint;

    AudioDevice::HandleStreamLoad(channel, loadASync);

    UnlockAudioDevice();

    return slot;
}

#define WAV_SIG_HEADER (0x46464952) // RIFF
#define WAV_SIG_DATA   (0x61746164) // data

void RSDK::LoadSfxToSlot(char *filename, uint8 slot, uint8 plays, uint8 scope)
{
    FileInfo info;
    InitFileInfo(&info);

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/SoundFX/%s", filename);

    RETRO_HASH_MD5(hash);
#if RETRO_PLATFORM == RETRO_XBOX
    // This runs on the async loader thread: GEN_HASH_MD5 goes through the global
    // textBuffer (documented not thread-safe) and would race the main thread —
    // hash via a local scratch instead
    char hashScratch[0x100];
    strncpy(hashScratch, filename, sizeof(hashScratch) - 1);
    hashScratch[sizeof(hashScratch) - 1] = 0;
    GEN_HASH_MD5_BUFFER(hashScratch, hash);
#else
    GEN_HASH_MD5(filename, hash);
#endif

    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
#if RETRO_PLATFORM == RETRO_XBOX
#endif
        HASH_COPY_MD5(sfxList[slot].hash, hash);
        sfxList[slot].scope              = scope;
        sfxList[slot].maxConcurrentPlays = plays;

        uint8 type = fullFilePath[strlen(fullFilePath) - 1];
        if (type == 'v' || type == 'V') { // A very loose way of checking that we're trying to load a '.wav' file.
            uint32 signature = ReadInt32(&info, false);

            if (signature == WAV_SIG_HEADER) {
                ReadInt32(&info, false); // chunk size
                ReadInt32(&info, false); // WAVE
                ReadInt32(&info, false); // FMT
#if !RETRO_USE_ORIGINAL_CODE
                int32 chunkSize = ReadInt32(&info, false); // chunk size
#else
                ReadInt32(&info, false); // chunk size
#endif
                ReadInt16(&info);        // audio format
                ReadInt16(&info);        // channels
                ReadInt32(&info, false); // sample rate
                ReadInt32(&info, false); // bytes per sec
                ReadInt16(&info);        // block align
                ReadInt16(&info);        // format

                Seek_Set(&info, 34);
                uint16 sampleBits = ReadInt16(&info);

#if !RETRO_USE_ORIGINAL_CODE
                // Original code added to help fix some issues
                Seek_Set(&info, 20 + chunkSize);
#endif

                // Find the data header
                int32 loop = 0;
                while (true) {
                    signature = ReadInt32(&info, false);
                    if (signature == WAV_SIG_DATA)
                        break;

                    loop += 4;
                    if (loop >= 0x40) {
                        if (loop != 0x100) {
                            CloseFile(&info);
                            // There's a bug here: `sfxList[id].scope` is not reset to `SCOPE_NONE`,
                            // meaning that the game will consider the SFX valid and allow it to be played.
                            // This can cause a crash because the SFX is incomplete.
#if !RETRO_USE_ORIGINAL_CODE
                            PrintLog(PRINT_ERROR, "Unable to read sfx: %s", filename);
#endif
                            return;
                        }
                        else {
                            break;
                        }
                    }
                }

                uint32 length = ReadInt32(&info, false);
                if (sampleBits == 16)
                    length /= 2;

#ifdef RETRO_SFX_USE_S16
                // Store SFX as S16 (native wav size) instead of F32: halves the SFX pool
                // footprint and the mixer's memory traffic. The buffer field stays float*
                // for engine compatibility; the mixer's CHANNEL_SFX branch casts.
                // length is published AFTER the samples are converted (below) so the
                // async loader never exposes a half-filled buffer to PlaySfx.
                AllocateStorage((void **)&sfxList[slot].buffer, sizeof(int16) * length, DATASET_SFX, false);
#else
                AllocateStorage((void **)&sfxList[slot].buffer, sizeof(float) * length, DATASET_SFX, false);
                sfxList[slot].length = length;
#endif

#if !RETRO_USE_ORIGINAL_CODE
                // The SFX pool can run out (it is much smaller on Xbox than the 32MB PC
                // default); without this guard the conversion below writes through a
                // NULL pointer and crashes the console
                if (!sfxList[slot].buffer) {
                    PrintLog(PRINT_ERROR, "Unable to allocate sfx buffer (%u samples): %s", length, filename);
                    sfxList[slot].scope  = SCOPE_NONE;
                    sfxList[slot].length = 0;
                    CloseFile(&info);
                    return;
                }
#endif

#ifdef RETRO_SFX_USE_S16
                // Convert the sample data to S16 (16-bit keeps the engine's 0.75 sfx
                // attenuation; 8-bit is stored plain, matching the F32 path).
                // Bulk-read the whole data chunk in ONE ReadBytes: per-sample ReadInt16
                // was a lock+seek+2-byte-fread per sample — tens of thousands of
                // syscalls per wav, minutes of loading on real disc hardware.
                int16 *buffer = (int16 *)sfxList[slot].buffer;
                if (sampleBits == 8) {
                    // Read the raw U8 samples into the upper half, then expand in
                    // place to S16. Iterate FORWARD: the write of buffer[s] (bytes
                    // 2s,2s+1) must not clobber raw[s'] (byte length+s') for any
                    // s' still unread — forward keeps 2s+1 < length+s+1 for all
                    // s<length-1, and raw[s] is read before its own write. (Backward
                    // corrupts the upper half, which garbled 8-bit sfx like SSExit.)
                    uint8 *raw = (uint8 *)buffer + length;
                    ReadBytes(&info, raw, length);
                    for (int32 s = 0; s < (int32)length; ++s) buffer[s] = (int16)((raw[s] - 0x80) << 8);
                }
                else {
                    ReadBytes(&info, buffer, length * sizeof(int16));
                    for (int32 s = 0; s < (int32)length; ++s) buffer[s] = (int16)((buffer[s] * 3) >> 2);
                }

                SDL_CompilerBarrier(); // samples first, then length: PlaySfx sees complete data only
                sfxList[slot].length = length;
#else
                // Convert the sample data to F32 format
                float *buffer = (float *)sfxList[slot].buffer;
                if (sampleBits == 8) {
                    // 8-bit sample. Convert from U8 to S8, and then from S8 to F32.
                    for (int32 s = 0; s < length; ++s) {
                        int32 sample = ReadInt8(&info);
                        *buffer++    = (sample - 0x80) / (float)0x80;
                    }
                }
                else {
                    // 16-bit sample. Convert from S16 to F32.
                    for (int32 s = 0; s < length; ++s) {
                        // For some reason, the game performs sign-extension manually here.
                        // Note that this is different from the 8-bit format's unsigned-to-signed conversion.
                        int32 sample = (uint16)ReadInt16(&info);

                        if (sample > 0x7FFF)
                            sample = (sample & 0x7FFF) - 0x8000;

                        *buffer++ = (sample / (float)0x8000) * 0.75f;
                    }
                }
#endif
            }
#if !RETRO_USE_ORIGINAL_CODE
            else {
                PrintLog(PRINT_ERROR, "Invalid header in sfx: %s", filename);
            }
#endif
        }
#if !RETRO_USE_ORIGINAL_CODE
        else {
            // what the
            PrintLog(PRINT_ERROR, "Could not find header in sfx: %s", filename);
        }
#endif
    }
#if !RETRO_USE_ORIGINAL_CODE
    else {
#if RETRO_PLATFORM == RETRO_XBOX
#endif
        PrintLog(PRINT_ERROR, "Unable to open sfx: %s", filename);
    }
#endif

    CloseFile(&info);
}

#if RETRO_PLATFORM == RETRO_XBOX
// ---- Async music loader -----------------------------------------------------
// Music OGGs are multi-MB reads that would freeze the screen if loaded on the
// main thread (PlayStream fires at stage start); this worker loads them in the
// background (CHANNEL_LOADING_STREAM reserves the channel meanwhile). SFX are
// loaded synchronously — bulk reads made them cheap, and deferring them caused
// audible trigger delays. Each job carries its own path/start/loop because the
// streamFilePath globals can be overwritten by a newer PlayStream before the
// worker gets to an older job.
// Thread safety: pack reads are seek+read-atomic (Reader.hpp packReadCS) and
// the pool allocator is guarded (Storage.cpp storageCS).

#include <xboxkrnl/xboxkrnl.h>

struct StreamLoadJob {
    ChannelInfo *channel;
    uint32 startPos;
    int32 loopPoint;
    char path[0x40];
};

#define STREAM_LOAD_QUEUE_SIZE (8)
static StreamLoadJob streamLoadQueue[STREAM_LOAD_QUEUE_SIZE];
static int32 streamLoadHead = 0; // guarded by streamLoadCS
static int32 streamLoadTail = 0;
static RTL_CRITICAL_SECTION streamLoadCS; // kernel CS: SDL mutexes are unreliable pre-SDL_Init on nxdk
static bool32 streamLoadCSInit      = false;
static SDL_Thread *streamLoadThread = NULL;

static int32 StreamLoaderProc(void *unused)
{
    (void)unused;

    while (true) {
        StreamLoadJob job;
        bool32 hasJob = false;

        RtlEnterCriticalSection(&streamLoadCS);
        if (streamLoadTail != streamLoadHead) {
            job            = streamLoadQueue[streamLoadTail];
            streamLoadTail = (streamLoadTail + 1) % STREAM_LOAD_QUEUE_SIZE;
            hasJob         = true;
        }
        RtlLeaveCriticalSection(&streamLoadCS);

        if (!hasJob) {
            SDL_Delay(5);
            continue;
        }

        // Restore the stream globals from the job — this worker is the only
        // consumer, so per-job values can't be clobbered by a newer PlayStream
        strcpy(streamFilePath, job.path);
        streamStartPos  = job.startPos;
        streamLoopPoint = job.loopPoint;
        PrintLog(PRINT_NORMAL, "streamtrace: load start %s (chState=%d)", job.path, job.channel->state);
        LoadStream(job.channel);
        PrintLog(PRINT_NORMAL, "streamtrace: load done %s (chState=%d)", job.path, job.channel->state);
    }

    return 0;
}

bool32 RSDK::EnqueueStreamLoad(ChannelInfo *channel)
{
    if (!streamLoadCSInit) { // first enqueue is on the main thread, pre-worker
        RtlInitializeCriticalSection(&streamLoadCS);
        streamLoadCSInit = true;
    }

    if (!streamLoadThread)
        streamLoadThread = SDL_CreateThread(StreamLoaderProc, "StreamLoader", NULL);
    if (!streamLoadThread) {
        PrintLog(PRINT_NORMAL, "streamtrace: NO THREAD for %s -> sync fallback", streamFilePath);
        return false;
    }

    StreamLoadJob job = {};
    job.channel       = channel;
    job.startPos      = streamStartPos;
    job.loopPoint     = streamLoopPoint;
    strncpy(job.path, streamFilePath, sizeof(job.path) - 1);

    bool32 queued = false;
    RtlEnterCriticalSection(&streamLoadCS);
    int32 next = (streamLoadHead + 1) % STREAM_LOAD_QUEUE_SIZE;
    if (next != streamLoadTail) {
        streamLoadQueue[streamLoadHead] = job;
        streamLoadHead                  = next;
        queued                          = true;
    }
    RtlLeaveCriticalSection(&streamLoadCS);

    PrintLog(PRINT_NORMAL, "streamtrace: enqueue %s queued=%d", streamFilePath, queued);
    return queued; // full queue -> caller loads synchronously
}
#endif

void RSDK::LoadSfx(char *filename, uint8 plays, uint8 scope)
{
#if RETRO_PLATFORM == RETRO_XBOX
    // Dedupe: stage sfx lists can re-list files that are already loaded (e.g. as
    // globals); a duplicate would waste a slot and pool space. Keep the widest
    // scope so a global re-listed by a stage isn't cleared on stage unload.
    {
        RETRO_HASH_MD5(hash);
        GEN_HASH_MD5(filename, hash);
        for (uint32 i = 0; i < SFX_COUNT; ++i) {
            if (sfxList[i].scope != SCOPE_NONE && HASH_MATCH_MD5(sfxList[i].hash, hash)) {
                if (scope == SCOPE_GLOBAL)
                    sfxList[i].scope = SCOPE_GLOBAL;
                return;
            }
        }
    }
#endif

    // Find an empty sound slot.
    uint16 id = -1;
    for (uint32 i = 0; i < SFX_COUNT; ++i) {
        if (sfxList[i].scope == SCOPE_NONE) {
            id = i;
            break;
        }
    }

    if (id == (uint16)-1)
        return;

    // Synchronous: bulk reads make sfx loads cheap, and deferring them to the
    // loader caused audible trigger delays on hardware
    LoadSfxToSlot(filename, (uint8)id, plays, scope);
}

#if RETRO_PLATFORM == RETRO_XBOX
// Temporary diagnosis for missing SFX on hardware: trace every PlaySfx decision
// and inaudible-channel anomalies over serial. Toggle off once diagnosed.
#define SFX_TRACE 1

// Allow at most ~30 trace lines per second so serial stays readable
static bool32 SfxTraceAllowed()
{
    static uint32 windowStart = 0;
    static uint32 lines       = 0;

    uint32 now = (uint32)SDL_GetTicks();
    if (now - windowStart >= 1000) {
        windowStart = now;
        lines       = 0;
    }
    return ++lines <= 30;
}
#endif

int32 RSDK::PlaySfx(uint16 sfx, uint32 loopPoint, uint32 priority)
{
#if SFX_TRACE
    if (sfx >= SFX_COUNT || !sfxList[sfx].scope) {
        if (SfxTraceAllowed())
            PrintLog(PRINT_NORMAL, "sfxtrace: DROP id=%u (scope=%d, count=%d) not loaded", sfx, sfx < SFX_COUNT ? sfxList[sfx].scope : -1, SFX_COUNT);
        return -1;
    }
#else
    if (sfx >= SFX_COUNT || !sfxList[sfx].scope)
        return -1;
#endif

    uint8 count = 0;
    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].soundID == sfx)
            ++count;
    }

    int8 slot = -1;
    // if we've hit the max, replace the oldest one
    if (count >= sfxList[sfx].maxConcurrentPlays) {
        int32 highestStackID = 0;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            int32 stackID = sfxList[sfx].playCount - channels[c].playIndex;
            if (stackID > highestStackID && channels[c].soundID == sfx) {
                slot           = c;
                highestStackID = stackID;
            }
        }
    }

    // if we don't have a slot yet, try to pick any channel that's not currently playing
    for (int32 c = 0; c < CHANNEL_COUNT && slot < 0; ++c) {
        if (channels[c].soundID == -1 && channels[c].state != CHANNEL_LOADING_STREAM) {
            slot = c;
        }
    }

    // as a last resort, run through all channels
    // pick the channel closest to being finished AND with lower priority
    if (slot < 0) {
        uint32 len = 0xFFFFFFFF;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            if (channels[c].sampleLength < len && priority > channels[c].priority && channels[c].state != CHANNEL_LOADING_STREAM) {
                slot = c;
                len  = (uint32)channels[c].sampleLength;
            }
        }
    }

    if (slot == -1) {
#if SFX_TRACE
        if (SfxTraceAllowed()) {
            PrintLog(PRINT_NORMAL, "sfxtrace: DROP id=%u no channel (prio=%u)", sfx, priority);
            // Dump the channel table so it's obvious what is hogging the slots
            for (int32 c = 0; c < CHANNEL_COUNT; ++c)
                PrintLog(PRINT_NORMAL, "sfxtrace:   ch%02d id=%d state=%d loop=%d prio=%u pos=%u/%u", c, channels[c].soundID, channels[c].state,
                         (int32)channels[c].loop, channels[c].priority, (uint32)channels[c].bufferPos, (uint32)channels[c].sampleLength);
        }
#endif
        return -1;
    }

#if SFX_TRACE
    if (SfxTraceAllowed())
        PrintLog(PRINT_NORMAL, "sfxtrace: play id=%u slot=%d len=%u loop=%u prio=%u", sfx, slot, sfxList[sfx].length, loopPoint, priority);
#endif

    LockAudioDevice();

    channels[slot].state        = CHANNEL_SFX;
    channels[slot].bufferPos    = 0;
    channels[slot].samplePtr    = sfxList[sfx].buffer;
    channels[slot].sampleLength = sfxList[sfx].length;
    channels[slot].volume       = 1.0f;
    channels[slot].pan          = 0.0f;
    channels[slot].speed        = TO_FIXED(1);
    channels[slot].soundID      = sfx;
    if (loopPoint >= 2)
        channels[slot].loop = loopPoint;
    else
        channels[slot].loop = loopPoint - 1;
    channels[slot].priority  = priority;
    channels[slot].playIndex = sfxList[sfx].playCount++;

    UnlockAudioDevice();

    return slot;
}

void RSDK::SetChannelAttributes(uint8 channel, float volume, float panning, float speed)
{
    if (channel < CHANNEL_COUNT) {
#if SFX_TRACE
        // Log only "inaudible" anomalies: silent volume, stopped/absurd speed, hard pan
        if ((volume <= 0.01f || speed == 0.0f || speed > 4.0f || panning <= -0.99f || panning >= 0.99f) && SfxTraceAllowed())
            PrintLog(PRINT_NORMAL, "sfxtrace: attr ch=%u id=%d vol=%d.%02d pan=%d.%02d spd=%d.%02d", channel, channels[channel].soundID,
                     (int32)volume, (int32)(fabsf(volume) * 100) % 100, (int32)panning, (int32)(fabsf(panning) * 100) % 100, (int32)speed,
                     (int32)(fabsf(speed) * 100) % 100);
#endif
        volume                   = fminf(4.0f, volume);
        volume                   = fmaxf(0.0f, volume);
        channels[channel].volume = volume;

        panning               = fminf(1.0f, panning);
        panning               = fmaxf(-1.0f, panning);
        channels[channel].pan = panning;

        if (speed > 0.0f)
            channels[channel].speed = (int32)(speed * TO_FIXED(1));
        else if (speed == 1.0f)
            channels[channel].speed = TO_FIXED(1);
    }
}

uint32 RSDK::GetChannelPos(uint32 channel)
{
    if (channel >= CHANNEL_COUNT)
        return 0;

    if (channels[channel].state == CHANNEL_SFX)
        return channels[channel].bufferPos;

    if (channels[channel].state == CHANNEL_STREAM) {
        if (!vorbisInfo->current_loc_valid || vorbisInfo->current_loc < 0)
            return 0;

        return vorbisInfo->current_loc;
    }

    return 0;
}

double RSDK::GetVideoStreamPos()
{
    if (channels[0].state == CHANNEL_STREAM && AudioDevice::audioState && AudioDevice::initializedAudioChannels && vorbisInfo->current_loc_valid) {
        return vorbisInfo->current_loc / (double)AUDIO_FREQUENCY;
    }

    return -1.0;
}

void RSDK::ClearStageSfx()
{
    LockAudioDevice();

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].state == CHANNEL_SFX || channels[c].state == (CHANNEL_SFX | CHANNEL_PAUSED)) {
            channels[c].soundID = -1;
            channels[c].state   = CHANNEL_IDLE;
        }
    }

    // Unload stage SFX
    for (int32 s = 0; s < SFX_COUNT; ++s) {
        if (sfxList[s].scope >= SCOPE_STAGE) {
            MEM_ZERO(sfxList[s]);
            sfxList[s].scope = SCOPE_NONE;
        }
    }

    UnlockAudioDevice();
}

#if RETRO_USE_MOD_LOADER
void RSDK::ClearGlobalSfx()
{
    LockAudioDevice();

    for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
        if (channels[c].state == CHANNEL_SFX || channels[c].state == (CHANNEL_SFX | CHANNEL_PAUSED)) {
            channels[c].soundID = -1;
            channels[c].state   = CHANNEL_IDLE;
        }
    }

    // Unload global SFX
    for (int32 s = 0; s < SFX_COUNT; ++s) {
        // clear global sfx (do NOT clear the stream channel 0 slot)
        if (sfxList[s].scope == SCOPE_GLOBAL && s != SFX_COUNT - 1) {
            MEM_ZERO(sfxList[s]);
            sfxList[s].scope = SCOPE_NONE;
        }
    }

    UnlockAudioDevice();
}
#endif
