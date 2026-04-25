// MiSTer render device — Phase 1 skeleton.
//
// Every public entry point logs via PrintLog() and returns. No hardware
// touched. No /dev/mem. Pixel output lands in Phase 2 via NativeVideoWriter.
//
// This file is textually `#include`d by Drawing.cpp (see the
// `#if RETRO_RENDERDEVICE_*` chain at Drawing.cpp:132-146, MiSTer arm on 144),
// so the RSDK:: namespace is already active and every common header
// (RetroEngine.hpp, types, PrintLog, videoSettings, isRunning, SCREEN_YSIZE,
// etc.) is already visible. Do NOT add #include directives here.
//
// NOTE: RenderDeviceBase:: statics (isRunning, windowRefreshDelay,
// displayWidth, displayHeight, displayCount, lastShaderID, startVertex_2P,
// startVertex_3P, pixelSize, textureSize, viewSize) are defined in
// Drawing.cpp at lines 172-188 for ALL backends. Do NOT redefine them here or
// we get multiple-definition link errors.

// --- Derived-class static storage ---
// NOTE: `RenderDevice::displayInfo` is DEFINED by Drawing.cpp:148
// (`RenderDevice::WindowInfo RenderDevice::displayInfo;`) for all backends.
// Do NOT redefine it here — multiple-definition link error.

unsigned long long RenderDevice::targetFreq = 0;
unsigned long long RenderDevice::curTicks   = 0;
unsigned long long RenderDevice::prevTicks  = 0;

// --- Phase 6 pacer / overlay static storage ---
MiSTerPacerFeedbackState RenderDevice::pacerFeedback          = {};
unsigned long long       RenderDevice::pacerFrameDeadlineNs   = 0;
unsigned long long       RenderDevice::pacerTargetFrameTimeNs = 0;

// FPS overlay: default-on in telemetry flavor per feedback-always-telemetry.md
#if defined(ENABLE_PERF_TELEMETRY) && ENABLE_PERF_TELEMETRY
bool     RenderDevice::showFPSOverlay = true;
#else
bool     RenderDevice::showFPSOverlay = false;
#endif
int32              RenderDevice::fpsOverlayMode  = 0;
unsigned long long RenderDevice::fpsAccumNs      = 0;
uint32             RenderDevice::fpsSampleCount  = 0;
int32              RenderDevice::fpsLastValue    = 0;
char               RenderDevice::fpsLabel[96]    = "--";

unsigned long long RenderDevice::pacerMaxJitterNs = 0;
unsigned long long RenderDevice::pacerJitterSumNs = 0;
uint32             RenderDevice::pacerLateCount   = 0;
uint32             RenderDevice::pacerStatsFrames = 0;

// File-scope (not function-static) so InitFPSCap can reset it across
// Release+re-Init / scene reload boundaries. P-2.2 review fix.
static unsigned long long s_prevFrameNs = 0;

