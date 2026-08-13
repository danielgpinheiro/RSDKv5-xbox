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

#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
// Per-SFX storage format for the APU backend: >0 = the buffer holds that many bytes of
// Xbox ADPCM (loose D:\SoundFXAD\ file); 0 = plain S16 PCM (pack fallback). Declared
// before the NXAudioDevice.cpp include below so SubmitSfx can read it (same TU).
uint32 sfxADPCMSize[SFX_COUNT] = { 0 };
#endif

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
#elif RETRO_AUDIODEVICE_NXAUDIO
#include "NXAudio/NXAudioDevice.cpp"
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
    // Allocate the vorbis work buffer once at boot so the MUS pool layout is
    // permanently [mix][vorbis][track]: track changes free/alloc only the LAST
    // block, so compaction reclaims it without moving the live streamBuffer that
    // stb_vorbis and the audio thread hold pointers into.
    vorbisAlloc.alloc_buffer_length_in_bytes = 512 * 1024;
    AllocateStorage((void **)&vorbisAlloc.alloc_buffer, 512 * 1024, DATASET_MUS, false);
#endif

    initializedAudioChannels = true;
}

#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
// --- Loose pre-converted PCM music streaming (drops CPU Vorbis) -----------------
// Music is shipped as headerless raw 22050 Hz / 8-bit unsigned / stereo interleaved
// PCM at D:\MusicPCM\<name>.pcm (converted offline by tools/oggpcm.sh — the Dreamcast
// approach). We stream it straight from disc — no whole-file load into DATASET_MUS,
// no 512KB stb_vorbis work buffer — and 2x-upsample into the 44100 Hz float ring the
// APU music voice already consumes. Audible only in xemu (hardware audio is silent),
// but the CPU (no Vorbis decode) and MUS-pool savings are real on hardware. Falls
// back to the packed OGG + Vorbis path when the loose file is absent.
static bool32 streamIsLoosePCM = false;
static FileInfo pcmStreamFile;
static int32 pcmDataSize    = 0; // total PCM bytes in the loose file
static uint32 pcmPlayPos44k = 0; // playback position in 44100 Hz stereo frames

static bool32 LoadStreamLoosePCM(ChannelInfo *channel)
{
    // streamFilePath is "Data/Music/<name>.<ext>" -> "D:\MusicPCM\<name>.pcm".
    const char *name = streamFilePath;
    for (const char *p = streamFilePath; *p; ++p)
        if (*p == '/' || *p == '\\')
            name = p + 1;

    char base[0x40];
    int32 i = 0;
    for (; name[i] && i < (int32)sizeof(base) - 1; ++i) base[i] = name[i];
    base[i] = '\0';
    for (int32 j = i - 1; j >= 0; --j) {
        if (base[j] == '.') {
            base[j] = '\0';
            break;
        }
    }

    char loosePath[0x80];
    sprintf_s(loosePath, sizeof(loosePath), "D:\\MusicPCM\\%s.pcm", base);

    InitFileInfo(&pcmStreamFile);
    pcmStreamFile.externalFile = true; // plain fOpen of D:\MusicPCM\...; never the data pack
    if (!LoadFile(&pcmStreamFile, loosePath, FMODE_RB))
        return false;

    pcmDataSize      = pcmStreamFile.fileSize;
    streamIsLoosePCM = true;

    // streamStartPos / streamLoopPoint are 44100 Hz per-channel sample indices (the
    // stb_vorbis units). The PCM is 22050 Hz 8-bit stereo = 2 bytes per stereo frame,
    // so 44100 sample index N -> 22050 frame N/2 -> byte offset (N/2)*2 (which equals
    // the 44100-frame index, the unit pcmPlayPos44k tracks).
    uint32 startByte = (streamStartPos / 2) * 2;
    if (startByte > (uint32)pcmDataSize)
        startByte = 0;
    Seek_Set(&pcmStreamFile, (int32)startByte);
    pcmPlayPos44k = startByte;

    channel->state = CHANNEL_STREAM;
    UpdateStreamBuffer(channel); // dispatches to the loose variant below (streamIsLoosePCM)
    return true;
}

