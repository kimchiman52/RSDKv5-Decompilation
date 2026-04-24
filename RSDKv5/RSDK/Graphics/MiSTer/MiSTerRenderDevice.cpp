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
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Init()");
    if (!SetupRendering())
        return false;
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
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::InitGraphicsAPI() [stub]");
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
    // Quiet — called every frame. Phase 2 will wire this to NativeVideoWriter.
}

void RenderDevice::FlipScreen()
{
    // Quiet — called every frame.
}

void RenderDevice::Release(bool32 isRefresh)
{
    PrintLog(PRINT_NORMAL, "MiSTerRenderDevice::Release(isRefresh=%d) [stub]", (int)isRefresh);
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
