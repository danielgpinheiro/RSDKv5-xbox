#define LockAudioDevice() SDL_LockAudioDevice(AudioDevice::device)
#define UnlockAudioDevice() SDL_UnlockAudioDevice(AudioDevice::device)

namespace RSDK
{
class AudioDevice : public AudioDeviceBase
{
public:
    static SDL_AudioDeviceID device;

    static bool32 Init();
    static void Release();

    static void FrameInit() {}

    inline static void HandleStreamLoad(ChannelInfo *channel, bool32 async)
    {
#if RETRO_PLATFORM == RETRO_XBOX
        // NXDK SDL2 thread crashes when LoadStream shares the Data.rsdk
        // persistent file handle across threads; load synchronously instead.
        (void)async;
        LoadStream(channel);
#else
        if (async)
            SDL_CreateThread((SDL_ThreadFunction)LoadStream, "LoadStream", (void *)channel);
        else
            LoadStream(channel);
#endif
    }

private:
    static SDL_AudioSpec deviceSpec;

    static uint8 contextInitialized;

    static void InitAudioChannels();

    static void AudioCallback(void *data, uint8 *stream, int32 len);
};
} // namespace RSDK