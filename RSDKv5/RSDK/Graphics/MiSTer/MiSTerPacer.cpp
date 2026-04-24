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

#endif  // ENABLE_PERF_TELEMETRY
