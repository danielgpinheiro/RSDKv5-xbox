
uint8 AudioDevice::contextInitialized;

SDL_AudioStream *AudioDevice::stream = NULL;

bool32 AudioDevice::Init()
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        PrintLog(PRINT_NORMAL, "ERROR: SDL audio init failed: %s", SDL_GetError());

    if (!contextInitialized) {
        contextInitialized = true;
        InitAudioChannels();
    }

#if RETRO_PLATFORM == RETRO_XBOX
    InitStreamLoader(); // start the music loader thread now (single-threaded, no audio lock held)
#endif

    // The engine mixes 44.1kHz stereo float; SDL3 converts to the device format
    // (48kHz S16 stereo on the Xbox AC97 backend) inside the stream
    SDL_AudioSpec spec;
    spec.format   = SDL_AUDIO_F32;
    spec.channels = AUDIO_CHANNELS;
    spec.freq     = AUDIO_FREQUENCY;

    audioState = false;
    stream     = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioStreamCallback, NULL);
    if (stream) {
        SDL_ResumeAudioStreamDevice(stream); // opened paused
        audioState = true;
    }
    else {
        PrintLog(PRINT_NORMAL, "ERROR: Unable to open audio device stream!");
        PrintLog(PRINT_NORMAL, "ERROR: %s", SDL_GetError());
    }

    return true;
}

void AudioDevice::Release()
{
    if (stream) {
        SDL_PauseAudioStreamDevice(stream);

        LockAudioDevice();
        AudioDeviceBase::Release();
        UnlockAudioDevice();

        SDL_DestroyAudioStream(stream); // also closes the device bound by OpenAudioDeviceStream
        stream = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioDevice::InitAudioChannels() { AudioDeviceBase::InitAudioChannels(); }

void SDLCALL AudioDevice::AudioStreamCallback(void *userdata, SDL_AudioStream *s, int32 additionalAmount, int32 totalAmount)
{
    (void)userdata; // Unused
    (void)totalAmount;

    static float mixBuffer[MIX_BUFFER_SIZE]; // same chunk size the SDL2 callback used

    while (additionalAmount > 0) {
        int32 len = additionalAmount < (int32)sizeof(mixBuffer) ? additionalAmount : (int32)sizeof(mixBuffer);
        len &= ~(int32)(sizeof(SAMPLE_FORMAT) * AUDIO_CHANNELS - 1); // whole frames only
        if (!len)
            break;

        AudioDevice::ProcessAudioMixing(mixBuffer, len / sizeof(SAMPLE_FORMAT));
        SDL_PutAudioStreamData(s, mixBuffer, len);

        additionalAmount -= len;
    }
}
