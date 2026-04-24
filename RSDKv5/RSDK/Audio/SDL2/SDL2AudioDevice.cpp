
uint8 AudioDevice::contextInitialized;

SDL_AudioDeviceID AudioDevice::device;
SDL_AudioSpec AudioDevice::deviceSpec;

bool32 AudioDevice::Init()
{
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!contextInitialized) {
        contextInitialized = true;
        InitAudioChannels();
    }

    SDL_AudioSpec want;
    want.freq     = AUDIO_FREQUENCY;
    want.format   = AUDIO_F32SYS;
    want.samples  = MIX_BUFFER_SIZE / AUDIO_CHANNELS;
    want.channels = AUDIO_CHANNELS;
    want.callback = AudioCallback;

    audioState = false;
    if ((device = SDL_OpenAudioDevice(nullptr, 0, &want, &deviceSpec, SDL_AUDIO_ALLOW_SAMPLES_CHANGE)) > 0) {
        SDL_PauseAudioDevice(device, SDL_FALSE);
        audioState = true;

// Phase 3(MiSTer): `ENABLE_PERF_TELEMETRY` is always defined (0 or 1) in the
// MiSTer build via platforms/MiSTer.cmake:158's $<BOOL:…> genexpr; must use
// `#if`, not `#ifdef`. For the Darwin/Linux SDL2 non-MiSTer build the macro
// may be undefined — fall back to `defined() && value` so the log line
// compiles out cleanly when telemetry isn't wired.
#if defined(ENABLE_PERF_TELEMETRY) && ENABLE_PERF_TELEMETRY
        const char *driver = SDL_GetCurrentAudioDriver();
        PrintLog(PRINT_NORMAL,
                 "SDL2AudioDevice::Init: opened device=%u freq=%d channels=%d samples=%u format=0x%x driver=%s",
                 (unsigned)device, deviceSpec.freq, (int)deviceSpec.channels, (unsigned)deviceSpec.samples,
                 (unsigned)deviceSpec.format, driver ? driver : "(null)");
#endif
    }
    else {
        PrintLog(PRINT_NORMAL, "ERROR: Unable to open audio device!");
        PrintLog(PRINT_NORMAL, "ERROR: %s", SDL_GetError());
    }

    return true;
}

void AudioDevice::Release()
{
    SDL_PauseAudioDevice(device, SDL_TRUE);

    LockAudioDevice();
    AudioDeviceBase::Release();
    UnlockAudioDevice();

    SDL_CloseAudioDevice(device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioDevice::InitAudioChannels()
{
    AudioDeviceBase::InitAudioChannels();
}

void AudioDevice::AudioCallback(void *data, uint8 *stream, int32 len)
{
    (void)data; // Unused

    AudioDevice::ProcessAudioMixing(stream, len / sizeof(SAMPLE_FORMAT));
}
