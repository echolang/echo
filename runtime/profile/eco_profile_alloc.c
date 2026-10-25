#include "eco_profile_internal.h"

#if defined(_WIN32)

typedef int eco_profile_alloc_unused;

#else

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

enum {
    k_addr_empty = 0,
    k_addr_tomb = 1
};

typedef struct {
    void *block;
    uint32_t stack;
    uint32_t thread;
    uint64_t size;
} addr_slot;

static pthread_mutex_t g_addr_mu = PTHREAD_MUTEX_INITIALIZER;
static addr_slot *g_addrs;
static uint32_t g_addr_cap;
static uint32_t g_addr_count;
static uint32_t g_addr_used;
static _Atomic uint64_t g_live_bytes;

void bump_live(int64_t delta)
{
    if (delta == 0) {
        return;
    }
    if (delta > 0) {
        atomic_fetch_add(&g_live_bytes, (uint64_t)delta);
    } else {
        atomic_fetch_sub(&g_live_bytes, (uint64_t)(-delta));
    }
}

static int addr_is_empty(void *block)
{
    return block == NULL;
}

static int addr_is_tomb(void *block)
{
    return block == (void *)(uintptr_t)k_addr_tomb;
}

static int grow_addrs(void)
{
    uint32_t next_cap = g_addr_cap == 0 ? k_addr_cap : g_addr_cap * 2;
    addr_slot *next = (addr_slot *)eco_map_zero((size_t)next_cap * sizeof(addr_slot));
    if (next == NULL) {
        return 0;
    }

    if (g_addrs != NULL && g_addr_cap != 0) {
        for (uint32_t i = 0; i < g_addr_cap; i++) {
            void *block = g_addrs[i].block;
            if (addr_is_empty(block) || addr_is_tomb(block)) {
                continue;
            }
            uint64_t h = eco_fnv(&block, sizeof(block));
            uint32_t slot = (uint32_t)(h & (next_cap - 1));
            while (!addr_is_empty(next[slot].block)) {
                slot = (slot + 1) & (next_cap - 1);
            }
            next[slot] = g_addrs[i];
        }
    }

    addr_slot *old = g_addrs;
    uint32_t old_cap = g_addr_cap;
    g_addrs = next;
    g_addr_cap = next_cap;
    g_addr_used = g_addr_count;
    if (old != NULL && old_cap != 0) {
        munmap(old, (size_t)old_cap * sizeof(addr_slot));
    }
    return 1;
}

int addr_put(void *block, uint32_t stack, uint32_t thread, uint64_t size)
{
    if (block == NULL) {
        return 0;
    }
    pthread_mutex_lock(&g_addr_mu);
    if (g_addr_cap == 0 || g_addr_used * 2 >= g_addr_cap) {
        if (!grow_addrs()) {
            pthread_mutex_unlock(&g_addr_mu);
            return 0;
        }
    }

    uint64_t h = eco_fnv(&block, sizeof(block));
    uint32_t slot = (uint32_t)(h & (g_addr_cap - 1));
    uint32_t tomb = UINT32_MAX;
    for (;;) {
        void *here = g_addrs[slot].block;
        if (addr_is_empty(here)) {
            if (tomb != UINT32_MAX) {
                slot = tomb;
            }
            break;
        }
        if (addr_is_tomb(here)) {
            if (tomb == UINT32_MAX) {
                tomb = slot;
            }
        } else if (here == block) {
            g_addrs[slot].stack = stack;
            g_addrs[slot].thread = thread;
            g_addrs[slot].size = size;
            pthread_mutex_unlock(&g_addr_mu);
            return 1;
        }
        slot = (slot + 1) & (g_addr_cap - 1);
    }

    g_addrs[slot].block = block;
    g_addrs[slot].stack = stack;
    g_addrs[slot].thread = thread;
    g_addrs[slot].size = size;
    g_addr_count += 1;
    if (tomb == UINT32_MAX) {
        g_addr_used += 1;
    }
    pthread_mutex_unlock(&g_addr_mu);
    bump_live((int64_t)size);
    return 1;
}

int addr_take(void *block, uint32_t *stack, uint64_t *size)
{
    if (block == NULL) {
        return 0;
    }
    pthread_mutex_lock(&g_addr_mu);
    if (g_addr_cap == 0) {
        pthread_mutex_unlock(&g_addr_mu);
        return 0;
    }

    uint64_t h = eco_fnv(&block, sizeof(block));
    uint32_t slot = (uint32_t)(h & (g_addr_cap - 1));
    for (uint32_t n = 0; n < g_addr_cap; n++) {
        void *here = g_addrs[slot].block;
        if (addr_is_empty(here)) {
            pthread_mutex_unlock(&g_addr_mu);
            return 0;
        }
        if (here == block) {
            if (stack != NULL) {
                *stack = g_addrs[slot].stack;
            }
            if (size != NULL) {
                *size = g_addrs[slot].size;
            }
            g_addrs[slot].block = (void *)(uintptr_t)k_addr_tomb;
            g_addr_count -= 1;
            pthread_mutex_unlock(&g_addr_mu);
            return 1;
        }
        slot = (slot + 1) & (g_addr_cap - 1);
    }
    pthread_mutex_unlock(&g_addr_mu);
    return 0;
}

uint32_t addr_copy_live(live_hit **out)
{
    *out = NULL;
    pthread_mutex_lock(&g_addr_mu);
    uint32_t n = g_addr_count;
    if (n == 0) {
        pthread_mutex_unlock(&g_addr_mu);
        return 0;
    }

    live_hit *copy = (live_hit *)eco_map_bytes((size_t)n * sizeof(live_hit));
    if (copy == NULL) {
        pthread_mutex_unlock(&g_addr_mu);
        return 0;
    }

    uint32_t w = 0;
    for (uint32_t i = 0; i < g_addr_cap && w < n; i++) {
        void *block = g_addrs[i].block;
        if (addr_is_empty(block) || addr_is_tomb(block)) {
            continue;
        }
        copy[w].stack = g_addrs[i].stack;
        copy[w].thread = g_addrs[i].thread;
        copy[w].size = g_addrs[i].size;
        w += 1;
    }
    pthread_mutex_unlock(&g_addr_mu);
    *out = copy;
    return w;
}

#endif