bool RenderDevice::Init()
{
    // Install fatal-signal handlers first so any crash downstream prints a
    // backtrace + register state + /proc/self/maps to stderr (which the
    // wrapper dup2's to last-run.log). Lets us diagnose wrapper-context
    // crashes that gdb masks.
    SigHandler_Init();

    // Phase 9: pixWidth is now aspect-keyed off SONIC_MANIA_ASPECT env var
    // emitted by the wrapper from status[13].
    //   "169" -> 424 (16:9 widescreen, paired with 8.7069 MHz pixel clock)
    //   "43" / unset / anything else -> 320 (4:3, NTSC-exact 6.75 MHz pixel)
    // Must be the FIRST statement so SetupRendering -> InitGraphicsAPI ->
    // SetScreenSize observe the new value. LoadSettingsINI() may have read a
    // stale value from upstream settings.ini; overwrite unconditionally.
    {
        const char *aspect_env = getenv("SONIC_MANIA_ASPECT");
        if (aspect_env && strcmp(aspect_env, "169") == 0) {
            videoSettings.pixWidth = 424;
        } else {
            videoSettings.pixWidth = 320;
        }
        PrintLog(PRINT_NORMAL, "Phase 9: SONIC_MANIA_ASPECT=%s -> pixWidth=%d",
                 aspect_env ? aspect_env : "(unset)", (int)videoSettings.pixWidth);
    }
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Init: pixWidth=%d SCREEN_YSIZE=%d", videoSettings.pixWidth, SCREEN_YSIZE);

    // Phase 9: also propagate the runtime dims to NativeVideoWriter so its
    // memcpy + DDR3 layout (BUF1_OFFSET, FRAME_BYTES) sizes off the active
    // aspect rather than the compile-time 320x240 default. MUST happen before
    // NativeVideoWriter_Init() runs so the mmap covers the correct region.
    NativeVideoWriter_SetDims((int)videoSettings.pixWidth, (int)SCREEN_YSIZE);

    // Phase 9: SONIC_MANIA_FPS_OVERLAY env var seeds the static overlay state.
    //   "0" -> off
    //   "1" -> Simple   (showFPSOverlay=true,  fpsOverlayMode=0)
    //   "2" -> Detailed (showFPSOverlay=true,  fpsOverlayMode=1)
    // Must run before any frame logic / pacer reads showFPSOverlay.
    {
        const char *fpso = getenv("SONIC_MANIA_FPS_OVERLAY");
        const int mode = fpso ? atoi(fpso) : 0;
        switch (mode) {
            case 1:
                RenderDevice::showFPSOverlay = true;
                RenderDevice::fpsOverlayMode = 0;
                break;
            case 2:
                RenderDevice::showFPSOverlay = true;
                RenderDevice::fpsOverlayMode = 1;
                break;
            case 0:
            default:
                RenderDevice::showFPSOverlay = false;
                RenderDevice::fpsOverlayMode = 0;
                break;
        }
        PrintLog(PRINT_NORMAL, "Phase 9: SONIC_MANIA_FPS_OVERLAY=%s -> showFPSOverlay=%d fpsOverlayMode=%d",
                 fpso ? fpso : "(unset)",
                 (int)RenderDevice::showFPSOverlay, (int)RenderDevice::fpsOverlayMode);
    }

    if (!SetupRendering())
        return false;

    if (!NativeVideoWriter_Init()) {
        PrintLog(PRINT_NORMAL,
                 "MiSTerRenderDevice::Init: NativeVideoWriter_Init FAILED (likely Mac host or /dev/mem inaccessible) - continuing as no-op");
        // Do NOT return false: the rest of the engine should still come up so
        // the Mac iteration path still logs and the armhf binary run under a
        // non-root UID can still exit cleanly with a visible error rather
        // than a silent crash. Subsequent WriteFrame calls become no-ops via
        // the ddr_base == NULL check inside the writer.
    }

    // Phase 3: match upstream pattern (SDL2RenderDevice.cpp:72-75,
    // DX9RenderDevice.cpp:114-117, EGLRenderDevice.cpp:143-145). Every other
    // backend initializes audio + input from inside Init(); the parent
    // RunRetroEngine() flow assumes they're up by the time Init() returns.
    //
    // NOTE: SDL2AudioDevice's AudioDevice::Init() (SDL2AudioDevice.cpp:7-33)
    // ALWAYS returns true — on SDL_OpenAudioDevice failure it only logs and
    // sets audioState=false internally. Upstream's `if (!AudioDevice::Init())
    // return false;` branch therefore never fires with the SDL2 audio backend.
    // We match SDL2RenderDevice's call pattern by discarding the return value
    // explicitly; failure detection comes from SDL2AudioDevice's own
    // "ERROR: Unable to open audio device!" log line, not the Init return.
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Init: calling AudioDevice::Init()");
    (void)AudioDevice::Init();

    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Init: calling InitInputDevices()");
    InitInputDevices();

    return true;
}

bool RenderDevice::SetupRendering()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::SetupRendering()");
    if (!InitGraphicsAPI())
        return false;
    InitVertexBuffer();
    GetDisplays();
    InitShaders();

    // Phase 5 fix: allocate the global `scanlines` buffer used by parallax /
    // scene rendering. EVERY other RSDKv5 backend does this inside
    // SetupRendering (SDL2RenderDevice.cpp:608-610, DX9:964-965, DX11:1385-86,
    // GLFW:156-157, VK:443-444, EGL:223-224). If we skip it, `scanlines`
    // stays NULL and the first call to ProcessParallax() / DrawLayerHScroll /
    // DrawLayerVScroll SIGSEGVs (`scanline = scanlines; scanline->position.x = ...`).
    // Gdb confirmed this is the cause of the armhf Logos-scene crash — the
    // Mac build happens to survive because macOS's NULL page behavior differs.
    int32 size = videoSettings.pixWidth >= SCREEN_YSIZE ? videoSettings.pixWidth : SCREEN_YSIZE;
    if (!scanlines) {
        scanlines = (ScanlineInfo *)malloc(size * sizeof(ScanlineInfo));
        if (scanlines) memset(scanlines, 0, size * sizeof(ScanlineInfo));
    }
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::SetupRendering: allocated scanlines[%d] = %p", size, scanlines);

    videoSettings.windowState = WINDOWSTATE_ACTIVE;
    videoSettings.dimMax      = 1.0;
    videoSettings.dimPercent  = 1.0;
    return true;
}

