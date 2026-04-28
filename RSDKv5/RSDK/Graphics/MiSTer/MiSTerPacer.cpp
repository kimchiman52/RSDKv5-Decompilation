// Phase 6 — Pacer / profiling implementation.
//
// Standalone TU (unlike MiSTerRenderDevice.cpp which is textually included
// by Drawing.cpp). All public entry points are C-linkage functions with
// flat `mister_pacer_*` / `mister_perf_*` names — see the namespace note in
// MiSTerPacer.hpp.
//
// Ported from 3sx-mister/src/port/sdl/sdl_app.c lines 9519-9579 (vsync
// feedback + precise_delay_ns) and 10657-10757 (closed-loop pacer call
// shape). The re-shape is narrow: the feedback state struct replaces
// module-level statics in sdl_app.c so the render device owns the storage
// and tests can inject their own.

#include "MiSTerPacer.hpp"

#include <SDL2/SDL.h>
#include <stdio.h>

#if defined(__linux__) && defined(PORT_MISTER)
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#include "NativeVideoWriter.h"

// ---------------------------------------------------------------------------
// Clock helpers. SDL2 has no SDL_GetTicksNS / SDL_DelayNS; those are
// SDL3-only. Build our own on SDL_GetPerformanceCounter(). File-static to
// avoid polluting global scope.
// ---------------------------------------------------------------------------

static uint64_t PerfFreqCached(void)
{
    static uint64_t freq = 0;
    if (freq == 0) {
        freq = (uint64_t)SDL_GetPerformanceFrequency();
        if (freq == 0) freq = 1;  // defensive — SDL guarantees nonzero
    }
    return freq;
}

static uint64_t PerfNowNs(void)
{
    uint64_t ticks = (uint64_t)SDL_GetPerformanceCounter();
    uint64_t freq  = PerfFreqCached();
    // Compute ticks * 1e9 / freq without overflow for realistic ranges.
    uint64_t secs = ticks / freq;
    uint64_t rem  = ticks % freq;
    return secs * 1000000000ULL + (rem * 1000000000ULL) / freq;
}

extern "C" uint64_t mister_pacer_now_ns(void)
{
    return PerfNowNs();
}

// ---------------------------------------------------------------------------
// PacerSleepUntilNs — platform dispatch
// ---------------------------------------------------------------------------

#if defined(__linux__) && defined(PORT_MISTER)

static const uint64_t kBusyWaitThresholdNs = 200000ULL;  // 200 μs — matches 3sx default

// Convert PerfNowNs() (PerformanceCounter) domain to CLOCK_MONOTONIC for
// clock_nanosleep. Trivial on most Linux kernels (SDL_GetPerformanceCounter
// on POSIX is usually CLOCK_MONOTONIC or CLOCK_MONOTONIC_RAW), but rather
// than assume, compute a one-shot offset between the two domains.
//
// P-2.5 review note: the offset is computed exactly once on first call and
// cached for the process lifetime. This is safe because, on Linux SDL2
// builds, both CLOCK_MONOTONIC and CLOCK_MONOTONIC_RAW (whichever
// SDL_GetPerformanceCounter ultimately wraps) advance at the SAME rate as
// each other and as the kernel's hrtimer clock — drift between the two
// reads is bounded by the read latency (sub-microsecond). They are not
// rate-adjusted at runtime (NTP slewing applies only to CLOCK_REALTIME).
// If a future SDL2 / kernel combination ever broke this assumption (e.g.
// SDL switching to CLOCK_BOOTTIME and the kernel suspending mid-run), the
// pacer would gradually drift relative to the FPGA scan-out and the
// vsync-feedback closed loop in UpdateFPSCap would have to re-snap each
// frame — observable as a pacer_phase_error_ns trend in the F12 jitter
// dump. Re-evaluate this caching if that telemetry ever shows it.
static int64_t MonoToPacerOffsetNs(void)
{
    static bool initialized = false;
    static int64_t offset = 0;
    if (!initialized) {
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        uint64_t mono_ns  = (uint64_t)mono.tv_sec * 1000000000ULL + (uint64_t)mono.tv_nsec;
        uint64_t pacer_ns = PerfNowNs();
        // offset = pacer - mono  →  mono = pacer - offset.
        offset = (int64_t)pacer_ns - (int64_t)mono_ns;
        initialized = true;
    }
    return offset;
}

