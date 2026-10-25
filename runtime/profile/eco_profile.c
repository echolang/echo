#include "eco_profile.h"

#if defined(_WIN32)

void __eco_prof_start(void) {}
void __eco_prof_finish(int status) { (void)status; }
void __eco_prof_alloc(void *block, uint64_t size) { (void)block; (void)size; }
void __eco_prof_realloc(void *old_block, void *block, uint64_t size)
{
    (void)old_block;
    (void)block;
    (void)size;
}
void __eco_prof_free(void *block) { (void)block; }
void __eco_prof_copy(const eco_site *site) { (void)site; }
void __eco_prof_enter(const eco_site *site) { (void)site; }
void __eco_prof_exit(void) {}

const unsigned char __eco_prof_anchor = 1;

#else

#include "eco_profile_internal.h"

#include <string.h>
#include <time.h>

const unsigned char __eco_prof_anchor = 1;

static _Thread_local thread_state *t_state;
static _Thread_local uint32_t t_cct_skip;
_Atomic(thread_state *) g_threads;
static _Atomic uint32_t g_thread_ids;
_Atomic int g_started;
_Atomic int g_closed;
_Atomic uint64_t g_dropped;
uint32_t g_sample_hz = 1000;

static uint64_t now_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void block_sigprof(thread_state *st)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &set, &st->saved_mask);
}

static void unblock_sigprof(thread_state *st)
{
    pthread_sigmask(SIG_SETMASK, &st->saved_mask, NULL);
}

static thread_state *make_state(void)
{
    thread_state *st = (thread_state *)eco_map_bytes(sizeof(thread_state));
    if (st == NULL) {
        return NULL;
    }
    memset(st, 0, sizeof(*st));
    st->thread_id = atomic_fetch_add(&g_thread_ids, 1) + 1;
    st->pending_cap = k_pending_cap;
    st->pending = (pending_sample *)eco_map_bytes((size_t)k_pending_cap * sizeof(pending_sample));
    thread_state *head = atomic_load(&g_threads);
    do {
        st->next = head;
    } while (!atomic_compare_exchange_weak(&g_threads, &head, st));
    return st;
}

thread_state *current_state(int from_signal)
{
    if (atomic_load(&g_closed)) {
        return NULL;
    }
    if (t_state != NULL) {
        return t_state;
    }
    if (from_signal) {
        return NULL;
    }
    t_state = make_state();
    return t_state;
}

void flush_pending(thread_state *st)
{
    if (st->pending == NULL) {
        st->pending_count = 0;
        return;
    }

    const uint32_t n = st->pending_count;
    st->pending_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        add_sample(st, intern_stack(st, st->pending[i].frames, st->pending[i].depth));
    }
}

void wait_idle(void)
{
    for (;;) {
        int any = 0;
        for (thread_state *st = atomic_load(&g_threads); st != NULL; st = st->next) {
            if (atomic_load(&st->busy) || atomic_load(&st->sampling)) {
                any = 1;
                break;
            }
        }
        if (!any) {
            return;
        }
    }
}

int enter_hook(thread_state **out)
{
    if (atomic_load(&g_closed) || !atomic_load(&g_started)) {
        return 0;
    }
    thread_state *st = current_state(0);
    if (st == NULL) {
        atomic_fetch_add(&g_dropped, 1);
        return 0;
    }
    block_sigprof(st);
    if (atomic_load(&st->busy)) {
        unblock_sigprof(st);
        st->dropped += 1;
        return 0;
    }
    atomic_store(&st->busy, 1);
    if (atomic_load(&g_closed)) {
        atomic_store(&st->busy, 0);
        unblock_sigprof(st);
        return 0;
    }
    *out = st;
    return 1;
}

void leave_hook(thread_state *st)
{
    flush_pending(st);
    atomic_store(&st->busy, 0);
    unblock_sigprof(st);
}

void __eco_prof_start(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_started, &expected, 1)) {
        return;
    }
    (void)current_state(0);
    start_sampler();
}

void __eco_prof_finish(int status)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_closed, &expected, 1)) {
        return;
    }
    stop_sampler();
    wait_idle();
    for (thread_state *st = atomic_load(&g_threads); st != NULL; st = st->next) {
        flush_pending(st);
    }
    write_file(status);
}

