#include "eco_coverage.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

_Static_assert(sizeof(eco_cov_header) == ECO_COV_HEADER_BYTES, "coverage header size");

static eco_cov_reg *g_regs;

static uint32_t padded_name(uint32_t name_len)
{
    return (name_len + 3u) & ~3u;
}

static void *map_counts(size_t bytes, const char *path)
{
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    if (path != NULL && path[0] != 0) {
        file = CreateFileA(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (file == INVALID_HANDLE_VALUE) {
            return NULL;
        }

        LARGE_INTEGER size;
        size.QuadPart = (LONGLONG)bytes;
        if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
            CloseHandle(file);
            return NULL;
        }
    }

    HANDLE mapping = CreateFileMappingA(
        file == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE : file,
        NULL,
        PAGE_READWRITE,
        (DWORD)((bytes >> 32) & 0xffffffffu),
        (DWORD)(bytes & 0xffffffffu),
        NULL);

    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }

    if (mapping == NULL) {
        return NULL;
    }

    void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    CloseHandle(mapping);
    return view;
#else
    int fd = -1;
    if (path != NULL && path[0] != 0) {
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return NULL;
        }

        if (ftruncate(fd, (off_t)bytes) != 0) {
            close(fd);
            return NULL;
        }
    }

    int flags = MAP_SHARED;
    if (fd < 0) {
        flags |= MAP_ANON;
    }

    void *view = mmap(NULL, bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (fd >= 0) {
        close(fd);
    }

    if (view == MAP_FAILED) {
        return NULL;
    }

    return view;
#endif
}

void __eco_cov_register(eco_cov_reg *reg)
{
    if (reg == NULL || reg->listed != 0) {
        return;
    }

    reg->listed = 1;
    reg->next = g_regs;
    g_regs = reg;
}

static const char *reg_module(const eco_cov_reg *reg)
{
    return reg != NULL && reg->module != NULL ? reg->module : "";
}

static const char *reg_id(const eco_cov_reg *reg)
{
    return reg != NULL && reg->id != NULL ? reg->id : "";
}

static int cmp_reg(const void *a, const void *b)
{
    const eco_cov_reg *ra = *(const eco_cov_reg *const *)a;
    const eco_cov_reg *rb = *(const eco_cov_reg *const *)b;
    const int by_module = strcmp(reg_module(ra), reg_module(rb));
    if (by_module != 0) {
        return by_module;
    }

    return strcmp(reg_id(ra), reg_id(rb));
}

void __eco_cov_start(void)
{
    uint32_t n_regs = 0;
    for (eco_cov_reg *reg = g_regs; reg != NULL; reg = reg->next) {
        n_regs += 1;
    }

    eco_cov_reg **order = NULL;
    if (n_regs > 0) {
        order = (eco_cov_reg **)malloc((size_t)n_regs * sizeof(*order));
    }

    if (order == NULL) {
        for (eco_cov_reg *reg = g_regs; reg != NULL; reg = reg->next) {
            if (reg->base != NULL) {
                *reg->base = reg->counts;
            }
        }
        return;
    }

    uint32_t i = 0;
    for (eco_cov_reg *reg = g_regs; reg != NULL; reg = reg->next) {
        order[i] = reg;
        i += 1;
    }
    qsort(order, (size_t)n_regs, sizeof(*order), cmp_reg);

    uint32_t n_modules = 0;
    size_t payload = 0;
    for (uint32_t r = 0; r < n_regs; ) {
        const char *name = reg_module(order[r]);
        uint32_t n = 0;
        uint32_t run = r;
        while (run < n_regs && strcmp(reg_module(order[run]), name) == 0) {
            n += order[run]->n;
            run += 1;
        }

        const uint32_t name_len = (uint32_t)strlen(name);
        payload += sizeof(uint32_t) * 2u + padded_name(name_len)
            + (size_t)n * sizeof(uint32_t);
        n_modules += 1;
        r = run;
    }

    const size_t bytes = (size_t)ECO_COV_HEADER_BYTES + payload;
    const char *path = getenv("ECHO_COVERAGE_OUT");
    uint8_t *mapped = (uint8_t *)map_counts(bytes, path);

    if (mapped == NULL) {
        for (uint32_t r = 0; r < n_regs; r++) {
            if (order[r]->base != NULL) {
                *order[r]->base = order[r]->counts;
            }
        }
        free(order);
        return;
    }

    eco_cov_header header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, ECO_COV_MAGIC, 6);
    header.version = ECO_COV_VERSION;
    header.n_modules = n_modules;
    header.reserved = 0;

    const char *test_id = getenv("ECHO_TEST_ID");
    if (test_id != NULL) {
        size_t n = strlen(test_id);
        if (n >= ECO_COV_TEST_ID_BYTES) {
            n = ECO_COV_TEST_ID_BYTES - 1;
        }
        memcpy(header.test_id, test_id, n);
    }

    memcpy(mapped, &header, sizeof(header));

    uint8_t *cursor = mapped + ECO_COV_HEADER_BYTES;
    for (uint32_t r = 0; r < n_regs; ) {
        const char *name = reg_module(order[r]);
        const uint32_t name_len = (uint32_t)strlen(name);
        const uint32_t name_bytes = padded_name(name_len);

        uint32_t n = 0;
        uint32_t run = r;
        while (run < n_regs && strcmp(reg_module(order[run]), name) == 0) {
            n += order[run]->n;
            run += 1;
        }

        memcpy(cursor, &name_len, sizeof(name_len));
        cursor += sizeof(uint32_t);
        memcpy(cursor, &n, sizeof(n));
        cursor += sizeof(uint32_t);
        memset(cursor, 0, name_bytes);
        if (name_len > 0) {
            memcpy(cursor, name, name_len);
        }
        cursor += name_bytes;

        uint32_t *counts = (uint32_t *)cursor;
        memset(counts, 0, (size_t)n * sizeof(uint32_t));
        uint32_t *slice = counts;
        for (uint32_t at = r; at < run; at++) {
            if (order[at]->base != NULL) {
                *order[at]->base = slice;
            }
            slice += order[at]->n;
        }
        cursor += (size_t)n * sizeof(uint32_t);
        r = run;
    }

    free(order);
}