bool RenderDevice::InitGraphicsAPI()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::InitGraphicsAPI()");
    // Phase 2: establish screen geometry so the software rasterizer knows
    // valid pitch / clip bounds. Mania is single-screen (screenCount=1) and
    // RetroEngine sets currentScreen=&screens[0] after Init succeeds.
    // Without this call, screens[0].size.x stays 0 and the rasterizer
    // produces no output, silently breaking the devmem2 probe in Step 7.
    SetScreenSize(0, videoSettings.pixWidth, SCREEN_YSIZE);
    return true;
}

void RenderDevice::InitVertexBuffer()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::InitVertexBuffer() [stub]");
}

void RenderDevice::GetDisplays()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::GetDisplays() [stub]");
    // INVARIANT: keep `displayInfo.displays` populated before setting
    // `displayCount` non-zero. Drawing.cpp:313-349 iterates
    // `displayInfo.displays[0..displayCount-1]` unconditionally and will NULL-
    // deref if `displayCount > 0` while `displays == nullptr`. Phase 2
    // implementers: allocate `displays[]` first, then assign `displayCount`.
    displayCount = 0;
}

void RenderDevice::CopyFrameBuffer()
{
    // Intentionally empty on MiSTer. The rasterizer's frameBuffer IS the
    // DDR3 source; there is no intermediate texture to upload (contrast
    // SDL2RenderDevice.cpp:79-96 which SDL_LockTexture/memcpy/Unlock-uploads
    // every frame). The actual DDR3 write happens in FlipScreen() — that is
    // the last hook per frame (see RetroEngine.cpp:313,320) so rasterizer
    // state is settled by then.
}

// Phase 6 — rolling FPS accumulator. Refreshes the overlay label once per
// second so the text is readable. Called from FlipScreen only when the
// overlay is on.
static void MiSTer_UpdateFPSOverlay(unsigned long long frame_ns,
                                    const MiSTerPacerFeedbackState &fs)
{
    RenderDevice::fpsAccumNs += frame_ns;
    RenderDevice::fpsSampleCount++;

    if (RenderDevice::fpsAccumNs >= 1000000000ULL) {
        double avg_ns = (double)RenderDevice::fpsAccumNs
                        / (double)RenderDevice::fpsSampleCount;
        if (avg_ns < 1.0) avg_ns = 1.0;
        RenderDevice::fpsLastValue = (int32)(1e9 / avg_ns + 0.5);

#if defined(ENABLE_PERF_TELEMETRY) && ENABLE_PERF_TELEMETRY
        if (RenderDevice::fpsOverlayMode == 1) {
            const char *prefix = fs.vsync_feedback_valid ? "CL" : "OL";
            long long phase_us = (long long)(fs.pacer_phase_error_ns / 1000);
            double u_ms = mister_perf_avg_scope_ms(MISTER_PERF_SCOPE_UPDATE);
            double r_ms = mister_perf_avg_scope_ms(MISTER_PERF_SCOPE_RASTER);
            double p_ms = mister_perf_avg_scope_ms(MISTER_PERF_SCOPE_PRESENT);
            SDL_snprintf(RenderDevice::fpsLabel, sizeof(RenderDevice::fpsLabel),
                         "%d u:%4.1f r:%4.1f p:%4.1f %s:e%+05lld",
                         RenderDevice::fpsLastValue,
                         u_ms, r_ms, p_ms,
                         prefix, phase_us);
        } else
#else
        (void)fs;
#endif
        {
            SDL_snprintf(RenderDevice::fpsLabel, sizeof(RenderDevice::fpsLabel),
                         "%d", RenderDevice::fpsLastValue);
        }
        RenderDevice::fpsAccumNs     = 0;
        RenderDevice::fpsSampleCount = 0;
    }
}

