// nxdk-audio (MCPX APU) backend — Plan B: real mixing offload.
//
// Each RSDK channel drives its OWN hardware voice, so the APU performs the
// resample / pitch / pan / volume / sum that the software mixer used to do on
// the CPU (ProcessAudioMixing is no longer on the output path). SFX channels map
// to NX_VOICE_TYPE_2D_STATIC voices submitted straight from the paged sfxList
// pool (the library DMAs from malloc'd memory via scatter-gather); the single
// music/stream channel drives one NX_VOICE_TYPE_2D_STREAM voice fed by the
// engine's Vorbis ring.
//
// The engine still owns the channels[] array as the bookkeeping/allocation
// layer (PlaySfx/StopSfx/SetChannelAttributes/... just write channel fields);
// AudioDevice::FrameInit() reconciles the hardware voices against it once per
// frame on the game thread. Channel mutation and the reconcile share that
// thread, so LockAudioDevice is a no-op; the only cross-context producer is the
// APU completion callback, which merely flags a finished music buffer.
#define LockAudioDevice()   ((void)0)
#define UnlockAudioDevice() ((void)0)

namespace RSDK
{
class AudioDevice : public AudioDeviceBase
{
public:
    static bool32 Init();
    static void Release();

    static void FrameInit(); // reconcile hardware voices with channels[]; pump the music voice

    // Stop every SFX voice immediately. The DATASET_SFX pool can be compacted on
    // SFX load/unload (ClearStageSfx), which moves bytes under the APU's DMA — a
    // voice must never be reading the pool when that happens. Call before mutating
    // the pool.
    static void StopSfxVoices();

    // Hardware playback position (in mono samples) of an SFX channel — replaces
    // channel->bufferPos, which the mixer no longer advances. Used by GetChannelPos.
    static uint32 GetSfxPlaybackSamples(uint32 channel);

    inline static void HandleStreamLoad(ChannelInfo *channel, bool32 async)
    {
        // Music streams share the persistent Data.rsdk handle with the main thread;
        // load synchronously (same reasoning as the SDL3 backend). This fills the
        // Vorbis ring; FrameInit then brings the music voice up from it.
        (void)async;
        LoadStream(channel);
    }

private:
    static uint8 contextInitialized;

    static void InitAudioChannels();
};
} // namespace RSDK
