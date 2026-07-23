// SDL3 removed SDL_LockAudioDevice; the stream get-callback runs with the stream lock
// held, so locking the stream gives the same mutual exclusion the SDL2 macros gave
#define LockAudioDevice()   SDL_LockAudioStream(AudioDevice::stream)
#define UnlockAudioDevice() SDL_UnlockAudioStream(AudioDevice::stream)

namespace RSDK
{
class AudioDevice : public AudioDeviceBase
{
public:
    static SDL_AudioStream *stream;

    static bool32 Init();
    static void Release();

    static void FrameInit() {}

    inline static void HandleStreamLoad(ChannelInfo *channel, bool32 async)
    {
        // Loading a music stream shares the persistent Data.rsdk file handle with the
        // main thread, which isn't safe across threads on Xbox — load synchronously.
        (void)async;
        LoadStream(channel);
    }

private:
    static uint8 contextInitialized;

    static void InitAudioChannels();

    static void SDLCALL AudioStreamCallback(void *userdata, SDL_AudioStream *stream, int32 additionalAmount, int32 totalAmount);
};
} // namespace RSDK
