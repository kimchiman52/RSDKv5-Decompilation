#ifndef MISTER_PACER_HPP
#define MISTER_PACER_HPP

// Phase 6 — Pacer / profiling helpers for the MiSTer render device.
//
// NOTE on design (namespaces): this header is transitively included by
// MiSTerRenderDevice.hpp, which Drawing.hpp includes from *inside*
// `namespace RSDK { ... }` scope (Drawing.hpp:4,258). Any `namespace Foo {}`
// opened in this header while that outer `RSDK` is active nests under
// `::RSDK::Foo`, which (a) poisons `RSDK::`-qualified lookups in the
// consuming TU (clang: "reference to 'RSDK' is ambiguous"), and (b) ODR-
// splits the same types across contexts. To sidestep both, every entity is
// declared with **C linkage and flat names** (`mister_pacer_*`, `mister_perf_*`).
// C linkage is namespace-neutral and binds to global scope regardless of
// any C++ `namespace` blocks the caller has open. The .cpp TU is standalone
// — no namespace wrappers around its bodies.
//
// Bodies in MiSTerPacer.cpp are guarded with
//   #if defined(__linux__) && defined(PORT_MISTER)
// for the MiSTer-specific hardware paths (clock_nanosleep, DDR3 reads, sysfs
// clock overrides). Non-MiSTer builds (Mac host iteration, SDL2 regression)
// still link the stub branch so the interface is uniform.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Vsync feedback + pacer phase state. Passed by pointer so the render device
// owns the storage (static member in RenderDevice); the helpers are stateless.
typedef struct MiSTerPacerFeedbackState {
    uint8_t   last_fpga_frame_cnt;
    uint32_t  last_feedback_seq;
    uint64_t  last_feedback_update_ns;   // mister_pacer_now_ns() at last successful read
    uint64_t  last_vsync_monotonic_ns;   // ditto, projection of last observed vsync
    bool      vsync_feedback_valid;
    bool      vsync_feedback_disabled;
    int64_t   pacer_phase_error_ns;      // for overlay display
} MiSTerPacerFeedbackState;

// Poll DDR3 feedback once. Returns true if state advanced (new seq observed
// and accepted). On Mac / non-MiSTer builds this is a no-op returning false.
// Safe to call even when the writer is not initialized.
bool       mister_pacer_poll_feedback(MiSTerPacerFeedbackState *fs);

// Unified ns clock — SDL2-compatible wrapper around
// SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency(). Caches the
// frequency lookup. All other pacer calls share this clock domain.
uint64_t   mister_pacer_now_ns(void);

// Sleep until absolute deadline (in mister_pacer_now_ns() domain).
//   MiSTer: clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...) coarse sleep,
//           then a ~200 μs busy-wait tail with `yield` on ARM.
//   Non-MiSTer: SDL_Delay((remaining + 999999) / 1000000) — ms-granularity.
void       mister_pacer_sleep_until_ns(uint64_t target_ns);

// ARM clock override — sysfs. Mode 0=stock (800MHz), 1=1000, 2=1200. No-op on
// non-MiSTer. Caller is responsible for registering the restore via
// std::atexit when mister_pacer_apply_arm_clock(!=0) was invoked.
// Phase 6 Step 6 (conditional overclock) — NOT wired into any CLI entry
// point until gameplay profiling confirms 800 MHz cannot sustain 60 fps.
void       mister_pacer_apply_arm_clock(int mode);
void       mister_pacer_restore_arm_clock(void);

// --- Profiling scopes (Phase 6 Step 5) ------------------------------------
// Hooks land in RetroEngine.cpp::ProcessEngine() around ENGINESTATE_REGULAR
// and in MiSTerRenderDevice::FlipScreen(). Ring-buffered over 60 frames.
#define MISTER_PERF_SCOPE_UPDATE             0
#define MISTER_PERF_SCOPE_RASTER             1
#define MISTER_PERF_SCOPE_PRESENT            2
// Sub-scopes inside RASTER, populated by ProcessObjectDrawLists. Together
// they sum (approximately) to RASTER. Used by the UFO Special Stage perf
// triage (`r:` running 15-25 ms with spikes to 30+ on the HPS Cortex-A9).
#define MISTER_PERF_SCOPE_RASTER_HOOK        3   // drawGroup hookCB callbacks
#define MISTER_PERF_SCOPE_RASTER_ENTITIES    4   // per-entity draw() loop
#define MISTER_PERF_SCOPE_RASTER_SCANLINE_CB 5   // per-tilelayer scanlineCallback
#define MISTER_PERF_SCOPE_RASTER_TILES       6   // DrawLayerHScroll/VScroll/etc
#define MISTER_PERF_SCOPE_COUNT              7

