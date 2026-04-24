#ifndef MISTER_RENDERDEVICE_H
#define MISTER_RENDERDEVICE_H

// MiSTer render device — Phase 1 skeleton, Phase 3 audio/input wiring.
//
// Mirrors SDL2RenderDevice.hpp's public surface so Drawing.hpp's backend
// dispatch (lines 245-257) can include this header without any ABI change.
// Includes <SDL2/SDL.h> for SDL_Event in ProcessEvent; no SDL types appear
// in the render-path interface (still talks to a native video writer that
// mmaps the MiSTer HPS->FPGA framebuffer directly). Pixel output landed in
// Phase 2; Phase 3 wires up SDL2 audio + input via the shared backend.

#include <SDL2/SDL.h>

#include "MiSTerPacer.hpp"

using ShaderEntry = ShaderEntryBase;

class RenderDevice : public RenderDeviceBase
{
public:
    // Simplified WindowInfo (no SDL_DisplayMode / SDL_Rect). The field names
    // (refresh_rate / width / height) are dereferenced unconditionally by
    // Drawing.cpp lines 313-347 via `displayInfo.displays[d].<field>`, so they
    // must be present with those exact names.
    //
    // NB: the SDL2 backend carries a leading `uint32 _pad` on its DisplayEntry
    // because its `displays` pointer is a union aliased to `SDL_DisplayMode`,
    // whose first field is `Uint32 format`. MiSTer has no such union — the pad
    // is intentionally omitted.
    struct WindowInfo {
        struct DisplayEntry {
            int32 width;
            int32 height;
            int32 refresh_rate;
        } *displays;
        // viewport: kept for API parity with SDL2 backend; unused in Phase 1.
        // TODO(phase-2): finalize viewport field names when NativeVideoWriter
        // arrives. Current lowercase {x,y,w,h} is a placeholder — DX9 uses
        // D3D9VIEWPORT conventions, SDL2 uses SDL_Rect. Pick whichever is
        // ergonomic for the native framebuffer path.
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
    // in Video.cpp (lines 241-258). These name-hide the 3-argument base forms
    // declared on RenderDeviceBase (Drawing.hpp:201-203) — they overload with
    // extra parameters in the derived scope. Same pattern as SDL2's override.
    static void SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);
    static void SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);
    static void SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                         int32 strideV);

    static bool ProcessEvents();
    static void ProcessEvent(SDL_Event event);

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

    // Phase 6 — pacer + overlay state. Public so the Phase-6 helper TUs and
    // ProcessEvent handler can touch them without friend-class gymnastics; the
    // rest of the engine stays oblivious because Drawing.cpp is the only file
    // that ever textually includes MiSTerRenderDevice.cpp.
    //
    // The struct type is declared in MiSTerPacer.hpp with C linkage so it
    // stays at global scope regardless of whatever C++ namespace this header
    // is included from — see the namespace note at the top of MiSTerPacer.hpp
    // for why that matters.
    static MiSTerPacerFeedbackState pacerFeedback;
    static unsigned long long       pacerFrameDeadlineNs;
    static unsigned long long       pacerTargetFrameTimeNs;

    // FPS overlay
    static bool     showFPSOverlay;
    static int32    fpsOverlayMode;      // 0=simple, 1=detailed (telemetry only)
    static unsigned long long fpsAccumNs;
    static uint32   fpsSampleCount;
    static int32    fpsLastValue;
    static char     fpsLabel[96];

    // Pacer jitter stats (for the F12 dump described in Phase 6 Step 2)
    static unsigned long long pacerMaxJitterNs;
    static unsigned long long pacerJitterSumNs;
    static uint32             pacerLateCount;
    static uint32             pacerStatsFrames;

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
