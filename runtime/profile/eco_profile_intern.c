#include "eco_profile_internal.h"

#if defined(_WIN32)

typedef int eco_profile_intern_unused;

#else

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void *eco_map_bytes(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    return p;
}

void *eco_map_zero(size_t bytes)
{
    return eco_map_bytes(bytes);
}

uint64_t eco_fnv(const void *data, size_t n)
{
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

int intern_ensure(intern_table *t, uint32_t item_size, intern_hash_fn hash, void *hash_ctx)
{
    if (t->item_size == 0) {
        t->item_size = item_size;
    }

    if (t->cap != 0 && t->count * 2 < t->cap) {
        return 1;
    }

    uint32_t old_count = t->count;
    void *old_items = t->items;
    intern_slot *old_map = t->map;
    uint32_t old_cap = t->cap;
    uint32_t next_cap = t->cap == 0 ? k_initial_cap : t->cap * 2;
    const size_t items_bytes = (size_t)next_cap * t->item_size;
    const size_t map_bytes = (size_t)(next_cap * 2) * sizeof(intern_slot);
    void *next_items = eco_map_bytes(items_bytes);
    intern_slot *next_map = (intern_slot *)eco_map_zero(map_bytes);
    if (next_items == NULL || next_map == NULL) {
        if (next_items != NULL) {
            munmap(next_items, items_bytes);
        }
        if (next_map != NULL) {
            munmap(next_map, map_bytes);
        }
        return 0;
    }

    if (old_items != NULL && old_count != 0) {
        memcpy(next_items, old_items, (size_t)old_count * t->item_size);
    }

    t->items = next_items;
    t->map = next_map;
    t->cap = next_cap;

    if (old_items != NULL && old_cap != 0) {
        munmap(old_items, (size_t)old_cap * t->item_size);
    }
    if (old_map != NULL && old_cap != 0) {
        munmap(old_map, (size_t)(old_cap * 2) * sizeof(intern_slot));
    }

    uint32_t map_cap = next_cap * 2;
    for (uint32_t i = 0; i < old_count; i++) {
        uint64_t ih = hash(next_items, i, hash_ctx);
        uint32_t slot = (uint32_t)(ih & (map_cap - 1));
        while (t->map[slot].used) {
            slot = (slot + 1) & (map_cap - 1);
        }
        t->map[slot].hash = ih;
        t->map[slot].rec = i;
        t->map[slot].used = 1;
    }

    return 1;
}

uint32_t intern_find(
    intern_table *t,
    uint64_t hash,
    intern_same_fn same,
    const void *ctx,
    uint32_t *out_slot
)
{
    uint32_t map_cap = t->cap * 2;
    uint32_t slot = (uint32_t)(hash & (map_cap - 1));
    for (;;) {
        intern_slot *s = &t->map[slot];
        if (!s->used) {
            *out_slot = slot;
            return UINT32_MAX;
        }
        if (s->hash == hash && same(t->items, s->rec, ctx)) {
            return s->rec;
        }
        slot = (slot + 1) & (map_cap - 1);
    }
}

uint32_t intern_insert(intern_table *t, uint32_t slot, uint64_t hash, const void *item)
{
    uint32_t rec = t->count;
    memcpy((unsigned char *)t->items + (size_t)rec * t->item_size, item, t->item_size);
    t->count += 1;
    t->map[slot].hash = hash;
    t->map[slot].rec = rec;
    t->map[slot].used = 1;
    return rec;
}

static int grow_frames(thread_state *st, uint32_t extra)
{
    uint32_t need = st->frame_count + extra;
    if (need <= st->frame_cap) {
        return 1;
    }
    uint32_t cap = st->frame_cap == 0 ? 4096 : st->frame_cap;
    while (cap < need) {
        cap *= 2;
    }
    uint64_t *old = st->frames;
    uint32_t old_cap = st->frame_cap;
    uint64_t *next = (uint64_t *)eco_map_bytes((size_t)cap * sizeof(uint64_t));
    if (next == NULL) {
        return 0;
    }
    if (old != NULL && st->frame_count != 0) {
        memcpy(next, old, (size_t)st->frame_count * sizeof(uint64_t));
    }
    st->frames = next;
    st->frame_cap = cap;
    if (old != NULL && old_cap != 0) {
        munmap(old, (size_t)old_cap * sizeof(uint64_t));
    }
    return 1;
}

static uint64_t hash_site_item(const void *items, uint32_t i, void *ctx)
{
    (void)ctx;
    const site_rec *r = (const site_rec *)items + i;
    return eco_fnv(&r->site, sizeof(r->site));
}

static int same_site(const void *items, uint32_t rec, const void *ctx)
{
    const site_rec *r = (const site_rec *)items + rec;
    return r->site == (const eco_site *)ctx;
}

uint32_t intern_site(thread_state *st, const eco_site *site)
{
    if (site == NULL) {
        return 0;
    }
    if (!intern_ensure(&st->sites, sizeof(site_rec), hash_site_item, NULL)) {
        return 0;
    }

    uint64_t h = eco_fnv(&site, sizeof(site));
    uint32_t slot = 0;
    uint32_t found = intern_find(&st->sites, h, same_site, site, &slot);
    if (found != UINT32_MAX) {
        return found + 1;
    }

    site_rec rec;
    rec.site = site;
    return intern_insert(&st->sites, slot, h, &rec) + 1;
}

typedef struct {
    const uint64_t *query;
    uint32_t depth;
    const uint64_t *stored;
} stack_key;

static uint64_t hash_stack_item(const void *items, uint32_t i, void *ctx)
{
    const stack_rec *r = (const stack_rec *)items + i;
    const uint64_t *frames = (const uint64_t *)ctx;
    return eco_fnv(frames + r->first, (size_t)r->depth * sizeof(uint64_t));
}

static int same_stack(const void *items, uint32_t rec, const void *ctx)
{
    const stack_rec *r = (const stack_rec *)items + rec;
    const stack_key *k = (const stack_key *)ctx;
    return r->depth == k->depth
        && memcmp(k->stored + r->first, k->query, (size_t)k->depth * sizeof(uint64_t)) == 0;
}

uint32_t intern_stack(thread_state *st, const uint64_t *frames, uint32_t depth)
{
    if (depth == 0) {
        return 0;
    }

    if (!intern_ensure(&st->stacks, sizeof(stack_rec), hash_stack_item, st->frames)) {
        return 0;
    }

    uint64_t h = eco_fnv(frames, (size_t)depth * sizeof(uint64_t));
    stack_key key = { frames, depth, st->frames };
    uint32_t slot = 0;
    uint32_t found = intern_find(&st->stacks, h, same_stack, &key, &slot);
    if (found != UINT32_MAX) {
        return found + 1;
    }

    if (!grow_frames(st, depth)) {
        return 0;
    }

    stack_rec rec;
    rec.first = st->frame_count;
    rec.depth = depth;
    memcpy(st->frames + rec.first, frames, (size_t)depth * sizeof(uint64_t));
    st->frame_count += depth;
    return intern_insert(&st->stacks, slot, h, &rec) + 1;
}

typedef struct {
    uint32_t site_or_kind;
    uint32_t stack;
} event_key;

static uint64_t hash_event_item(const void *items, uint32_t i, void *ctx)
{
    (void)ctx;
    const event_rec *r = (const event_rec *)items + i;
    uint64_t key[2] = { r->site_or_kind, r->stack };
    return eco_fnv(key, sizeof(key));
}

static int same_event(const void *items, uint32_t rec, const void *ctx)
{
    const event_rec *r = (const event_rec *)items + rec;
    const event_key *k = (const event_key *)ctx;
    return r->site_or_kind == k->site_or_kind && r->stack == k->stack;
}

void add_event(thread_state *st, uint32_t site_or_kind, uint32_t stack, uint64_t bytes)
{
    if (stack == 0) {
        st->dropped += 1;
        return;
    }
    if (!intern_ensure(&st->events, sizeof(event_rec), hash_event_item, NULL)) {
        st->dropped += 1;
        return;
    }

    event_key key = { site_or_kind, stack };
    uint64_t pair[2] = { site_or_kind, stack };
    uint64_t h = eco_fnv(pair, sizeof(pair));
    uint32_t slot = 0;
    uint32_t found = intern_find(&st->events, h, same_event, &key, &slot);
    if (found != UINT32_MAX) {
        event_rec *r = (event_rec *)st->events.items + found;
        r->count += 1;
        r->bytes += bytes;
        return;
    }

    event_rec rec;
    rec.site_or_kind = site_or_kind;
    rec.stack = stack;
    rec.count = 1;
    rec.bytes = bytes;
    intern_insert(&st->events, slot, h, &rec);
}

static uint64_t hash_sample_item(const void *items, uint32_t i, void *ctx)
{
    (void)ctx;
    const sample_rec *r = (const sample_rec *)items + i;
    return eco_fnv(&r->stack, sizeof(r->stack));
}

static int same_sample(const void *items, uint32_t rec, const void *ctx)
{
    const sample_rec *r = (const sample_rec *)items + rec;
    const uint32_t *stack = (const uint32_t *)ctx;
    return r->stack == *stack;
}

void add_sample(thread_state *st, uint32_t stack)
{
    if (stack == 0) {
        st->dropped += 1;
        return;
    }
    if (!intern_ensure(&st->samples, sizeof(sample_rec), hash_sample_item, NULL)) {
        st->dropped += 1;
        return;
    }

    uint64_t h = eco_fnv(&stack, sizeof(stack));
    uint32_t slot = 0;
    uint32_t found = intern_find(&st->samples, h, same_sample, &stack, &slot);
    if (found != UINT32_MAX) {
        ((sample_rec *)st->samples.items)[found].samples += 1;
        return;
    }

    sample_rec rec;
    rec.stack = stack;
    rec.thread = st->thread_id;
    rec.samples = 1;
    intern_insert(&st->samples, slot, h, &rec);
}

static uint64_t hash_cct_item(const void *items, uint32_t i, void *ctx)
{
    (void)ctx;
    const cct_rec *r = (const cct_rec *)items + i;
    uint64_t key[2] = { r->parent, r->site };
    return eco_fnv(key, sizeof(key));
}

static int same_cct(const void *items, uint32_t rec, const void *ctx)
{
    const cct_rec *r = (const cct_rec *)items + rec;
    const uint64_t *key = (const uint64_t *)ctx;
    return r->parent == (uint32_t)key[0] && r->site == (uint32_t)key[1];
}

uint32_t intern_cct(thread_state *st, uint32_t parent, uint32_t site)
{
    intern_table *t = &st->cct;
    if (!intern_ensure(t, sizeof(cct_rec), hash_cct_item, NULL)) {
        return 0;
    }

    if (t->count == 0) {
        cct_rec dummy;
        memset(&dummy, 0, sizeof(dummy));
        uint64_t zero[2] = { 0, 0 };
        uint64_t h = eco_fnv(zero, sizeof(zero));
        uint32_t slot = 0;
        intern_find(t, h, same_cct, zero, &slot);
        intern_insert(t, slot, h, &dummy);
    }

    uint64_t key[2] = { parent, site };
    uint64_t h = eco_fnv(key, sizeof(key));
    uint32_t slot = 0;
    uint32_t found = intern_find(t, h, same_cct, key, &slot);
    if (found != UINT32_MAX) {
        return found;
    }

    cct_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.parent = parent;
    rec.site = site;
    return intern_insert(t, slot, h, &rec);
}

#endif