extern "C" void mister_pacer_sleep_until_ns(uint64_t target_pacer_ns)
{
    uint64_t now = PerfNowNs();
    if (now >= target_pacer_ns) return;
    uint64_t remaining = target_pacer_ns - now;

    if (remaining > kBusyWaitThresholdNs) {
        int64_t offset = MonoToPacerOffsetNs();
        int64_t abs_mono_ns = (int64_t)target_pacer_ns - offset - (int64_t)kBusyWaitThresholdNs;
        if (abs_mono_ns < 0) abs_mono_ns = 0;

        struct timespec ts;
        ts.tv_sec  = (time_t)(abs_mono_ns / 1000000000LL);
        ts.tv_nsec = (long)(abs_mono_ns % 1000000000LL);

        int rc;
        do {
            rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        } while (rc == EINTR);
        // rc nonzero (other than EINTR) means the kernel rejected the args;
        // fall through to the busy-wait loop rather than spin forever.
    }

    // Busy-wait tail — yield on ARM to avoid hammering the bus.
    while (PerfNowNs() < target_pacer_ns) {
#if defined(__arm__) || defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
}

#else  // !MiSTer — Mac host, SDL2 regression, etc.

extern "C" void mister_pacer_sleep_until_ns(uint64_t target_pacer_ns)
{
    uint64_t now = PerfNowNs();
    if (now >= target_pacer_ns) return;
    uint64_t remaining_ns = target_pacer_ns - now;
    Uint32 ms = (Uint32)((remaining_ns + 999999ULL) / 1000000ULL);
    if (ms == 0) ms = 1;
    SDL_Delay(ms);
}

#endif

// ---------------------------------------------------------------------------
// mister_pacer_poll_feedback — 3sx poll_vsync_feedback() ported.
// ---------------------------------------------------------------------------

extern "C" bool mister_pacer_poll_feedback(MiSTerPacerFeedbackState *fs)
{
    if (!fs || fs->vsync_feedback_disabled) return false;

#if defined(__linux__) && defined(PORT_MISTER)
    if (!NativeVideoWriter_IsActive()) return false;

    // Triple-read seq bracket for torn-read detection.
    uint32_t seq1 = NativeVideoWriter_ReadFeedbackSeq();
    uint32_t word = NativeVideoWriter_ReadFeedback();
    uint32_t seq2 = NativeVideoWriter_ReadFeedbackSeq();

    if (seq1 != seq2 || seq1 == 0) return false;  // torn or uninitialized
    if (seq1 == fs->last_feedback_seq) return false;  // no new data

    fs->last_feedback_seq = seq1;

    uint8_t  cnt   = NV_FeedbackFrameCounter(word);
    uint32_t ts_us = NV_FeedbackTimestampUs(word);

    if (cnt == fs->last_fpga_frame_cnt) return false;  // same counter

    struct timespec mono_ts;
    clock_gettime(CLOCK_MONOTONIC, &mono_ts);
    uint32_t mono_us = (uint32_t)((uint64_t)mono_ts.tv_sec * 1000000ULL +
                                  (uint64_t)mono_ts.tv_nsec / 1000ULL) & 0x00FFFFFFu;
    int32_t delta_us = (int32_t)((mono_us - ts_us) & 0x00FFFFFFu);
    if (delta_us > 0x00800000) delta_us -= 0x01000000;  // 24-bit signed wrap

    if (delta_us < 0 || delta_us >= 200000) {
        // Stale / bogus (normal is ~1 ms). Skip.
        return false;
    }

    uint64_t now_ns = PerfNowNs();
    uint64_t delta_ns = (uint64_t)delta_us * 1000ULL;
    if (delta_ns > now_ns) {
        return false;
    }
    uint64_t vsync_time_ns = now_ns - delta_ns;

    bool was_invalid = !fs->vsync_feedback_valid;

    fs->last_fpga_frame_cnt     = cnt;
    fs->last_vsync_monotonic_ns = vsync_time_ns;
    fs->last_feedback_update_ns = now_ns;
    fs->vsync_feedback_valid    = true;

    if (was_invalid) {
        fprintf(stderr,
                "[MiSTerPacer] vsync feedback engaged (delta=%d us, seq=%u)\n",
                (int)delta_us, (unsigned)seq1);
        fflush(stderr);
    }
    return true;
#else
    (void)fs;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// ARM clock override — gated, optional. Phase 6 Step 6.
// Not wired into any entry point until gameplay profiling justifies it.
// ---------------------------------------------------------------------------

#if defined(__linux__) && defined(PORT_MISTER)

static bool SysfsWrite(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);
    return (n == (ssize_t)len);
}

static bool SysfsRead(const char *path, char *buf, size_t buf_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) return false;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
    buf[n] = '\0';
    return true;
}

