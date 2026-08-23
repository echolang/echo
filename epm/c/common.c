/*
 * Platform-independent bits of the epm shim: argv builder, allocator, SHA-256.
 */

#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

#ifdef _WIN32
    char *copy = _strdup(s);
#else
    char *copy = strdup(s);
#endif

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

int epm_grow(char **buf, size_t *cap, size_t need)
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

void epm_free(void *p)
{
    free(p);
}

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
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }

    free(ctx);
}