void __eco_prof_alloc(void *block, uint64_t size)
{
    thread_state *st = NULL;
    if (!enter_hook(&st)) {
        return;
    }
    uint32_t stack = walk_here(st);
    add_event(st, ECO_PROF_KIND_ALLOC, stack, size);
    if (!addr_put(block, stack, st->thread_id, size)) {
        st->dropped += 1;
    }
    leave_hook(st);
}

void __eco_prof_realloc(void *old_block, void *block, uint64_t size)
{
    thread_state *st = NULL;
    if (!enter_hook(&st)) {
        return;
    }
    uint32_t stack = walk_here(st);
    if (old_block == NULL && block != NULL) {
        add_event(st, ECO_PROF_KIND_ALLOC, stack, size);
        if (!addr_put(block, stack, st->thread_id, size)) {
            st->dropped += 1;
        }
    } else if (old_block != NULL && block == NULL && size == 0) {
        uint64_t old_size = 0;
        uint32_t old_stack = 0;
        if (addr_take(old_block, &old_stack, &old_size)) {
            bump_live(-(int64_t)old_size);
            add_event(st, ECO_PROF_KIND_FREE, old_stack == 0 ? stack : old_stack, old_size);
        }
    } else if (old_block != NULL && block == NULL) {
        // failed realloc: leave the old record
    } else if (old_block != NULL && block != NULL) {
        add_event(st, ECO_PROF_KIND_REALLOC, stack, size);
        if (!addr_put(block, stack, st->thread_id, size)) {
            st->dropped += 1;
        } else if (old_block != block) {
            uint64_t old_size = 0;
            if (addr_take(old_block, NULL, &old_size)) {
                bump_live(-(int64_t)old_size);
            }
        }
    }
    leave_hook(st);
}

void __eco_prof_free(void *block)
{
    thread_state *st = NULL;
    if (!enter_hook(&st)) {
        return;
    }
    uint64_t size = 0;
    uint32_t stack = 0;
    if (addr_take(block, &stack, &size)) {
        bump_live(-(int64_t)size);
        if (stack == 0) {
            stack = walk_here(st);
        }
        add_event(st, ECO_PROF_KIND_FREE, stack, size);
    }
    leave_hook(st);
}

void __eco_prof_copy(const eco_site *site)
{
    thread_state *st = NULL;
    if (!enter_hook(&st)) {
        return;
    }
    uint32_t id = intern_site(st, site);
    uint32_t stack = walk_here(st);
    uint32_t kind = site != NULL ? site->kind : ECO_PROF_KIND_COPY_BYTES;
    uint32_t site_or_kind = id == 0 ? kind : (ECO_PROF_SITE_BIT | (id - 1));
    uint64_t bytes = site != NULL ? site->type_bytes : 0;
    add_event(st, site_or_kind, stack, bytes);
    leave_hook(st);
}

void __eco_prof_enter(const eco_site *site)
{
    thread_state *st = NULL;
    if (!enter_hook(&st)) {
        t_cct_skip += 1;
        return;
    }
    if (st->shadow_depth >= k_max_depth) {
        t_cct_skip += 1;
        leave_hook(st);
        return;
    }
    uint32_t id = intern_site(st, site);
    uint32_t parent = st->shadow_depth == 0 ? 0 : st->shadow[st->shadow_depth - 1];
    uint32_t node = intern_cct(st, parent, id);
    if (node == 0) {
        t_cct_skip += 1;
        leave_hook(st);
        return;
    }
    cct_rec *cct = (cct_rec *)st->cct.items;
    cct[node].calls += 1;
    cct[node].entered_at = now_ticks();
    st->shadow[st->shadow_depth] = node;
    st->shadow_depth += 1;
    leave_hook(st);
}

void __eco_prof_exit(void)
{
    if (t_cct_skip != 0) {
        t_cct_skip -= 1;
        return;
    }
    thread_state *st = NULL;
    if (!enter_hook(&st)) {
        return;
    }
    if (st->shadow_depth != 0) {
        st->shadow_depth -= 1;
        uint32_t node = st->shadow[st->shadow_depth];
        cct_rec *cct = (cct_rec *)st->cct.items;
        uint64_t entered = cct[node].entered_at;
        if (entered != 0) {
            cct[node].ticks += now_ticks() - entered;
            cct[node].entered_at = 0;
        }
    }
    leave_hook(st);
}

#endif