#if defined(ENABLE_PERF_TELEMETRY) && ENABLE_PERF_TELEMETRY
void   mister_perf_begin_scope(int scope);
void   mister_perf_end_scope(int scope);
double mister_perf_avg_scope_ms(int scope);

// --- UFO render triage instrumentation (Phase 12) -------------------------
// Per-frame per-class draw accumulators. Reset by mister_perf_class_reset()
// at top of ProcessObjectDrawLists, populated inline around each entity
// draw() call, dumped by mister_perf_emit_spike_log() if the frame hit a
// RASTER spike. Indexed by entity classID (uint16) so OBJECT_COUNT matches
// the engine's RSDK::OBJECT_COUNT.
#define MISTER_PERF_OBJECT_COUNT 1024
void     mister_perf_class_reset(void);
void     mister_perf_class_accum(uint16_t class_id, uint64_t ns);
uint64_t mister_perf_now_ns(void);

// --- Per-tilelayer accumulators (Phase 12) --------------------------------
// LAYER_COUNT is fixed at 8 in RSDKv5; we hard-code 16 here to leave headroom
// for any future expansion without touching the perf TU.
#define MISTER_PERF_LAYER_COUNT 16
void mister_perf_layer_reset(void);
void mister_perf_layer_accum_cb(int layer_idx, uint64_t ns);
void mister_perf_layer_accum_raster(int layer_idx, uint64_t ns);

// --- Spike log emission (Phase 12) ----------------------------------------
// Called from RetroEngine.cpp::ProcessEngine *after* end_scope(RASTER) so
// the outer scope's latest sample reflects the just-finished frame's
// RASTER total (not the previous frame's). Looks at that sample; if it
// exceeds spike_threshold_ms, emits a single PrintLog line with the
// per-class top-10 + per-layer breakdown. Also bumps a 60-frame ring of
// RASTER samples and emits a periodic "[perf-window]" summary every
// WINDOW_FRAMES frames.
//
// The mister_perf_class_name_set hook lets the engine register class IDs
// → human-readable names so the spike log can print "UFO_Sphere" instead
// of "class=42". The engine populates this from objectClassList[] at
// stage load.
//
// Frame-state hand-off: ProcessObjectDrawLists records this frame's
// visible/drawn entity counts via mister_perf_record_frame_counts() before
// returning; the spike log reads them from the same globals when it fires
// after RASTER closes.
void mister_perf_class_name_set(uint16_t class_id, const char *name);
void mister_perf_record_frame_counts(int total_visible, int total_drawn);
void mister_perf_emit_spike_log(void);

// --- End-of-run aggregate (Phase 12) --------------------------------------
// Dumps a per-class min/max/avg lifetime summary. Called from
// RenderDevice::Release(false).
void mister_perf_emit_run_summary(void);
#else
// Inline no-ops so they compile out to nothing in the clean flavor without
// adding a per-call-site #if at every instrumentation point.
static inline void   mister_perf_begin_scope(int s)  { (void)s; }
static inline void   mister_perf_end_scope(int s)    { (void)s; }
static inline double mister_perf_avg_scope_ms(int s) { (void)s; return 0.0; }
static inline void     mister_perf_class_reset(void) {}
static inline void     mister_perf_class_accum(uint16_t c, uint64_t n) { (void)c; (void)n; }
static inline uint64_t mister_perf_now_ns(void) { return 0; }
static inline void mister_perf_layer_reset(void) {}
static inline void mister_perf_layer_accum_cb(int i, uint64_t n) { (void)i; (void)n; }
static inline void mister_perf_layer_accum_raster(int i, uint64_t n) { (void)i; (void)n; }
static inline void mister_perf_class_name_set(uint16_t c, const char *n) { (void)c; (void)n; }
static inline void mister_perf_record_frame_counts(int v, int d) { (void)v; (void)d; }
static inline void mister_perf_emit_spike_log(void) {}
static inline void mister_perf_emit_run_summary(void) {}
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MISTER_PACER_HPP
