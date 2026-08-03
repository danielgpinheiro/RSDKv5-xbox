// nxdk-audio (MCPX APU) backend — Plan A: the engine still software-mixes every
// channel into one stereo stream (ProcessAudioMixing); this backend outputs that
// stream through a single hardware streaming voice on the APU instead of SDL/AC97.
//
// The mixer runs in FrameInit() on the game thread and channel mutations
// (PlaySfx/StopSfx/...) run on the same thread, so there is no mixer/mutator race
// and LockAudioDevice can be a no-op. The only cross-context producer is the APU
// completion callback, which merely flags a finished buffer (atomic counter).
#define LockAudioDevice()   ((void)0)
#define UnlockAudioDevice() ((void)0)

namespace RSDK
{
class AudioDevice : public AudioDeviceBase
{
public:
    static bool32 Init();
    static void Release();

    static void FrameInit(); // pumps the streaming voice (refills completed buffers)

    inline static void HandleStreamLoad(ChannelInfo *channel, bool32 async)
    {
        // Music streams share the persistent Data.rsdk handle with the main thread;
        // load synchronously (same reasoning as the SDL3 backend).
        (void)async;
        // Breadcrumbs -> xbwatson: pinpoint whether the Blue Spheres freeze is in the
        // stream load (these bracket it) or elsewhere (only the "begin" line shows).
        debugPrint("NXAUDIO: stream load begin (sfx=%d)\n", (int)channel->soundID);
        LoadStream(channel);
        debugPrint("NXAUDIO: stream load end\n");
    }

private:
    static uint8 contextInitialized;

    static void InitAudioChannels();
};
} // namespace RSDK
