#ifndef ECO_SPAWN_H
#define ECO_SPAWN_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define ECO_SPAWN_INHERIT -1
#define ECO_SPAWN_DEVNULL -2
#define ECO_SPAWN_STDOUT -3

/* execve, with an optional chdir first. null or empty chdir_to keeps this process's directory.
 * child_fd[3] is -1 inherit, -2 /dev/null, -3 the child's stdout once placed,
 * else dup2 onto 0/1/2; a source below 3 is the caller's to lift first.
 * posix_spawn + addchdir_np when the libc has it; fork otherwise. returns 0 or an errno.
 * the fork child is async-signal-safe: no malloc, no Echo. */
int __eco_spawn_with_chdir(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3]);

#ifdef ECO_SPAWN_TESTING
/* always the fork path. Catch2 hits a libc that has addchdir_np, so the public helper would not. */
int __eco_spawn_with_chdir_fallback(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3]);
#endif

#ifdef __cplusplus
}
#endif

#endif
