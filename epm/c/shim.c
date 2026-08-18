/*
 * Platform bits Echo cannot say: readdir over an opaque handle, stat, mkdir,
 * rmdir, WEXITSTATUS, a process runner, and SHA-256.
 *
 * Nothing here is clever. The Echo side owns policy; this file is the seam.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- terminal ----------------------------------------------------------- */

int32_t epm_isatty(int32_t fd)
{
    return isatty(fd) ? 1 : 0;
}

int32_t epm_columns(int32_t fd)
{
    if (!isatty(fd)) {
        return 0;
    }

    struct winsize size;

    if (ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return (int32_t)size.ws_col;
    }

    return 0;
}

/* ---- directory listing -------------------------------------------------- */

DIR *epm_opendir(const char *path)
{
    return opendir(path);
}

/* 1 = wrote a name, 0 = done, -1 = error. skips "." and ".." */
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

/* ---- stat / mkdir / rmdir ---------------------------------------------- */

/* 0 missing, 1 file, 2 directory, -1 error */
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

/* ---- process ------------------------------------------------------------ */

typedef struct {
    char **v;
    int32_t n;
    int32_t cap;
} epm_argv;

void *epm_argv_new(void)
{
    return calloc(1, sizeof(epm_argv));
}

int32_t epm_argv_push(void *handle, const char *s)
{
    epm_argv *a = handle;

    if (a == NULL || s == NULL) {
        return -1;
    }

    if (a->n + 1 >= a->cap) {
        int32_t cap = a->cap < 8 ? 8 : a->cap * 2;
        char **grown = realloc(a->v, (size_t)cap * sizeof(char *));

        if (grown == NULL) {
            return -1;
        }

        a->v = grown;
        a->cap = cap;
    }

    char *copy = strdup(s);

    if (copy == NULL) {
        return -1;
    }

    a->v[a->n] = copy;
    a->n += 1;
    a->v[a->n] = NULL;
    return 0;
}

void epm_argv_free(void *handle)
{
    epm_argv *a = handle;

    if (a == NULL) {
        return;
    }

    int32_t i;

    for (i = 0; i < a->n; i++) {
        free(a->v[i]);
    }

    free(a->v);
    free(a);
}

static int grow(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap) {
        return 0;
    }

    size_t next = *cap < 256 ? 256 : *cap;

    while (next < need) {
        next *= 2;
    }

    char *grown = realloc(*buf, next);

    if (grown == NULL) {
        return -1;
    }

    *buf = grown;
    *cap = next;
    return 0;
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
                if (grow(&out_buf, &out_cap, out_len + (size_t)n) != 0) {
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
                if (grow(&err_buf, &err_cap, err_len + (size_t)n) != 0) {
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

void epm_free(void *p)
{
    free(p);
}

/* ---- SHA-256 ------------------------------------------------------------ */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
    size_t fill;
} epm_sha256_ctx;

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(epm_sha256_ctx *ctx, const uint8_t *block)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    size_t i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24)
            | ((uint32_t)block[i * 4 + 1] << 16)
            | ((uint32_t)block[i * 4 + 2] << 8)
            | ((uint32_t)block[i * 4 + 3]);
    }

    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void *epm_sha256_new(void)
{
    epm_sha256_ctx *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
    ctx->fill = 0;
    return ctx;
}

void epm_sha256_update(void *handle, const uint8_t *data, uint64_t n)
{
    epm_sha256_ctx *ctx = handle;
    size_t i = 0;

    ctx->bitcount += n * 8;

    while (i < n) {
        size_t room = 64 - ctx->fill;
        size_t take = (n - i) < room ? (size_t)(n - i) : room;
        memcpy(ctx->buffer + ctx->fill, data + i, take);
        ctx->fill += take;
        i += take;

        if (ctx->fill == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->fill = 0;
        }
    }
}

void epm_sha256_final(void *handle, uint8_t *out)
{
    epm_sha256_ctx *ctx = handle;
    uint64_t bits = ctx->bitcount;
    size_t i;

    ctx->buffer[ctx->fill++] = 0x80;

    if (ctx->fill > 56) {
        while (ctx->fill < 64) {
            ctx->buffer[ctx->fill++] = 0;
        }
        sha256_transform(ctx, ctx->buffer);
        ctx->fill = 0;
    }

    while (ctx->fill < 56) {
        ctx->buffer[ctx->fill++] = 0;
    }

    for (i = 0; i < 8; i++) {
        ctx->buffer[63 - i] = (uint8_t)(bits >> (i * 8));
    }
    sha256_transform(ctx, ctx->buffer);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->state[i];
    }

    free(ctx);
}
