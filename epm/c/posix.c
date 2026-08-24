/*
 * POSIX half of the epm shim: directory, stat, mkdir, a process runner.
 */

#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

DIR *epm_opendir(const char *path)
{
    return opendir(path);
}

int32_t epm_readdir(DIR *dir, char *name, uint64_t cap)
{
    if (dir == NULL || name == NULL || cap == 0) {
        return -1;
    }

    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(dir);

        if (ent == NULL) {
            return errno == 0 ? 0 : -1;
        }

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        size_t n = strlen(ent->d_name);
        if (n + 1 > cap) {
            return -1;
        }

        memcpy(name, ent->d_name, n + 1);
        return 1;
    }
}

void epm_closedir(DIR *dir)
{
    if (dir != NULL) {
        closedir(dir);
    }
}

int32_t epm_kind(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }

    return S_ISDIR(st.st_mode) ? 2 : 1;
}

int32_t epm_mkdir(const char *path)
{
    if (mkdir(path, 0755) == 0) {
        return 0;
    }

    return errno == EEXIST ? 0 : -1;
}

int32_t epm_rmdir(const char *path)
{
    return rmdir(path) == 0 ? 0 : -1;
}

int32_t epm_rename(const char *from, const char *to)
{
    if (from == NULL || to == NULL) {
        return -1;
    }

    return rename(from, to) == 0 ? 0 : -1;
}

static void drain_fail(int out_r, int err_r, pid_t pid, char *out_buf, char *err_buf)
{
    close(out_r);
    close(err_r);
    free(out_buf);
    free(err_buf);
    waitpid(pid, NULL, 0);
}

int32_t epm_run(
    void *handle,
    char **out_s, int32_t *out_n,
    char **err_s, int32_t *err_n,
    int32_t *code
)
{
    epm_argv *a = handle;

    if (a == NULL || a->n == 0 || a->v == NULL || a->v[0] == NULL) {
        return -1;
    }

    int out_pipe[2];
    int err_pipe[2];

    if (pipe(out_pipe) != 0) {
        return -1;
    }

    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvp(a->v[0], a->v);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    size_t out_cap = 256;
    size_t err_cap = 256;
    size_t out_len = 0;
    size_t err_len = 0;
    char *out_buf = malloc(out_cap);
    char *err_buf = malloc(err_cap);
    int out_eof = 0;
    int err_eof = 0;

    if (out_buf == NULL || err_buf == NULL) {
        drain_fail(out_pipe[0], err_pipe[0], pid, out_buf, err_buf);
        return -1;
    }

    while (!out_eof || !err_eof) {
        struct pollfd fds[2];
        nfds_t nfd = 0;
        int out_i = -1;
        int err_i = -1;

        if (!out_eof) {
            fds[nfd].fd = out_pipe[0];
            fds[nfd].events = POLLIN;
            out_i = (int)nfd;
            nfd += 1;
        }

        if (!err_eof) {
            fds[nfd].fd = err_pipe[0];
            fds[nfd].events = POLLIN;
            err_i = (int)nfd;
            nfd += 1;
        }

        int pr = poll(fds, nfd, -1);

        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }

            drain_fail(out_pipe[0], err_pipe[0], pid, out_buf, err_buf);
            return -1;
        }

        if (out_i >= 0 && (fds[out_i].revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[512];
            ssize_t n = read(out_pipe[0], tmp, sizeof(tmp));

            if (n > 0) {
                if (epm_grow(&out_buf, &out_cap, out_len + (size_t)n) != 0) {
                    drain_fail(out_pipe[0], err_pipe[0], pid, out_buf, err_buf);
                    return -1;
                }

                memcpy(out_buf + out_len, tmp, (size_t)n);
                out_len += (size_t)n;
            }
            else if (n == 0 || (n < 0 && errno != EINTR)) {
                out_eof = 1;
            }
        }

        if (err_i >= 0 && (fds[err_i].revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[512];
            ssize_t n = read(err_pipe[0], tmp, sizeof(tmp));

            if (n > 0) {
                if (epm_grow(&err_buf, &err_cap, err_len + (size_t)n) != 0) {
                    drain_fail(out_pipe[0], err_pipe[0], pid, out_buf, err_buf);
                    return -1;
                }

                memcpy(err_buf + err_len, tmp, (size_t)n);
                err_len += (size_t)n;
            }
            else if (n == 0 || (n < 0 && errno != EINTR)) {
                err_eof = 1;
            }
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        *code = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status)) {
        *code = 128 + WTERMSIG(status);
    }
    else {
        *code = 1;
    }

    *out_s = out_buf;
    *out_n = (int32_t)out_len;
    *err_s = err_buf;
    *err_n = (int32_t)err_len;
    return 0;
}
