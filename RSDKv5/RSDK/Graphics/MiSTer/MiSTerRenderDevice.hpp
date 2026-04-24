#ifndef MISTER_RENDERDEVICE_H
#define MISTER_RENDERDEVICE_H

// MiSTer render device — Phase 1 skeleton.
//
// Mirrors SDL2RenderDevice.hpp's public surface so Drawing.hpp's backend
// dispatch (lines 245-257) can include this header without any ABI change.
// Deliberately free of SDL / GLFW / Vulkan types — this backend talks to a
// native video writer that mmaps the MiSTer HPS->FPGA framebuffer directly.
// Pixel output lands in Phase 2; Phase 1 stubs log-and-return.

using ShaderEntry = ShaderEntryBase;

class RenderDevice : public RenderDeviceBase
{
public:
    // Simplified WindowInfo (no SDL_DisplayMode / SDL_Rect). The field names
    // (refresh_rate / width / height) are dereferenced unconditionally by
    // Drawing.cpp lines 313-347 via `displayInfo.displays[d].<field>`, so they
    // must be present with those exact names.
    struct WindowInfo {
        struct DisplayEntry {
            uint32 _pad;
            int32 width;
            int32 height;
            int32 refresh_rate;
        } *displays;
        // viewport: kept for API parity with SDL2 backend; unused in Phase 1.
        struct {
            int32 x, y, w, h;
        } viewport;
    };
    static WindowInfo displayInfo;

    static bool Init();
    static void CopyFrameBuffer();
    static void FlipScreen();
    static void Release(bool32 isRefresh);

    static void RefreshWindow();
    static void GetWindowSize(int32 *width, int32 *height);

    static void SetupImageTexture(int32 width, int32 height, uint8 *imagePixels);
    // 8-argument YUV signatures mirror SDL2RenderDevice.hpp + the call sites
    // in Video.cpp (lines 241-258). The 3-argument forms declared on
    // RenderDeviceBase (Drawing.hpp:201-203) are shadowed; follow SDL2's
    // override here.
    static void SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);
    static void SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);
    static void SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);

    static bool ProcessEvents();

    static void InitFPSCap();
    static bool CheckFPSCap();
    static void UpdateFPSCap();

    static bool InitShaders();
    static void LoadShader(const char *fileName, bool32 linear);

    static inline void ShowCursor(bool32 shown) { (void)shown; }
    static inline bool GetCursorPos(Vector2 *pos)
    {
        (void)pos;
        return false;
    }
    static inline void SetWindowTitle() { /* no-op: MiSTer has no window chrome */ }

private:
    static bool SetupRendering();
    static void InitVertexBuffer();
    static bool InitGraphicsAPI();
    static void GetDisplays();

    static unsigned long long targetFreq;
    static unsigned long long curTicks;
    static unsigned long long prevTicks;
};

#endif // !MISTER_RENDERDEVICE_H