void RenderDevice::FlipScreen()
{
    // Mania is single-screen; screens[0].frameBuffer holds the freshly-
    // rasterized RGB565 frame. screens[0].pitch is in UINT16 pixels, so
    // multiply by sizeof(uint16) for the byte pitch the writer expects.
    // Phase 9: dims are runtime-keyed off SONIC_MANIA_ASPECT (see Init()).
    if (screens[0].size.x != nv_frame_width_runtime ||
        screens[0].size.y != nv_frame_height_runtime) {
        // Defensive: the engine might reconfigure mid-run (SetScreenSize
        // calls from mods / cutscenes). If dims drift from the writer's
        // contract, skip this frame — the writer would reject it anyway.
        // Still update prevFrameNs so the next correctly-sized frame's delta
        // is bounded by one real frame, not by however long the dim mismatch
        // persisted. P-2.3 review fix.
        s_prevFrameNs = mister_pacer_now_ns();
        return;
    }

    // SCOPE_PRESENT wraps the DDR3 memcpy + ctrl flip.
    mister_perf_begin_scope(MISTER_PERF_SCOPE_PRESENT);

    // --- Phase 6: FPS overlay (on-canvas, before WriteFrame) ---
    unsigned long long now = mister_pacer_now_ns();
    if (s_prevFrameNs != 0 && showFPSOverlay) {
        MiSTer_UpdateFPSOverlay(now - s_prevFrameNs, pacerFeedback);
    }
    s_prevFrameNs = now;

    if (showFPSOverlay && currentScreen == &screens[0]) {
        // DrawDevString writes into currentScreen->frameBuffer (RGB565) and
        // clips against currentScreen->size. Overlay lives at (2,2) top-left
        // in green — avoids Mania's HUD which is top-center and top-right.
        DrawDevString(fpsLabel, 2, 2, ALIGN_LEFT, 0x00FF00);
    }

    NativeVideoWriter_WriteFrame(screens[0].frameBuffer,
                                 nv_frame_width_runtime,
                                 nv_frame_height_runtime,
                                 screens[0].pitch * (int)sizeof(uint16));

    mister_perf_end_scope(MISTER_PERF_SCOPE_PRESENT);
}

void RenderDevice::Release(bool32 isRefresh)
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Release(isRefresh=%d)", (int)isRefresh);

    // Intentional: do NOT call AudioDevice::Release() or ReleaseInputDevices()
    // here. The engine main loop (RetroEngine.cpp:326-328) already calls them
    // in order *before* invoking RenderDevice::Release(false). No upstream
    // backend (SDL2/DX9/DX11/EGL/Vulkan) redoes that work from inside their
    // Release; adding it would cause double-release / double-free.

    if (!isRefresh) {
        // Full shutdown only on the final teardown; Refresh is used by
        // the engine for windowed<->fullscreen transitions etc., which
        // are no-ops for our FPGA-driven scanout. See OQ-4 in phase-2-plan.md.
        NativeVideoWriter_Shutdown();
    }
}

void RenderDevice::RefreshWindow()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::RefreshWindow() [stub]");
}

void RenderDevice::GetWindowSize(int32 *width, int32 *height)
{
    // Phase 9: report the active runtime width (320 for 4:3, 424 for 16:9)
    // rather than a hardcoded 320 placeholder.
    if (width)
        *width = nv_frame_width_runtime;
    if (height)
        *height = SCREEN_YSIZE;
}

void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels)
{
    (void)width;
    (void)height;
    (void)imagePixels;
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::SetupImageTexture() [stub]");
}

void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
    (void)width;
    (void)height;
    (void)yPlane;
    (void)uPlane;
    (void)vPlane;
    (void)strideY;
    (void)strideU;
    (void)strideV;
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::SetupVideoTexture_YUV420() [stub]");
}

void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
    (void)width;
    (void)height;
    (void)yPlane;
    (void)uPlane;
    (void)vPlane;
    (void)strideY;
    (void)strideU;
    (void)strideV;
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::SetupVideoTexture_YUV422() [stub]");
}

void RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
    (void)width;
    (void)height;
    (void)yPlane;
    (void)uPlane;
    (void)vPlane;
    (void)strideY;
    (void)strideU;
    (void)strideV;
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::SetupVideoTexture_YUV444() [stub]");
}

// Phase 3: lifted-and-shifted from SDL2RenderDevice.cpp:694-1035 with
// window-specific branches stripped:
//   - Entire window-event case (MiSTer has no SDL window; SDL_QUIT still
//     handles window-close via X11/wayland mapping on desktop, a no-op on
//     MiSTer).
//   - Alt+Enter fullscreen toggle inside SDL_SCANCODE_RETURN (no window to
//     toggle; the outer case label stays so RETURN falls through to the
//     default keyboard-state-update path).
// Everything else — controller add/remove, keyboard up/down with devmenu
// hotkeys, mouse/touch, SDL_APP_*, SDL_QUIT — is preserved verbatim.
void RenderDevice::ProcessEvent(SDL_Event event)
{
// ENABLE_PERF_TELEMETRY is ALWAYS defined (to 0 or 1) in MiSTer builds via
// platforms/MiSTer.cmake:158's $<BOOL:…> genexpr — must use `#if`, not
// `#ifdef`. For non-MiSTer builds the macro may be undefined; use the
// defined() && value form so the log line compiles out harmlessly.
#if defined(ENABLE_PERF_TELEMETRY) && ENABLE_PERF_TELEMETRY
    if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED
        || event.type == SDL_KEYDOWN || event.type == SDL_KEYUP || event.type == SDL_QUIT) {
        PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::ProcessEvent(type=0x%x)", (unsigned)event.type);
    }
