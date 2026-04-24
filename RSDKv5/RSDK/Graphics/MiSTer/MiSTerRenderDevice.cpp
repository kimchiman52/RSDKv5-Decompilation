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

bool RenderDevice::Init()
{
    // Clamp pixWidth to MiSTer native resolution. Decision #1 (phase-2-plan.md).
    // Must be the FIRST statement so SetupRendering -> InitGraphicsAPI ->
    // SetScreenSize observe the clamped value. LoadSettingsINI() may have
    // read a stale 424 from upstream settings.ini; overwrite unconditionally.
    videoSettings.pixWidth = 320;
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Init: pixWidth=%d SCREEN_YSIZE=%d", videoSettings.pixWidth, SCREEN_YSIZE);

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
    return true;
}

bool RenderDevice::SetupRendering()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::SetupRendering()");
    if (!InitGraphicsAPI())
        return false;
    InitVertexBuffer();
    GetDisplays();
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

void RenderDevice::FlipScreen()
{
    // Mania is single-screen; screens[0].frameBuffer holds the freshly-
    // rasterized RGB565 frame. screens[0].pitch is in UINT16 pixels, so
    // multiply by sizeof(uint16) for the byte pitch the writer expects.
    if (screens[0].size.x != NV_FRAME_WIDTH || screens[0].size.y != NV_FRAME_HEIGHT) {
        // Defensive: the engine might reconfigure mid-run (SetScreenSize
        // calls from mods / cutscenes). If dims drift from the writer's
        // contract, skip this frame — the writer would reject it anyway.
        return;
    }
    NativeVideoWriter_WriteFrame(screens[0].frameBuffer,
                                 NV_FRAME_WIDTH,
                                 NV_FRAME_HEIGHT,
                                 screens[0].pitch * (int)sizeof(uint16));
}

void RenderDevice::Release(bool32 isRefresh)
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Release(isRefresh=%d)", (int)isRefresh);
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
    // 4:3 native placeholder. Phase 2 wires pixWidth to 320.
    if (width)
        *width = 320;
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

bool RenderDevice::ProcessEvents()
{
    // Phase 3 will pump SDL2 input events here.
    return isRunning;
}

void RenderDevice::InitFPSCap()
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::InitFPSCap() [stub]");
}

bool RenderDevice::CheckFPSCap()
{
    return true;
}

void RenderDevice::UpdateFPSCap()
{
    // no-op — vsync is free on MiSTer via the FPGA scan-out.
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
