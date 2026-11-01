#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "eco_spawn.h"

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NSIG
#define NSIG 65
#endif

#if defined(__GLIBC__)
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *, const char *)
    __attribute__((weak));
#else
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *, const char *);
#endif

static int write_errno(int fd, int code)
{
    const char *p = (const char *)&code;
    size_t left = sizeof(code);

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        p += n;
        left -= (size_t)n;
    }

    return 0;
}

static int read_errno(int fd, int *code)
{
    char *p = (char *)code;
    size_t got = 0;

    while (got < sizeof(*code)) {
        ssize_t n = read(fd, p + got, sizeof(*code) - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (n == 0) {
            return (int)got;
        }

        got += (size_t)n;
    }

    return (int)got;
}

static int has_chdir(const char *chdir_to)
{
    return chdir_to != NULL && chdir_to[0] != '\0';
}

static int add_stdio_actions(posix_spawn_file_actions_t *actions, const int child_fd[3])
{
    for (int i = 0; i < 3; i++) {
        int src = child_fd[i];
        int rc = 0;

        if (src == ECO_SPAWN_INHERIT) {
            continue;
        }

        if (src == ECO_SPAWN_STDOUT) {
            if (i == STDOUT_FILENO) {
                continue;
            }

            src = STDOUT_FILENO;
        }

        if (src == ECO_SPAWN_DEVNULL) {
            int flags = (i == 0) ? O_RDONLY : O_WRONLY;
            rc = posix_spawn_file_actions_addopen(actions, i, "/dev/null", flags, 0);
        }
        else {
            rc = posix_spawn_file_actions_adddup2(actions, src, i);
        }

        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

static int spawn_posix_chdir(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3])
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        return rc;
    }

    rc = posix_spawnattr_init(&attr);
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return rc;
    }

    rc = add_stdio_actions(&actions, child_fd);
    if (rc == 0 && has_chdir(chdir_to)) {
        rc = posix_spawn_file_actions_addchdir_np(&actions, chdir_to);
    }

    sigset_t sigdefault;
    sigset_t sigmask;
    sigemptyset(&sigdefault);
    sigaddset(&sigdefault, SIGPIPE);
    sigemptyset(&sigmask);

    if (rc == 0) {
        rc = posix_spawnattr_setsigdefault(&attr, &sigdefault);
    }

    if (rc == 0) {
        rc = posix_spawnattr_setsigmask(&attr, &sigmask);
    }

    if (rc == 0) {
        rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    }

    pid_t pid = 0;
    if (rc == 0) {
        rc = posix_spawn(&pid, path, &actions, &attr, argv, envp);
    }

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0) {
        return rc;
    }

    *pid_out = (int)pid;
    return 0;
}

static void reset_caught_signals(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;

    for (int sig = 1; sig < NSIG; sig++) {
        struct sigaction old;
        if (sigaction(sig, 0, &old) != 0) {
            continue;
        }

        if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            sigaction(sig, &sa, 0);
        }
    }

    sigaction(SIGPIPE, &sa, 0);
}

static int dup2_retrying(int from, int to)
{
    while (dup2(from, to) < 0) {
        if (errno != EINTR) {
            return errno;
        }
    }

    return 0;
}

static int place_child_fds(const int child_fd[3])
{
    for (int i = 0; i < 3; i++) {
        int src = child_fd[i];
        if (src == ECO_SPAWN_INHERIT) {
            continue;
        }

        if (src == ECO_SPAWN_DEVNULL) {
            int n = open("/dev/null", i == 0 ? O_RDONLY : O_WRONLY);
            if (n < 0) {
                return errno;
            }

            if (n == i) {
                continue;
            }

            int rc = dup2_retrying(n, i);
            close(n);
            if (rc != 0) {
                return rc;
            }

            continue;
        }

        if (src == ECO_SPAWN_STDOUT) {
            src = STDOUT_FILENO;
        }

        if (src != i) {
            int rc = dup2_retrying(src, i);
            if (rc != 0) {
                return rc;
            }
        }
    }

    return 0;
}

static int spawn_fork_chdir(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3])
{
    int errpipe[2];
    if (pipe2(errpipe, O_CLOEXEC) != 0) {
        return errno;
    }

    sigset_t all;
    sigset_t old;
    sigfillset(&all);
    int masked = pthread_sigmask(SIG_BLOCK, &all, &old);
    if (masked != 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return masked;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int code = errno;
        pthread_sigmask(SIG_SETMASK, &old, 0);
        close(errpipe[0]);
        close(errpipe[1]);
        return code;
    }

    if (pid == 0) {
        close(errpipe[0]);

        int rc = place_child_fds(child_fd);
        if (rc != 0) {
            write_errno(errpipe[1], rc);
            _exit(127);
        }

        if (has_chdir(chdir_to) && chdir(chdir_to) != 0) {
            write_errno(errpipe[1], errno);
            _exit(127);
        }

        reset_caught_signals();
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, 0);

        execve(path, argv, envp);
        write_errno(errpipe[1], errno);
        _exit(127);
    }

    pthread_sigmask(SIG_SETMASK, &old, 0);
    close(errpipe[1]);
    int code = 0;
    int n = read_errno(errpipe[0], &code);
    close(errpipe[0]);

    if (n == (int)sizeof(code)) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        *pid_out = (int)pid;
        return code;
    }

    if (n != 0) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        *pid_out = (int)pid;
        return EIO;
    }

    *pid_out = (int)pid;
    return 0;
}

int __eco_spawn_with_chdir(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3])
{
#if defined(__GLIBC__)
    if (posix_spawn_file_actions_addchdir_np) {
        return spawn_posix_chdir(pid_out, path, argv, envp, chdir_to, child_fd);
    }

    return spawn_fork_chdir(pid_out, path, argv, envp, chdir_to, child_fd);
#else
    return spawn_posix_chdir(pid_out, path, argv, envp, chdir_to, child_fd);
#endif
}

#ifdef ECO_SPAWN_TESTING
int __eco_spawn_with_chdir_fallback(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3])
{
    return spawn_fork_chdir(pid_out, path, argv, envp, chdir_to, child_fd);
}
#endif

#else

int __eco_spawn_with_chdir(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3])
{
    (void)pid_out;
    (void)path;
    (void)argv;
    (void)envp;
    (void)chdir_to;
    (void)child_fd;
    return 38;
}

#ifdef ECO_SPAWN_TESTING
int __eco_spawn_with_chdir_fallback(
    int *pid_out,
    const char *path,
    char *const argv[],
    char *const envp[],
    const char *chdir_to,
    const int child_fd[3])
{
    return __eco_spawn_with_chdir(pid_out, path, argv, envp, chdir_to, child_fd);
}
#endif

#endif
