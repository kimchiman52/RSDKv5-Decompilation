#include "SigHandler.h"

#if defined(__linux__) && defined(PORT_MISTER)

#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static const char *sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        default:      return "???";
    }
}

static void fatal_handler(int sig, siginfo_t *info, void *ucontext) {
    /*
     * Async-signal-safe output. dprintf / backtrace / backtrace_symbols_fd
     * are technically AS-safe under glibc for this usage (backtrace is
     * best-effort). Re-entrancy is prevented by SIG_DFL reset at the end.
     */
    dprintf(2, "\n================ FATAL %s (%d) ================\n",
            sig_name(sig), sig);
    dprintf(2, "si_addr  = %p\n", info ? info->si_addr : (void *)0);
    dprintf(2, "si_code  = %d\n", info ? info->si_code : -1);

#if defined(__arm__)
    ucontext_t *uc = (ucontext_t *)ucontext;
    if (uc) {
        dprintf(2, "pc=%08lx lr=%08lx sp=%08lx cpsr=%08lx\n",
                (unsigned long)uc->uc_mcontext.arm_pc,
                (unsigned long)uc->uc_mcontext.arm_lr,
                (unsigned long)uc->uc_mcontext.arm_sp,
                (unsigned long)uc->uc_mcontext.arm_cpsr);
        dprintf(2, "r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
                (unsigned long)uc->uc_mcontext.arm_r0,
                (unsigned long)uc->uc_mcontext.arm_r1,
                (unsigned long)uc->uc_mcontext.arm_r2,
                (unsigned long)uc->uc_mcontext.arm_r3);
        dprintf(2, "r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
                (unsigned long)uc->uc_mcontext.arm_r4,
                (unsigned long)uc->uc_mcontext.arm_r5,
                (unsigned long)uc->uc_mcontext.arm_r6,
                (unsigned long)uc->uc_mcontext.arm_r7);
        dprintf(2, "r8=%08lx r9=%08lx r10=%08lx fp=%08lx ip=%08lx\n",
                (unsigned long)uc->uc_mcontext.arm_r8,
                (unsigned long)uc->uc_mcontext.arm_r9,
                (unsigned long)uc->uc_mcontext.arm_r10,
                (unsigned long)uc->uc_mcontext.arm_fp,
                (unsigned long)uc->uc_mcontext.arm_ip);
    }
#else
    (void)ucontext;
#endif

    void *frames[64];
    int nframes = backtrace(frames, 64);
    dprintf(2, "\n=== backtrace (%d frames) ===\n", nframes);
    backtrace_symbols_fd(frames, nframes, 2);

    /* Numeric frame addrs for addr2line resolution */
    dprintf(2, "\n=== frame addrs (for addr2line) ===\n");
    for (int i = 0; i < nframes; i++) {
        dprintf(2, "  #%d %p\n", i, frames[i]);
    }

    /* Memory map so addrs can be resolved against shared libs vs main */
    dprintf(2, "\n=== /proc/self/maps ===\n");
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            (void)!write(2, buf, (size_t)n);
        }
        close(fd);
    }

    dprintf(2, "\n================ END FATAL ================\n");

    /* Re-raise with default disposition so the process actually terminates
     * with the expected exit code (SIGSEGV -> 139 etc.). */
    signal(sig, SIG_DFL);
    raise(sig);
}

void SigHandler_Init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fatal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    (void)sigaction(SIGSEGV, &sa, NULL);
    (void)sigaction(SIGBUS,  &sa, NULL);
    (void)sigaction(SIGABRT, &sa, NULL);
    (void)sigaction(SIGFPE,  &sa, NULL);
    (void)sigaction(SIGILL,  &sa, NULL);

    dprintf(2, "SigHandler_Init: installed SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL handlers\n");
}

/* Install handlers BEFORE main() via GCC/Clang constructor attribute. This
 * is required because the wrapper-context armhf crash happens in engine
 * init code that runs *before* MiSTerRenderDevice::Init would otherwise
 * call SigHandler_Init(). The constructor runs during dynamic-linker
 * startup, ahead of main() and before any engine code executes.
 *
 * Also: chdir() to SONIC_MANIA_HOME if the env var is set (wrapper sets it
 * to /media/fat/games/sonic-mania). The upstream wrapper execve's the
 * binary with cwd=/, which means engine's relative file opens (./log.txt,
 * ./Settings.ini, ./Data.rsdk) go to rootfs, which is a read-only
 * ramdisk on MiSTer. RSDK::CreateFile returns NULL, then
 * RSDK::WriteText calls SDL_RWwrite(NULL, ...) → SIGSEGV at 0xc.
 * Chdir here as early as possible so engine's relative-path opens land
 * on the writable APP_DIR. */
__attribute__((constructor))
static void SigHandler_EarlyInit(void) {
    SigHandler_Init();
    dprintf(2, "SigHandler: early-init via __attribute__((constructor)) complete\n");

    const char *home = getenv("SONIC_MANIA_HOME");
    if (home && home[0]) {
        if (chdir(home) == 0) {
            dprintf(2, "SigHandler: chdir(\"%s\") ok\n", home);
        } else {
            dprintf(2, "SigHandler: chdir(\"%s\") FAILED errno=%d\n", home, errno);
        }
    } else {
        dprintf(2, "SigHandler: SONIC_MANIA_HOME not set; leaving cwd alone\n");
    }
}

#else /* !(linux && PORT_MISTER) */

void SigHandler_Init(void) {
    /* Non-MiSTer build: no-op. */
}

#endif
