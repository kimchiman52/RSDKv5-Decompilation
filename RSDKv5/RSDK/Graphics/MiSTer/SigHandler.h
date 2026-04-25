#ifndef RSDK_MISTER_SIG_HANDLER_H
#define RSDK_MISTER_SIG_HANDLER_H

/*
 * SigHandler — installs fatal-signal handlers that print a backtrace to
 * stderr before letting the process die. Used to diagnose wrapper-context
 * crashes on armhf that gdb masks (timing / ptrace differences).
 *
 * Output goes to stderr which the Mania wrapper dup2's to
 * /media/fat/games/sonic-mania/logs/last-run.log.
 */

#ifdef __cplusplus
extern "C" {
#endif

void SigHandler_Init(void);

#ifdef __cplusplus
}
#endif

#endif
