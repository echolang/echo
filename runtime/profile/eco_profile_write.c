#include "eco_profile_internal.h"

#if defined(_WIN32)

typedef int eco_profile_write_unused;

#else

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

void write_u32(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

void write_u64(FILE *f, uint64_t v)
{
    fwrite(&v, 8, 1, f);
}

static void pad4(FILE *f, uint32_t n)
{
    uint32_t r = n & 3u;
    if (r != 0) {
        uint32_t z = 0;
        fwrite(&z, 1, 4 - r, f);
    }
}

static const char *output_path(void)
{
    const char *env = getenv("ECHO_PROFILE_OUT");
    if (env != NULL && env[0] != 0) {
        return env;
    }
    return "a.ecoprof";
}

static int binary_path(char *buf, size_t cap)
{
#if defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[4096];
        if (realpath(buf, resolved) != NULL) {
            strncpy(buf, resolved, cap - 1);
            buf[cap - 1] = 0;
        }
        return 1;
    }
    return 0;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0) {
        return 0;
    }
    buf[n] = 0;
    return 1;
#endif
}

static void write_name(FILE *f, const char *s)
{
    if (s == NULL) {
        s = "";
    }
    uint32_t n = (uint32_t)strlen(s);
    write_u32(f, n);
    fwrite(s, 1, n, f);
    pad4(f, n);
}

void write_file(int status)
{
    const char *path = output_path();
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }

    uint32_t thread_count = 0;
    uint64_t dropped = atomic_load(&g_dropped);
    uint32_t flags = 0;

    for (thread_state *st = atomic_load(&g_threads); st != NULL; st = st->next) {
        thread_count += 1;
        dropped += st->dropped;
        if (st->cct.count > 1) {
            flags |= ECO_PROF_FLAG_CALLS;
        }
    }

    unsigned char header[64];
    memset(header, 0, sizeof(header));
    memcpy(header, ECO_DUMP_MAGIC, 8);
    uint32_t version = ECO_DUMP_VERSION;
    uint32_t header_bytes = ECO_DUMP_HEADER_BYTES;
    uint64_t ticks = 1000000000ull;
    uint32_t st_status = (uint32_t)status;
    uint64_t anchor = (uint64_t)(uintptr_t)&__eco_prof_anchor;
    memcpy(header + 8, &version, 4);
    memcpy(header + 12, &header_bytes, 4);
    memcpy(header + 16, &ticks, 8);
    memcpy(header + 24, &st_status, 4);
    memcpy(header + 28, &flags, 4);
    memcpy(header + 32, &anchor, 8);
    memcpy(header + 40, &g_sample_hz, 4);
    memcpy(header + 44, &thread_count, 4);
    memcpy(header + 48, &dropped, 8);
    fwrite(header, 1, 64, f);

    char binpath[4096];
    if (binary_path(binpath, sizeof(binpath))) {
        write_name(f, binpath);
    } else {
        write_name(f, "");
    }

    live_hit *live = NULL;
    uint32_t live_n = addr_copy_live(&live);
    write_u32(f, live_n);
    for (uint32_t i = 0; i < live_n; i++) {
        write_u32(f, live[i].stack);
        write_u32(f, live[i].thread);
        write_u64(f, live[i].size);
    }

    for (thread_state *st = atomic_load(&g_threads); st != NULL; st = st->next) {
        write_u32(f, st->thread_id);
        write_u32(f, st->dropped);
        write_u32(f, st->sites.count);
        write_u32(f, st->frame_count);
        write_u32(f, st->stacks.count);
        write_u32(f, st->events.count);
        write_u32(f, st->samples.count);
        write_u32(f, st->cct.count);

        const site_rec *sites = (const site_rec *)st->sites.items;
        for (uint32_t i = 0; i < st->sites.count; i++) {
            const eco_site *site = sites[i].site;
            uint32_t kind = site != NULL ? site->kind : 0;
            uint32_t pad = 0;
            uint64_t type_bytes = site != NULL ? site->type_bytes : 0;
            uint64_t fn = site != NULL ? (uint64_t)(uintptr_t)site->function : 0;
            write_u32(f, kind);
            write_u32(f, pad);
            write_u64(f, type_bytes);
            write_u64(f, fn);
            write_name(f, site != NULL ? site->type_name : "");
        }

        if (st->frame_count != 0) {
            fwrite(st->frames, 8, st->frame_count, f);
        }

        const stack_rec *stacks = (const stack_rec *)st->stacks.items;
        for (uint32_t i = 0; i < st->stacks.count; i++) {
            write_u32(f, stacks[i].first);
            write_u32(f, stacks[i].depth);
        }

        const event_rec *events = (const event_rec *)st->events.items;
        for (uint32_t i = 0; i < st->events.count; i++) {
            write_u32(f, events[i].site_or_kind);
            write_u32(f, events[i].stack);
            write_u64(f, events[i].count);
            write_u64(f, events[i].bytes);
        }

        const sample_rec *samples = (const sample_rec *)st->samples.items;
        for (uint32_t i = 0; i < st->samples.count; i++) {
            write_u32(f, samples[i].stack);
            write_u32(f, samples[i].thread);
            write_u64(f, samples[i].samples);
        }

        const cct_rec *cct = (const cct_rec *)st->cct.items;
        for (uint32_t i = 0; i < st->cct.count; i++) {
            write_u32(f, cct[i].parent);
            write_u32(f, cct[i].site);
            write_u64(f, cct[i].calls);
            write_u64(f, cct[i].ticks);
        }
    }

    fclose(f);
}

#endif