static void UpdateStreamBufferLoosePCM(ChannelInfo *channel)
{
    float *out            = channel->samplePtr;
    const int32 outFrames = MIX_BUFFER_SIZE / 2; // 44100 Hz stereo frames to produce
    const int32 srcFrames = outFrames / 2;       // 22050 Hz source frames (2x upsample)
    const int32 needBytes = srcFrames * 2;       // 8-bit stereo -> 2 bytes / frame

    static uint8 src[(MIX_BUFFER_SIZE / 4) * 2];
    int32 have = 0;
    while (have < needBytes) {
        int32 got = (int32)ReadBytes(&pcmStreamFile, src + have, needBytes - have);
        if (got <= 0) {
            if (channel->loop) {
                uint32 loopByte = (streamLoopPoint / 2) * 2;
                if (loopByte >= (uint32)pcmDataSize)
                    loopByte = 0;
                Seek_Set(&pcmStreamFile, (int32)loopByte);
                pcmPlayPos44k = loopByte;
                continue; // read the remainder from the loop point
            }

            // End of a non-looping track: pad with silence (0x80), idle, close the file.
            memset(src + have, 0x80, needBytes - have);
            channel->state   = CHANNEL_IDLE;
            channel->soundID = -1;
            CloseFile(&pcmStreamFile);
            streamIsLoosePCM = false;
            have             = needBytes;
            break;
        }
        have += got;
    }

    // u8 [0,255] (128 = silence) -> float, halved to match the Vorbis path's 0.5
    // attenuation, then linearly upsampled 22050 -> 44100 (one interpolated frame
    // between each source pair; the final source frame is held).
    const float scale = 0.5f / 128.0f;
    for (int32 n = 0; n < srcFrames; ++n) {
        float l0 = ((float)src[n * 2 + 0] - 128.0f) * scale;
        float r0 = ((float)src[n * 2 + 1] - 128.0f) * scale;
        float l1 = l0, r1 = r0;
        if (n + 1 < srcFrames) {
            l1 = ((float)src[(n + 1) * 2 + 0] - 128.0f) * scale;
            r1 = ((float)src[(n + 1) * 2 + 1] - 128.0f) * scale;
        }
        int32 o    = n * 4;
        out[o + 0] = l0;
        out[o + 1] = r0;
        out[o + 2] = (l0 + l1) * 0.5f;
        out[o + 3] = (r0 + r1) * 0.5f;
    }

    pcmPlayPos44k += (uint32)outFrames;
}
#endif