extern "C" void mister_pacer_apply_arm_clock(int mode)
{
    const char *freq = "800000";
    if (mode == 1) freq = "1000000";
    else if (mode == 2) freq = "1200000";

    SysfsWrite("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "performance");

    char cur_max[32] = {};
    bool have_cur = SysfsRead("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
                              cur_max, sizeof(cur_max));
    long target  = strtol(freq, NULL, 10);
    long current = have_cur ? strtol(cur_max, NULL, 10) : 0;

    if (target >= current) {
        if (!SysfsWrite("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", freq)) {
            fprintf(stderr, "[MiSTerPacer] apply_arm_clock: scaling_max_freq write failed\n");
            return;
        }
        SysfsWrite("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", freq);
    } else {
        SysfsWrite("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", freq);
        if (!SysfsWrite("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", freq)) {
            fprintf(stderr, "[MiSTerPacer] apply_arm_clock: scaling_max_freq write failed\n");
            return;
        }
    }
}

extern "C" void mister_pacer_restore_arm_clock(void)
{
    mister_pacer_apply_arm_clock(0);
}

#else

extern "C" void mister_pacer_apply_arm_clock(int mode) { (void)mode; }
extern "C" void mister_pacer_restore_arm_clock(void)   {}

#endif

// ---------------------------------------------------------------------------
// Perf scope ring buffer — Phase 6 Step 5.
// Only compiled when ENABLE_PERF_TELEMETRY=1; otherwise the header's inline
// stubs provide no-op bodies.
// ---------------------------------------------------------------------------

#if defined(ENABLE_PERF_TELEMETRY) && ENABLE_PERF_TELEMETRY

namespace {

constexpr int kRingSize = 60;

struct ScopeRing {
    uint64_t start_ns;
    uint64_t samples[kRingSize];   // nanoseconds per frame
    int      write_idx;
    int      filled;
};

ScopeRing g_rings[MISTER_PERF_SCOPE_COUNT] = {};

}  // anonymous namespace

extern "C" void mister_perf_begin_scope(int scope)
{
    if (scope < 0 || scope >= MISTER_PERF_SCOPE_COUNT) return;
    // P-2.4 review fix: detect unpaired begin/begin (a missing end_scope on
    // the previous frame would silently fold a multi-frame sample into the
    // next one). Telemetry-only — never enabled in clean release flow.
    if (g_rings[scope].start_ns != 0) {
        fprintf(stderr,
                "[MiSTerPerf] WARNING: begin_scope(%d) called with start_ns "
                "already set; previous end_scope missing. Frame sample for "
                "this scope will be dropped.\n",
                scope);
        fflush(stderr);
    }
    g_rings[scope].start_ns = PerfNowNs();
}

extern "C" void mister_perf_end_scope(int scope)
{
    if (scope < 0 || scope >= MISTER_PERF_SCOPE_COUNT) return;
    ScopeRing &r = g_rings[scope];
    if (r.start_ns == 0) return;   // EndScope without BeginScope — ignore.
    uint64_t now = PerfNowNs();
    uint64_t delta = (now > r.start_ns) ? (now - r.start_ns) : 0;
    r.samples[r.write_idx] = delta;
    r.write_idx = (r.write_idx + 1) % kRingSize;
    if (r.filled < kRingSize) r.filled++;
    r.start_ns = 0;
}

extern "C" double mister_perf_avg_scope_ms(int scope)
{
    if (scope < 0 || scope >= MISTER_PERF_SCOPE_COUNT) return 0.0;
    const ScopeRing &r = g_rings[scope];
    if (r.filled == 0) return 0.0;
    uint64_t sum = 0;
    for (int i = 0; i < r.filled; ++i) sum += r.samples[i];
    return (double)sum / (double)r.filled / 1e6;
}

// =========================================================================
// UFO render triage instrumentation (Phase 12)
// =========================================================================
//
// Goal: in a single telemetry run, capture enough data to pin down which
// part of ProcessObjectDrawLists is responsible for the 30 ms RASTER spikes
// observed in UFO Special Stage 1. We don't want to ship multiple
// instrumentation rounds — this is supposed to be the one cycle that
// answers "where is the time going?".
//
// What gets captured:
//  - Per-frame per-class draw cost (ns) + count, indexed by classID
//  - Per-frame per-tilelayer scanlineCallback + raster cost (ns)
//  - Per-frame visible-vs-drawn entity count
//  - Per-spike (RASTER > 18 ms) one detail line: top-10 classes, layer
//    breakdown, sub-scope split, entity counts
//  - Per-window (every 60 frames) summary: avg/max/p95 RASTER + spike count
//  - Run summary on Release: per-class lifetime min/max/avg + total spike
//    contribution

namespace RSDK {
// Forward-declared from Dev/Debug.hpp so we can land output in the same
// userFileDir/log.txt where the engine's PrintLog stream lives, not the
// short-lived osd-wrapper.log that fprintf(stderr) hits via the wrapper's
// stdio dup2. Use plain `int` to exactly match the underlying type of
// RSDK's `int32` typedef (`typedef signed int int32` from
// RetroEngine.hpp:19) so the C++ name mangling matches the engine's
// definition. Don't substitute `int32_t` here even if it's identical
// today — Itanium ABI mangles by canonical type name and a future stdint
// swap could silently de-link.
void PrintLog(int mode, const char *message, ...);
}  // namespace RSDK
static constexpr int kPrintNormal = 0;

namespace {

// --- Per-class accumulators (current frame + lifetime) --------------------
struct ClassPerf {
    // Current-frame:
    uint64_t cur_ns;
    uint32_t cur_count;
    // Lifetime (across all frames where this class drew at least once):
    uint64_t life_total_ns;
    uint64_t life_max_frame_ns;
    uint32_t life_frame_count;
    uint32_t life_total_draws;
    // Class name (registered by Object.cpp from objectClassList[].name).
    char name[32];
};
ClassPerf g_class[MISTER_PERF_OBJECT_COUNT] = {};

// --- Per-layer accumulators (current frame) -------------------------------
struct LayerPerf {
    uint64_t cur_cb_ns;
    uint64_t cur_raster_ns;
    uint64_t life_cb_ns;
    uint64_t life_raster_ns;
    uint32_t life_frame_count;
};
LayerPerf g_layer[MISTER_PERF_LAYER_COUNT] = {};

// --- Window accumulator (60-frame rolling stats) --------------------------
constexpr int kWindowFrames = 60;
struct WindowState {
    uint64_t raster_samples[kWindowFrames];
    int      write_idx;
    int      filled;
    uint32_t spike_count;
    uint32_t total_frames;       // monotonic frame counter for [perf-spike] f=N
    uint64_t spike_threshold_ns; // 8 ms by default — UFO baseline is 5-8 ms
                                 // r:avg per-window, peaks 15-25, busy 30+;
                                 // 8 ms catches the busy half without flooding.
};
WindowState g_window = { {}, 0, 0, 0, 0, 8'000'000ULL };

// Per-frame entity count hand-off from ProcessObjectDrawLists →
// mister_perf_emit_spike_log (which now fires AFTER RASTER closes, so
// counts must be stashed before the function returns).
int g_last_visible = 0;
int g_last_drawn   = 0;

}  // anonymous namespace

extern "C" uint64_t mister_perf_now_ns(void) { return PerfNowNs(); }

extern "C" void mister_perf_class_reset(void)
{
    // Don't memset the whole array — we want lifetime values preserved.
    // Reset only cur_ns / cur_count.
    for (int i = 0; i < MISTER_PERF_OBJECT_COUNT; ++i) {
        g_class[i].cur_ns    = 0;
        g_class[i].cur_count = 0;
    }
}

extern "C" void mister_perf_class_accum(uint16_t class_id, uint64_t ns)
{
    if (class_id >= MISTER_PERF_OBJECT_COUNT) return;
    g_class[class_id].cur_ns    += ns;
    g_class[class_id].cur_count += 1;
}

extern "C" void mister_perf_class_name_set(uint16_t class_id, const char *name)
{
    if (class_id >= MISTER_PERF_OBJECT_COUNT || !name) return;
    // Idempotent — only set on first call so we don't flood with re-sets
    // every stage load. snprintf bounds at sizeof-1 to leave room for NUL.
    if (g_class[class_id].name[0] == '\0') {
        SDL_snprintf(g_class[class_id].name, sizeof(g_class[class_id].name),
                     "%s", name);
    }
}

extern "C" void mister_perf_layer_reset(void)
{
    for (int i = 0; i < MISTER_PERF_LAYER_COUNT; ++i) {
        g_layer[i].cur_cb_ns     = 0;
        g_layer[i].cur_raster_ns = 0;
    }
}

extern "C" void mister_perf_layer_accum_cb(int layer_idx, uint64_t ns)
{
    if (layer_idx < 0 || layer_idx >= MISTER_PERF_LAYER_COUNT) return;
    g_layer[layer_idx].cur_cb_ns += ns;
}

extern "C" void mister_perf_layer_accum_raster(int layer_idx, uint64_t ns)
{
    if (layer_idx < 0 || layer_idx >= MISTER_PERF_LAYER_COUNT) return;
    g_layer[layer_idx].cur_raster_ns += ns;
}

namespace {

// Latest sample helper — reads the most-recently-written sample from a
// scope ring (the one mister_perf_end_scope just wrote). Returns 0 if the
// ring is empty.
uint64_t latest_scope_sample(int scope)
{
    if (scope < 0 || scope >= MISTER_PERF_SCOPE_COUNT) return 0;
    const ScopeRing &r = g_rings[scope];
    if (r.filled == 0) return 0;
    int last = (r.write_idx - 1 + kRingSize) % kRingSize;
    return r.samples[last];
}

// Sort top-N indices by g_class[i].cur_ns descending. Output goes into
// out_idx[] which is at least n elements. Stable selection sort — O(N*n)
// but N=OBJECT_COUNT=1024 and n=10 so it's fine vs. full sort.
int select_top_classes(int *out_idx, int n)
{
    bool used[MISTER_PERF_OBJECT_COUNT] = {};
    int found = 0;
    for (int slot = 0; slot < n; ++slot) {
        int best = -1;
        uint64_t best_ns = 0;
        for (int i = 0; i < MISTER_PERF_OBJECT_COUNT; ++i) {
            if (used[i]) continue;
            if (g_class[i].cur_ns > best_ns) {
                best_ns = g_class[i].cur_ns;
                best    = i;
            }
        }
        if (best < 0 || best_ns == 0) break;
        used[best] = true;
        out_idx[slot] = best;
        ++found;
    }
    return found;
}

double ns_to_ms(uint64_t ns) { return (double)ns / 1.0e6; }

}  // anonymous namespace

extern "C" void mister_perf_record_frame_counts(int total_visible, int total_drawn)
{
    g_last_visible = total_visible;
    g_last_drawn   = total_drawn;
}

extern "C" void mister_perf_emit_spike_log(void)
{
    // Called from RetroEngine.cpp AFTER end_scope(RASTER), so the latest
    // RASTER sample is THIS frame's outer total. Sum-of-subscopes is also
    // emitted alongside as a sanity check / "uninstrumented work" delta:
    // the difference (RASTER - sum) tells us how much time lives in the
    // ProcessObjectDrawLists bookkeeping (sort, mod callbacks, clip
    // resets, debug-overlay paths) vs. the timed sub-scopes.
    uint64_t r_ns  = latest_scope_sample(MISTER_PERF_SCOPE_RASTER);
    uint64_t hk_ns = latest_scope_sample(MISTER_PERF_SCOPE_RASTER_HOOK);
    uint64_t en_ns = latest_scope_sample(MISTER_PERF_SCOPE_RASTER_ENTITIES);
    uint64_t cb_ns = latest_scope_sample(MISTER_PERF_SCOPE_RASTER_SCANLINE_CB);
    uint64_t tl_ns = latest_scope_sample(MISTER_PERF_SCOPE_RASTER_TILES);
    uint64_t sum_subs = hk_ns + en_ns + cb_ns + tl_ns;
    uint64_t u_ns = latest_scope_sample(MISTER_PERF_SCOPE_UPDATE);
    uint64_t p_ns = latest_scope_sample(MISTER_PERF_SCOPE_PRESENT);
    int total_visible = g_last_visible;
    int total_drawn   = g_last_drawn;

    g_window.raster_samples[g_window.write_idx] = r_ns;
    g_window.write_idx = (g_window.write_idx + 1) % kWindowFrames;
    if (g_window.filled < kWindowFrames) g_window.filled++;
    g_window.total_frames++;

    // Roll per-class current frame into lifetime stats.
    for (int i = 0; i < MISTER_PERF_OBJECT_COUNT; ++i) {
        if (g_class[i].cur_ns == 0) continue;
        g_class[i].life_total_ns    += g_class[i].cur_ns;
        g_class[i].life_total_draws += g_class[i].cur_count;
        g_class[i].life_frame_count += 1;
        if (g_class[i].cur_ns > g_class[i].life_max_frame_ns) {
            g_class[i].life_max_frame_ns = g_class[i].cur_ns;
        }
    }
    // Roll per-layer current frame into lifetime.
    for (int i = 0; i < MISTER_PERF_LAYER_COUNT; ++i) {
        if (g_layer[i].cur_cb_ns == 0 && g_layer[i].cur_raster_ns == 0) continue;
        g_layer[i].life_cb_ns     += g_layer[i].cur_cb_ns;
        g_layer[i].life_raster_ns += g_layer[i].cur_raster_ns;
        g_layer[i].life_frame_count += 1;
    }

    // -------------------- spike log --------------------
    if (r_ns >= g_window.spike_threshold_ns) {
        g_window.spike_count++;

        // Build top-N class string.
        int top_idx[10];
        int top_n = select_top_classes(top_idx, 10);
        char top_buf[512];
        int  top_off = 0;
        top_buf[0] = '\0';
        for (int k = 0; k < top_n; ++k) {
            int i = top_idx[k];
            const char *name = g_class[i].name[0] ? g_class[i].name : "(unnamed)";
            int n = SDL_snprintf(top_buf + top_off, sizeof(top_buf) - top_off,
                                 "%s%s=%.2fms(%u)",
                                 (k == 0 ? "" : " "),
                                 name,
                                 ns_to_ms(g_class[i].cur_ns),
                                 g_class[i].cur_count);
            if (n < 0 || (size_t)n >= sizeof(top_buf) - top_off) break;
            top_off += n;
        }

        // Build per-layer breakdown (only layers that did anything).
        char layer_buf[256];
        int  layer_off = 0;
        layer_buf[0] = '\0';
        for (int i = 0; i < MISTER_PERF_LAYER_COUNT; ++i) {
            if (g_layer[i].cur_cb_ns == 0 && g_layer[i].cur_raster_ns == 0) continue;
            int n = SDL_snprintf(layer_buf + layer_off, sizeof(layer_buf) - layer_off,
                                 "%sL%d=cb%.2f/r%.2f",
                                 (layer_off == 0 ? "" : " "),
                                 i,
                                 ns_to_ms(g_layer[i].cur_cb_ns),
                                 ns_to_ms(g_layer[i].cur_raster_ns));
            if (n < 0 || (size_t)n >= sizeof(layer_buf) - layer_off) break;
            layer_off += n;
        }

        RSDK::PrintLog(kPrintNormal,
            "[perf-spike] f=%u r=%.2fms sum=%.2f delta=%.2f "
            "(h=%.2f cb=%.2f ents=%.2f tiles=%.2f) u=%.2f p=%.2f "
            "vis=%d drawn=%d ents-top10: %s | layers: %s",
            g_window.total_frames,
            ns_to_ms(r_ns), ns_to_ms(sum_subs),
            ns_to_ms(r_ns > sum_subs ? r_ns - sum_subs : 0),
            ns_to_ms(hk_ns), ns_to_ms(cb_ns),
            ns_to_ms(en_ns), ns_to_ms(tl_ns),
            ns_to_ms(u_ns), ns_to_ms(p_ns),
            total_visible, total_drawn,
            top_buf, layer_buf);
    }

    // -------------------- periodic window summary ------
    if (g_window.total_frames % kWindowFrames == 0 && g_window.filled > 0) {
        // avg, max, p95 over the ring.
        uint64_t sum = 0, mx = 0;
        uint64_t sorted[kWindowFrames];
        for (int i = 0; i < g_window.filled; ++i) {
            uint64_t s = g_window.raster_samples[i];
            sum += s;
            if (s > mx) mx = s;
            sorted[i] = s;
        }
        // simple insertion sort for p95
        for (int i = 1; i < g_window.filled; ++i) {
            uint64_t v = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; }
            sorted[j + 1] = v;
        }
        int p95_idx = (int)((double)g_window.filled * 0.95);
        if (p95_idx >= g_window.filled) p95_idx = g_window.filled - 1;
        double avg_ms = ns_to_ms(sum / g_window.filled);
        double max_ms = ns_to_ms(mx);
        double p95_ms = ns_to_ms(sorted[p95_idx]);

        RSDK::PrintLog(kPrintNormal,
            "[perf-window] f=%u last%df r:avg=%.2f max=%.2f p95=%.2f "
            "spikes(>=%.0fms)=%u",
            g_window.total_frames,
            g_window.filled,
            avg_ms, max_ms, p95_ms,
            ns_to_ms(g_window.spike_threshold_ns),
            g_window.spike_count);
        // Don't reset spike_count — it's a monotonic per-run counter; the
        // delta-since-last-window is implied by the periodic emission.
    }
}