#endif

    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            SDL_GameController *game_controller = SDL_GameControllerOpen(event.cdevice.which);

            if (game_controller != NULL) {
                uint32 id;
                char idBuffer[0x20];
                sprintf_s(idBuffer, sizeof(idBuffer), "SDLDevice%d", SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(game_controller)));
                GenerateHashCRC(&id, idBuffer);

                if (SKU::InitSDL2InputDevice(id, game_controller) == NULL)
                    SDL_GameControllerClose(game_controller);
            }

            break;
        }

        case SDL_CONTROLLERDEVICEREMOVED: {
            uint32 id;
            char idBuffer[0x20];
            sprintf_s(idBuffer, sizeof(idBuffer), "SDLDevice%d", event.cdevice.which);
            GenerateHashCRC(&id, idBuffer);

            RemoveInputDevice(InputDeviceFromID(id));
            break;
        }

        case SDL_APP_WILLENTERFOREGROUND:
#if RETRO_REV02
            SKU::userCore->focusState = 0;
#endif
            break;

        case SDL_APP_WILLENTERBACKGROUND:
#if RETRO_REV02
            SKU::userCore->focusState = 1;
#endif
            break;

        case SDL_APP_TERMINATING: isRunning = false; break;

        case SDL_MOUSEBUTTONDOWN:
            switch (event.button.button) {
                case SDL_BUTTON_LEFT: touchInfo.down[0] = true; touchInfo.count = 1;
#if !RETRO_REV02
                    RSDK::SKU::buttonDownCount++;
#endif
                    break;

                case SDL_BUTTON_RIGHT:
#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[3] = true;
                    RSDK::SKU::buttonDownCount++;
#endif
                    break;
            }
            break;

        case SDL_MOUSEBUTTONUP:
            switch (event.button.button) {
                case SDL_BUTTON_LEFT: touchInfo.down[0] = false; touchInfo.count = 0;
#if !RETRO_REV02
                    RSDK::SKU::buttonDownCount--;
#endif
                    break;

                case SDL_BUTTON_RIGHT:
#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[3] = false;
                    RSDK::SKU::buttonDownCount--;
#endif
                    break;
            }
            break;

        case SDL_FINGERMOTION:
        case SDL_FINGERDOWN:
        case SDL_FINGERUP: {
            int32 count     = SDL_GetNumTouchFingers(event.tfinger.touchId);
            touchInfo.count = 0;
            for (int32 i = 0; i < count; i++) {
                SDL_Finger *finger = SDL_GetTouchFinger(event.tfinger.touchId, i);
                if (finger) {
                    touchInfo.down[touchInfo.count] = true;
                    touchInfo.x[touchInfo.count]    = finger->x;
                    touchInfo.y[touchInfo.count]    = finger->y;
                    touchInfo.count++;
                }
            }
            break;
        }

        case SDL_KEYDOWN:
#if !RETRO_REV02
            ++RSDK::SKU::buttonDownCount;
#endif
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_RETURN:
                    // PHASE-3(MiSTer): Alt+Enter fullscreen toggle removed —
                    // MiSTer has no SDL_Window to flip. RETURN falls through
                    // to the default SKU::UpdateKeyState path below.
#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[1] = true;
#endif
                    // [fallthrough]

                default:
#if RETRO_INPUTDEVICE_KEYBOARD
                    SKU::UpdateKeyState(event.key.keysym.scancode);
#endif
                    break;

                case SDL_SCANCODE_ESCAPE:
                    if (engine.devMenu) {
#if RETRO_REV0U
                        if (sceneInfo.state == ENGINESTATE_DEVMENU || RSDK::Legacy::gameMode == RSDK::Legacy::ENGINE_DEVMENU)
#else
                        if (sceneInfo.state == ENGINESTATE_DEVMENU)
#endif
                            CloseDevMenu();
                        else
                            OpenDevMenu();
                    }
                    else {
#if RETRO_INPUTDEVICE_KEYBOARD
                        SKU::UpdateKeyState(event.key.keysym.scancode);
#endif
                    }

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                    RSDK::SKU::specialKeyStates[0] = true;
#endif
                    break;