void RSDK::UpdateStreamBuffer(ChannelInfo *channel)
{
#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
    if (streamIsLoosePCM) {
        UpdateStreamBufferLoosePCM(channel);
        return;
    }
#endif

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
    // vorbisAlloc.alloc_buffer is deliberately persistent (allocated once in
    // InitAudioChannels): freeing it per track churned the pool layout and
    // forced compaction, which moved the live streamBuffer under the audio
    // thread. Keeping [mix][vorbis][track] means only the last block changes.
    if (streamBuffer) {
        RemoveStorageEntry((void **)&streamBuffer);
        streamBuffer = NULL;
    }

#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
    // Prefer the loose pre-converted PCM (no Vorbis decode, streamed from disc);
    // fall back to the packed OGG below when the loose file is absent.
    if (streamIsLoosePCM) {
        CloseFile(&pcmStreamFile);
        streamIsLoosePCM = false;
    }
    if (LoadStreamLoosePCM(channel))
        return;
#endif

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
            if (!vorbisAlloc.alloc_buffer)                         // persistent: allocated once at boot, reused for every track
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

#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
// Load a loose Xbox-ADPCM SFX from D:\SoundFXAD\<filename> (converted offline; ~1/4 the
// RAM of S16 PCM). Walks the RIFF chunks generically (the ADPCM fmt chunk is larger and
// has a fact chunk, so the pack loader's fixed offsets don't fit). Returns true when the
// slot is fully populated as ADPCM; false -> caller falls back to the pack PCM WAV.
static bool32 LoadSfxADPCMLoose(const char *filename, uint8 slot, uint8 plays, uint8 scope, uint32 *hash)
{
    char fn[0x60];
    int32 n = 0;
    for (; filename[n] && n < (int32)sizeof(fn) - 1; ++n) fn[n] = (filename[n] == '/') ? '\\' : filename[n];
    fn[n] = '\0';

    char loosePath[0x80];
    sprintf_s(loosePath, sizeof(loosePath), "D:\\SoundFXAD\\%s", fn);

    FileInfo info;
    InitFileInfo(&info);
    info.externalFile = true; // plain fOpen of the absolute path, never the data pack (cf. Video.cpp)
    if (!LoadFile(&info, loosePath, FMODE_RB))
        return false;

    if (ReadInt32(&info, false) != WAV_SIG_HEADER) { // 'RIFF'
        CloseFile(&info);
        return false;
    }

    uint32 sampleCount = 0, dataOffset = 0, dataSize = 0;
    bool32 isXbAdpcm = false;
    int32 pos        = 12; // skip RIFF(4) + size(4) + 'WAVE'(4)
    while (pos + 8 <= info.fileSize) {
        Seek_Set(&info, pos);
        char id[4];
        ReadBytes(&info, id, 4);
        uint32 sz = ReadInt32(&info, false);
        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ')
            isXbAdpcm = (ReadInt16(&info) == 0x0069);
        else if (id[0] == 'f' && id[1] == 'a' && id[2] == 'c' && id[3] == 't')
            sampleCount = ReadInt32(&info, false);
        else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            dataOffset = pos + 8;
            dataSize   = sz;
        }
        pos += 8 + (int32)sz + (int32)(sz & 1);
    }

    if (!isXbAdpcm || !dataSize || !sampleCount) {
        CloseFile(&info);
        return false;
    }

    AllocateStorage((void **)&sfxList[slot].buffer, dataSize, DATASET_SFX, false);
    if (!sfxList[slot].buffer) {
        PrintLog(PRINT_ERROR, "Unable to allocate ADPCM sfx buffer (%u B): %s", dataSize, filename);
        CloseFile(&info);
        return false;
    }
    Seek_Set(&info, dataOffset);
    ReadBytes(&info, (void *)sfxList[slot].buffer, dataSize);
    CloseFile(&info);

    HASH_COPY_MD5(sfxList[slot].hash, hash);
    sfxList[slot].scope              = scope;
    sfxList[slot].maxConcurrentPlays = plays;
    sfxList[slot].length             = sampleCount;
    sfxADPCMSize[slot]               = dataSize;
    return true;
}
#endif