extern "C" void mister_perf_emit_run_summary(void)
{
    if (g_window.total_frames == 0) return;

    // Build a top-30 class lifetime ranking by total ns.
    bool used[MISTER_PERF_OBJECT_COUNT] = {};
    RSDK::PrintLog(kPrintNormal,
        "[perf-run] frames=%u spikes>=%.0fms=%u  per-class top by total render time:",
        g_window.total_frames,
        ns_to_ms(g_window.spike_threshold_ns),
        g_window.spike_count);

    for (int slot = 0; slot < 30; ++slot) {
        int best = -1;
        uint64_t best_ns = 0;
        for (int i = 0; i < MISTER_PERF_OBJECT_COUNT; ++i) {
            if (used[i]) continue;
            if (g_class[i].life_total_ns > best_ns) {
                best_ns = g_class[i].life_total_ns;
                best    = i;
            }
        }
        if (best < 0 || best_ns == 0) break;
        used[best] = true;

        const char *name = g_class[best].name[0] ? g_class[best].name : "(unnamed)";
        double avg_per_active = (g_class[best].life_frame_count == 0) ? 0.0
            : ns_to_ms(g_class[best].life_total_ns / g_class[best].life_frame_count);
        double avg_per_draw   = (g_class[best].life_total_draws == 0) ? 0.0
            : ns_to_ms(g_class[best].life_total_ns / g_class[best].life_total_draws);
        RSDK::PrintLog(kPrintNormal,
            "  %2d. %-24s total=%.2fms framesActive=%u avgFrame=%.3fms "
            "maxFrame=%.3fms draws=%u avgDraw=%.4fms",
            slot + 1, name,
            ns_to_ms(g_class[best].life_total_ns),
            g_class[best].life_frame_count,
            avg_per_active,
            ns_to_ms(g_class[best].life_max_frame_ns),
            g_class[best].life_total_draws,
            avg_per_draw);
    }

    RSDK::PrintLog(kPrintNormal,
        "[perf-run] per-layer lifetime:");
    for (int i = 0; i < MISTER_PERF_LAYER_COUNT; ++i) {
        if (g_layer[i].life_frame_count == 0) continue;
        double cb_avg = ns_to_ms(g_layer[i].life_cb_ns / g_layer[i].life_frame_count);
        double r_avg  = ns_to_ms(g_layer[i].life_raster_ns / g_layer[i].life_frame_count);
        RSDK::PrintLog(kPrintNormal,
            "  L%d  framesActive=%u avgFrameCb=%.3fms avgFrameRaster=%.3fms",
            i, g_layer[i].life_frame_count, cb_avg, r_avg);
    }
}

#endif  // ENABLE_PERF_TELEMETRY