#if !RETRO_USE_ORIGINAL_CODE
                case SDL_SCANCODE_F1:
                    if (engine.devMenu) {
                        sceneInfo.listPos--;
                        while (sceneInfo.listPos < sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart
                            || sceneInfo.listPos > sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd
                            || !sceneInfo.listCategory[sceneInfo.activeCategory].sceneCount) {
                            sceneInfo.activeCategory--;
                            if (sceneInfo.activeCategory >= sceneInfo.categoryCount) {
                                sceneInfo.activeCategory = sceneInfo.categoryCount - 1;
                            }
                            sceneInfo.listPos = sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd - 1;
                        }

#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5: LoadScene(); break;
                            case 4:
                            case 3: RSDK::Legacy::stageMode = RSDK::Legacy::STAGEMODE_LOAD; break;
                        }
#else
                        LoadScene();
#endif
                    }
                    break;

                case SDL_SCANCODE_F2:
                    if (engine.devMenu) {
                        sceneInfo.listPos++;
                        while (sceneInfo.listPos < sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart
                            || sceneInfo.listPos > sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetEnd
                            || !sceneInfo.listCategory[sceneInfo.activeCategory].sceneCount) {
                            sceneInfo.activeCategory++;
                            if (sceneInfo.activeCategory >= sceneInfo.categoryCount) {
                                sceneInfo.activeCategory = 0;
                            }
                            sceneInfo.listPos = sceneInfo.listCategory[sceneInfo.activeCategory].sceneOffsetStart;
                        }

#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5: LoadScene(); break;
                            case 4:
                            case 3: RSDK::Legacy::stageMode = RSDK::Legacy::STAGEMODE_LOAD; break;
                        }
#else
                        LoadScene();
#endif
                    }
                    break;
#endif

                case SDL_SCANCODE_F3:
                    // MiSTer: upstream cycles user shaders, but InitShaders()
                    // sets shaderSupport=false and userShaderCount stays 0.
                    // Rebind F3 to toggle the Phase-6 FPS overlay (always on
                    // in telemetry flavor by default).
                    showFPSOverlay = !showFPSOverlay;
                    PrintLog(PRINT_NORMAL,
                             "MiSTerRenderDevice: FPS overlay %s",
                             showFPSOverlay ? "on" : "off");
                    break;

#if !RETRO_USE_ORIGINAL_CODE
                case SDL_SCANCODE_F4:
                    if (engine.devMenu)
                        engine.showEntityInfo ^= 1;
                    break;

                case SDL_SCANCODE_F5:
                    if (engine.devMenu) {
                        // Quick-Reload
#if RETRO_USE_MOD_LOADER
                        if (event.key.keysym.mod & KMOD_LCTRL)
                            RefreshModFolders();
#endif

#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5: LoadScene(); break;
                            case 4:
                            case 3: RSDK::Legacy::stageMode = RSDK::Legacy::STAGEMODE_LOAD; break;
                        }
#else
                        LoadScene();
#endif
                    }
                    break;

                case SDL_SCANCODE_F6:
                    // MiSTer: upstream decrements screenCount when devMenu &&
                    // screenCount>1; Mania runs screenCount==1 so that path
                    // is inert. Rebind F6 to cycle the FPS overlay mode.
                    //
                    // P-2.6 review fix: in the clean (non-telemetry) flavor
                    // the detailed mode would print zero-filled u/r/p/phase
                    // fields because mister_perf_avg_scope_ms is a stub
                    // returning 0.0 and pacer_phase_error_ns is never set.
                    // Gate the cycle on telemetry so clean+F6 is a true
                    // no-op rather than a degenerate UI state.
#if defined(ENABLE_PERF_TELEMETRY) && ENABLE_PERF_TELEMETRY
                    fpsOverlayMode = (fpsOverlayMode + 1) % 2;
                    PrintLog(PRINT_NORMAL,
                             "MiSTerRenderDevice: FPS overlay mode=%d",
                             (int)fpsOverlayMode);
#endif
                    break;

                case SDL_SCANCODE_F7:
                    if (engine.devMenu && videoSettings.screenCount < SCREEN_COUNT)
                        videoSettings.screenCount++;
                    break;

                case SDL_SCANCODE_F8:
                    if (engine.devMenu)
                        engine.showUpdateRanges ^= 1;
                    break;

                case SDL_SCANCODE_F9:
                    if (engine.devMenu)
                        showHitboxes ^= 1;
                    break;

                case SDL_SCANCODE_F10:
                    if (engine.devMenu)
                        engine.showPaletteOverlay ^= 1;
                    break;
