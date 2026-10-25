#ifndef ECO_PROFILE_INTERNAL_H
#define ECO_PROFILE_INTERNAL_H

#pragma once

#include "eco_profile.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if !defined(_WIN32)

#include <pthread.h>
#include <signal.h>

enum {
    k_max_depth = 64,
    k_initial_cap = 1024,
    k_addr_cap = 4096,
    k_pending_cap = 16384
};

typedef struct {
    uint32_t depth;
    uint64_t frames[k_max_depth];
} pending_sample;

typedef struct {
    uint64_t hash;
    uint32_t rec;
    uint32_t used;
} intern_slot;

typedef struct intern_table {
    void *items;
    intern_slot *map;
    uint32_t count;
    uint32_t cap;
    uint32_t item_size;
} intern_table;

typedef uint64_t (*intern_hash_fn)(const void *items, uint32_t i, void *ctx);
typedef int (*intern_same_fn)(const void *items, uint32_t rec, const void *ctx);

typedef struct {
    uint32_t first;
    uint32_t depth;
} stack_rec;

typedef struct {
    uint32_t site_or_kind;
    uint32_t stack;
    uint64_t count;
    uint64_t bytes;
} event_rec;

typedef struct {
    uint32_t stack;
    uint32_t thread;
    uint64_t samples;
} sample_rec;

typedef struct {
    const eco_site *site;
} site_rec;

typedef struct {
    uint32_t parent;
    uint32_t site;
    uint64_t calls;
    uint64_t ticks;
    uint64_t entered_at;
} cct_rec;

typedef struct thread_state {
    struct thread_state *next;
    _Atomic int busy;
    _Atomic int sampling;
    uint32_t thread_id;
    uint64_t dropped;
    sigset_t saved_mask;

    uint64_t *frames;
    uint32_t frame_count;
    uint32_t frame_cap;

    intern_table stacks;
    intern_table events;
    intern_table samples;
    intern_table sites;

    intern_table cct;
    uint32_t shadow[k_max_depth];
    uint32_t shadow_depth;

    // SIGPROF walks into this pre-mapped PC arena; intern at leave_hook / finish
    pending_sample *pending;
    uint32_t pending_count;
    uint32_t pending_cap;
} thread_state;

void *eco_map_bytes(size_t bytes);
void *eco_map_zero(size_t bytes);
uint64_t eco_fnv(const void *data, size_t n);

int intern_ensure(intern_table *t, uint32_t item_size, intern_hash_fn hash, void *hash_ctx);
uint32_t intern_find(
    intern_table *t,
    uint64_t hash,
    intern_same_fn same,
    const void *ctx,
    uint32_t *out_slot
);
uint32_t intern_insert(intern_table *t, uint32_t slot, uint64_t hash, const void *item);

uint32_t intern_site(thread_state *st, const eco_site *site);
uint32_t intern_stack(thread_state *st, const uint64_t *frames, uint32_t depth);
void add_event(thread_state *st, uint32_t site_or_kind, uint32_t stack, uint64_t bytes);
void add_sample(thread_state *st, uint32_t stack);
uint32_t intern_cct(thread_state *st, uint32_t parent, uint32_t site);

uint32_t capture_stack(uint64_t *frames, uint32_t cap, void *pc, void *fp);
uint32_t walk_stack(thread_state *st, void *pc, void *fp);
uint32_t walk_here(thread_state *st);
void start_sampler(void);
void stop_sampler(void);

void bump_live(int64_t delta);
int addr_put(void *block, uint32_t stack, uint32_t thread, uint64_t size);
int addr_take(void *block, uint32_t *stack, uint64_t *size);
typedef struct {
    uint32_t stack;
    uint32_t thread;
    uint64_t size;
} live_hit;

uint32_t addr_copy_live(live_hit **out);

thread_state *current_state(int from_signal);
int enter_hook(thread_state **out);
void leave_hook(thread_state *st);
void flush_pending(thread_state *st);
void wait_idle(void);

void write_u32(FILE *f, uint32_t v);
void write_u64(FILE *f, uint64_t v);
void write_file(int status);

extern _Atomic(thread_state *) g_threads;
extern _Atomic int g_started;
extern _Atomic int g_closed;
extern _Atomic uint64_t g_dropped;
extern uint32_t g_sample_hz;

#endif

#endif
