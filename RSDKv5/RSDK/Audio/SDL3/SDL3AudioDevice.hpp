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
#if RETRO_PLATFORM == RETRO_XBOX
        // Pack reads are seek+read-atomic (Reader.hpp packReadLock), so streams can
        // load on the async loader thread; CHANNEL_LOADING_STREAM keeps the channel
        // reserved until the data lands. Falls back to sync if the queue is full.
        if (async && EnqueueStreamLoad(channel))
            return;
#else
        (void)async;
#endif
        LoadStream(channel);
    }

private:
    static uint8 contextInitialized;

    static void InitAudioChannels();

    static void SDLCALL AudioStreamCallback(void *userdata, SDL_AudioStream *stream, int32 additionalAmount, int32 totalAmount);
};
} // namespace RSDK