#endif
                case SDL_SCANCODE_BACKSPACE:
                    if (engine.devMenu)
                        engine.gameSpeed = engine.fastForwardSpeed;
                    break;

                case SDL_SCANCODE_F11:
                case SDL_SCANCODE_INSERT:
                    if (engine.devMenu)
                        engine.frameStep = true;
                    break;

                case SDL_SCANCODE_F12:
                case SDL_SCANCODE_PAUSE:
                    // Phase 6 Step 2: log pacer jitter stats on every press.
                    // Always-on (no devMenu gate) so dev testers can sample
                    // the ring buffer without opening the dev menu — which
                    // itself perturbs pacer cadence.
                    //
                    // P-2.7 review note on jitter semantics: the values
                    // reported here measure (now - pacerFrameDeadlineNs)
                    // AFTER the closed-loop 25%-blend correction in
                    // UpdateFPSCap has already moved the deadline toward
                    // the FPGA-observed vsync. They are NOT the raw
                    // wake-up phase relative to true vsync. In the steady
                    // state with vsync feedback engaged the two converge,
                    // but during initial lock-on or after a snap the post-
                    // blend deadline can mask underlying scheduler jitter.
                    // Use phase_err in the F12 line (raw closed-loop
                    // error before blend) for the true vsync-phase signal.
                    if (pacerStatsFrames > 0) {
                        unsigned long long avg_jitter_ns =
                            pacerJitterSumNs / pacerStatsFrames;
                        PrintLog(PRINT_NORMAL,
                                 "MiSTerPacer jitter over %u frames: "
                                 "max=%lluus avg=%lluus late(>500us)=%u phase_err=%lldus %s",
                                 (unsigned)pacerStatsFrames,
                                 (unsigned long long)(pacerMaxJitterNs / 1000ULL),
                                 (unsigned long long)(avg_jitter_ns / 1000ULL),
                                 (unsigned)pacerLateCount,
                                 (long long)(pacerFeedback.pacer_phase_error_ns / 1000),
                                 pacerFeedback.vsync_feedback_valid ? "CL" : "OL");
                        // Reset so the next F12 window is a fresh sample.
                        pacerMaxJitterNs = 0;
                        pacerJitterSumNs = 0;
                        pacerLateCount   = 0;
                        pacerStatsFrames = 0;
                    }
                    if (engine.devMenu) {
#if RETRO_REV0U
                        switch (engine.version) {
                            default: break;
                            case 5:
                                if (sceneInfo.state != ENGINESTATE_NONE)
                                    sceneInfo.state ^= ENGINESTATE_STEPOVER;
                                break;
                            case 4:
                            case 3:
                                if (RSDK::Legacy::stageMode != ENGINESTATE_NONE)
                                    RSDK::Legacy::stageMode ^= RSDK::Legacy::STAGEMODE_STEPOVER;
                                break;
                        }
#else
                        if (sceneInfo.state != ENGINESTATE_NONE)
                            sceneInfo.state ^= ENGINESTATE_STEPOVER;
#endif
                    }
                    break;
            }
            break;

        case SDL_KEYUP:
#if !RETRO_REV02
            --RSDK::SKU::buttonDownCount;
#endif
            switch (event.key.keysym.scancode) {
                default:
#if RETRO_INPUTDEVICE_KEYBOARD
                    SKU::ClearKeyState(event.key.keysym.scancode);
#endif
                    break;

#if !RETRO_REV02 && RETRO_INPUTDEVICE_KEYBOARD
                case SDL_SCANCODE_ESCAPE:
                    RSDK::SKU::specialKeyStates[0] = false;
                    SKU::ClearKeyState(event.key.keysym.scancode);
                    break;

                case SDL_SCANCODE_RETURN:
                    RSDK::SKU::specialKeyStates[1] = false;
                    SKU::ClearKeyState(event.key.keysym.scancode);
                    break;
#endif
                case SDL_SCANCODE_BACKSPACE: engine.gameSpeed = 1; break;
            }
            break;

        case SDL_QUIT: isRunning = false; break;
    }
}

bool RenderDevice::ProcessEvents()
{
    SDL_Event sdlEvent;

    while (SDL_PollEvent(&sdlEvent)) {
        ProcessEvent(sdlEvent);

        if (!isRunning)
            return false;
    }

    return isRunning;
}

void RenderDevice::InitFPSCap()
{
    // Phase 6 — pacer scaffold. target_frame_time_ns = 1s / refreshRate.
    int32 rr = videoSettings.refreshRate > 0 ? videoSettings.refreshRate : 60;
    pacerTargetFrameTimeNs = (unsigned long long)(1000000000.0 / (double)rr);
    pacerFrameDeadlineNs   = 0;
    pacerFeedback          = {};

    // Reset pacer jitter stats so the ring starts clean on every init.
    pacerMaxJitterNs = 0;
    pacerJitterSumNs = 0;
    pacerLateCount   = 0;
    pacerStatsFrames = 0;

    // P-2.2 review fix: reset the FPS-sample baseline so a Release+re-Init
    // (or scene reload, which re-runs InitFPSCap path) does not let the
    // first post-reset frame inherit a multi-second delta.
    s_prevFrameNs = 0;
    fpsAccumNs     = 0;
    fpsSampleCount = 0;

    PrintLog(PRINT_NORMAL,
             "MiSTerRenderDevice::InitFPSCap: target=%lluns (%d Hz)",
             (unsigned long long)pacerTargetFrameTimeNs, (int)rr);
}