void RSDK::LoadSfxToSlot(char *filename, uint8 slot, uint8 plays, uint8 scope)
{
    FileInfo info;
    InitFileInfo(&info);

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/SoundFX/%s", filename);

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(filename, hash);

#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
    // Prefer the loose Xbox-ADPCM SFX (1/4 the pool footprint); fall back to the pack WAV.
    if (LoadSfxADPCMLoose(filename, slot, plays, scope, hash))
        return;
#endif

    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
        sfxADPCMSize[slot] = 0; // pack fallback -> plain S16 PCM in this slot
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
                // Store SFX as S16 (native wav size) instead of F32: halves the SFX
                // pool footprint so the ~68 global sounds (jump/rings/menu) fit. This
                // is purely a storage format — loading stays synchronous, no threads.
                // The buffer field stays float* for engine compatibility; the mixer's
                // CHANNEL_SFX branch casts.
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
                // Bulk-read the whole data chunk in ONE ReadBytes (per-sample ReadInt*
                // was tens of thousands of syscalls per wav — minutes on real disc),
                // then convert in place. 16-bit keeps the engine's 0.75 attenuation;
                // 8-bit is stored plain, matching the F32 path.
                int16 *buffer = (int16 *)sfxList[slot].buffer;
                if (sampleBits == 8) {
                    // Read raw U8 into the upper half, then expand in place to S16.
                    // Iterate FORWARD: the write of buffer[s] (bytes 2s,2s+1) must not
                    // clobber raw[s'] (byte length+s') for any s' still unread — forward
                    // keeps 2s+1 < length+s+1 for all s<length-1, and raw[s] is read
                    // before its own write. (Backward corrupted 8-bit sfx like SSExit.)
                    uint8 *raw = (uint8 *)buffer + length;
                    ReadBytes(&info, raw, length);
                    for (int32 s = 0; s < (int32)length; ++s) buffer[s] = (int16)((raw[s] - 0x80) << 8);
                }
                else {
                    ReadBytes(&info, buffer, length * sizeof(int16));
                    for (int32 s = 0; s < (int32)length; ++s) buffer[s] = (int16)((buffer[s] * 3) >> 2);
                }
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

void RSDK::LoadSfx(char *filename, uint8 plays, uint8 scope)
{
#if RETRO_PLATFORM == RETRO_XBOX
    // Dedupe: stage sfx lists can re-list files already loaded as globals; a
    // duplicate would waste a slot and pool space. Keep the widest scope so a
    // global re-listed by a stage isn't cleared on stage unload.
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

    if (id != (uint16)-1)
        LoadSfxToSlot(filename, id, plays, scope);
}

int32 RSDK::PlaySfx(uint16 sfx, uint32 loopPoint, uint32 priority)
{
    if (sfx >= SFX_COUNT || !sfxList[sfx].scope)
        return -1;

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

    if (slot == -1)
        return -1;

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
#if RETRO_AUDIODEVICE_NXAUDIO
        // Plan B: the mixer no longer advances bufferPos — read the hardware voice's
        // playback position (in mono samples) instead.
        return AudioDevice::GetSfxPlaybackSamples(channel);
#else
        return channels[channel].bufferPos;
#endif

    if (channels[channel].state == CHANNEL_STREAM) {
#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
        // Loose-PCM streams have no vorbisInfo — report the tracked playback position.
        if (streamIsLoosePCM)
            return pcmPlayPos44k;
#endif
        if (!vorbisInfo->current_loc_valid || vorbisInfo->current_loc < 0)
            return 0;

        return vorbisInfo->current_loc;
    }

    return 0;
}

double RSDK::GetVideoStreamPos()
{
#if RETRO_PLATFORM == RETRO_XBOX && defined(RSDK_USE_NXAUDIO)
    // A loose-PCM music stream has no vorbisInfo; avoid the NULL deref below.
    if (channels[0].state == CHANNEL_STREAM && streamIsLoosePCM && AudioDevice::audioState && AudioDevice::initializedAudioChannels)
        return pcmPlayPos44k / (double)AUDIO_FREQUENCY;
#endif
    if (channels[0].state == CHANNEL_STREAM && AudioDevice::audioState && AudioDevice::initializedAudioChannels && vorbisInfo
        && vorbisInfo->current_loc_valid) {
        return vorbisInfo->current_loc / (double)AUDIO_FREQUENCY;
    }

    return -1.0;
}

void RSDK::ClearStageSfx()
{
    LockAudioDevice();

#if RETRO_AUDIODEVICE_NXAUDIO
    // Plan B: the SFX voices DMA straight from the DATASET_SFX pool. Unloading stage
    // SFX below (and the next stage's loads) can compact/overwrite that pool, so stop
    // every hardware voice first — a voice must never be reading bytes as they move.
    AudioDevice::StopSfxVoices();
#endif

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