bool RenderDevice::CheckFPSCap()
{
    // MiSTer: always proceed; the closed-loop pacer in UpdateFPSCap owns
    // the sleep. Returning false here would put us in a spin around
    // ProcessEvents that wastes ARM cycles.
    return true;
}

void RenderDevice::UpdateFPSCap()
{
    mister_pacer_poll_feedback(&pacerFeedback);

    unsigned long long now_ns = mister_pacer_now_ns();

    // Staleness disengagement (100 ms without new seq ⇒ open-loop).
    if (pacerFeedback.vsync_feedback_valid &&
        pacerFeedback.last_feedback_update_ns > 0 &&
        (now_ns - pacerFeedback.last_feedback_update_ns) > 100000000ULL) {
        pacerFeedback.vsync_feedback_valid = false;
        PrintLog(PRINT_NORMAL,
                 "MiSTerRenderDevice: vsync feedback stale, falling back to open-loop");
    }

    if (pacerFrameDeadlineNs == 0) {
        pacerFrameDeadlineNs = now_ns + pacerTargetFrameTimeNs;
    }

    // Closed-loop phase correction (ported from 3sx sdl_app.c:10679-10710).
    // P-3.3 review fix: hoist the μs→ns conversion of the CMake-driven lead
    // time into a named constant so the unit math is unambiguous at the use
    // site below. MISTER_PACER_LEAD_TIME_US is set by platforms/MiSTer.cmake
    // (default 2000 µs).
    static constexpr unsigned long long kPacerLeadTimeNs =
        (unsigned long long)MISTER_PACER_LEAD_TIME_US * 1000ULL;

    if (pacerFeedback.vsync_feedback_valid &&
        now_ns >= pacerFeedback.last_vsync_monotonic_ns) {
        unsigned long long elapsed = now_ns - pacerFeedback.last_vsync_monotonic_ns;
        unsigned long long frames_since = elapsed / pacerTargetFrameTimeNs;
        unsigned long long next_vsync   = pacerFeedback.last_vsync_monotonic_ns
                                       + (frames_since + 1) * pacerTargetFrameTimeNs;
        unsigned long long ideal = (next_vsync > kPacerLeadTimeNs) ? (next_vsync - kPacerLeadTimeNs) : 0;
        if (ideal <= now_ns) ideal += pacerTargetFrameTimeNs;

        int64 error = (int64)((long long)ideal - (long long)pacerFrameDeadlineNs);
        pacerFeedback.pacer_phase_error_ns = error;
        if (error >  (int64)pacerTargetFrameTimeNs ||
            error < -(int64)pacerTargetFrameTimeNs) {
            pacerFrameDeadlineNs = ideal;        // snap
        } else {
            pacerFrameDeadlineNs = (unsigned long long)
                ((long long)pacerFrameDeadlineNs + error / 4);   // 25% blend
        }
    } else {
        pacerFeedback.pacer_phase_error_ns = 0;
    }

    // Sleep until deadline.
    if (now_ns < pacerFrameDeadlineNs) {
        mister_pacer_sleep_until_ns(pacerFrameDeadlineNs);
        now_ns = mister_pacer_now_ns();
    }

    // Jitter stats — measured against post-blend deadline.
    unsigned long long jitter_ns = (now_ns > pacerFrameDeadlineNs)
                                   ? (now_ns - pacerFrameDeadlineNs) : 0;
    pacerStatsFrames++;
    if (jitter_ns > pacerMaxJitterNs) pacerMaxJitterNs = jitter_ns;
    pacerJitterSumNs += jitter_ns;
    if (jitter_ns > 500000ULL) pacerLateCount++;

    // Advance deadline; guard against >1 frame behind.
    pacerFrameDeadlineNs += pacerTargetFrameTimeNs;
    if (now_ns > pacerFrameDeadlineNs + pacerTargetFrameTimeNs)
        pacerFrameDeadlineNs = now_ns + pacerTargetFrameTimeNs;
}

bool RenderDevice::InitShaders()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::InitShaders() [stub: no shaders]");
    videoSettings.shaderSupport = false;
    return true;
}

void RenderDevice::LoadShader(const char *fileName, bool32 linear)
{
    (void)fileName;
    (void)linear;
    // Intentionally quiet — called often during config load.
}
