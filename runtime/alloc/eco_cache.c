#include "eco_cache.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define ECO_TLS __declspec(thread)
#define ECO_ALWAYS_INLINE __forceinline
#else
#define ECO_TLS _Thread_local
#define ECO_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

enum {
    k_page_size = 16384,
    k_page_header = 128,
    k_large_header = 16,
    k_class_count = 35,
    k_cutoff = 14336,
    k_lookup = k_cutoff / 8,
    k_large = 0xFFFFu,
    k_central_empty_cap = 1
};

static const uint64_t k_page_magic = 0xEC0CA5E1C0FFEE01ull;
static const uint32_t k_large_magic = 0xEC0CA5E1u;

/* 16 B through one 16 KiB page. anything above is libc with a prefix, so
 * realloc can grow in place. a class that needed a second page used to copy
 * on growth anyway. */
static const uint16_t k_class_size[k_class_count] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192,
    224, 256, 320, 384, 448, 512, 640, 768, 896, 1024,
    1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096, 5120, 6144,
    7168, 8192, 10240, 12288, 14336
};

_Static_assert(k_class_size[k_class_count - 1] == k_cutoff, "cutoff is the last class");
_Static_assert(k_page_header + k_cutoff <= k_page_size, "every small slot fits in one page");

typedef struct Page {
    uint64_t magic;
    uint16_t klass;
    uint16_t _pad;
    uint32_t capacity;
    uint32_t bump;
    uint32_t slot_size;
    uint32_t used;
    uint64_t owner_id;
    struct Page *next;
    struct Page *prev;
    struct Page *live_next;
    void *free_list;
    _Atomic(void *) foreign;
} Page;

typedef struct Cache {
    uint64_t id;
    Page *hint;
    Page *current[k_class_count];
    Page *pages[k_class_count];
} Cache;

typedef struct Large {
    uint32_t magic;
    uint16_t klass;
    uint16_t _pad;
    uint64_t requested;
} Large;

_Static_assert(sizeof(Page) <= k_page_header, "page header must sit in the reserved prefix");
_Static_assert(sizeof(Large) == k_large_header, "large prefix is 16 bytes");

static uint8_t g_size_to_class[k_lookup + 1];
static uint32_t g_class_cap[k_class_count];
static atomic_uint_fast64_t g_next_id = 1;
#if defined(ECO_CACHE_NO_TLS)
static int tls_tables;
#else
static ECO_TLS int tls_tables;
#endif

#if defined(_WIN32)
typedef INIT_ONCE eco_once_t;
#define ECO_ONCE_INIT INIT_ONCE_STATIC_INIT

typedef struct {
    INIT_ONCE once;
    CRITICAL_SECTION cs;
} eco_mutex_t;

#define ECO_MUTEX_INIT { INIT_ONCE_STATIC_INIT }

static BOOL CALLBACK eco_once_thunk(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once;
    (void)context;
    ((void (*)(void))param)();
    return TRUE;
}

static void eco_call_once(eco_once_t *once, void (*fn)(void))
{
    InitOnceExecuteOnce(once, eco_once_thunk, (PVOID)fn, NULL);
}

static BOOL CALLBACK eco_mutex_init(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once;
    (void)context;
    InitializeCriticalSection((CRITICAL_SECTION *)param);
    return TRUE;
}

static void eco_mutex_lock(eco_mutex_t *m)
{
    InitOnceExecuteOnce(&m->once, eco_mutex_init, &m->cs, NULL);
    EnterCriticalSection(&m->cs);
}

static void eco_mutex_unlock(eco_mutex_t *m)
{
    LeaveCriticalSection(&m->cs);
}
#else
typedef pthread_once_t eco_once_t;
#define ECO_ONCE_INIT PTHREAD_ONCE_INIT

typedef pthread_mutex_t eco_mutex_t;
#define ECO_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER

static void eco_call_once(eco_once_t *once, void (*fn)(void))
{
    pthread_once(once, fn);
}

static void eco_mutex_lock(eco_mutex_t *m)
{
    pthread_mutex_lock(m);
}

static void eco_mutex_unlock(eco_mutex_t *m)
{
    pthread_mutex_unlock(m);
}
#endif

#if defined(ECO_CACHE_NO_TLS)
static Cache g_cache = { 1 };
static eco_mutex_t g_mu = ECO_MUTEX_INIT;
static void lock_cache(void)
{
    eco_mutex_lock(&g_mu);
}
static void unlock_cache(void)
{
    eco_mutex_unlock(&g_mu);
}
#else
static ECO_TLS Cache tls;
static void lock_cache(void) {}
static void unlock_cache(void) {}
#endif

static Page *g_central[k_class_count];
static unsigned g_central_empty[k_class_count];
static eco_mutex_t g_central_mu = ECO_MUTEX_INIT;

static void lock_central(void)
{
    eco_mutex_lock(&g_central_mu);
}

static void unlock_central(void)
{
    eco_mutex_unlock(&g_central_mu);
}

static Page *g_live;
static eco_mutex_t g_live_mu = ECO_MUTEX_INIT;

static void live_insert(Page *page)
{
    eco_mutex_lock(&g_live_mu);
    page->live_next = g_live;
    g_live = page;
    eco_mutex_unlock(&g_live_mu);
}

static void live_remove(Page *page)
{
    Page **it;

    eco_mutex_lock(&g_live_mu);
    for (it = &g_live; *it != NULL; it = &(*it)->live_next) {
        if (*it == page) {
            *it = page->live_next;
            page->live_next = NULL;
            break;
        }
    }
    eco_mutex_unlock(&g_live_mu);
}

static int live_has(Page *page)
{
    Page *it;
    int found = 0;

    eco_mutex_lock(&g_live_mu);
    for (it = g_live; it != NULL; it = it->live_next) {
        if (it == page) {
            found = 1;
            break;
        }
    }
    eco_mutex_unlock(&g_live_mu);
    return found;
}

static uint64_t take_id(void)
{
    return (uint64_t)atomic_fetch_add(&g_next_id, 1);
}

static void fill_tables(void)
{
    unsigned c;
    unsigned i;

    for (c = 0; c < k_class_count; c++) {
        g_class_cap[c] = (uint32_t)((k_page_size - k_page_header) / k_class_size[c]);
    }

    c = 0;
    g_size_to_class[0] = 0;
    for (i = 1; i <= k_lookup; i++) {
        size_t n = (size_t)i * 8;
        while (c + 1 < k_class_count && k_class_size[c] < n) {
            c++;
        }
        if (k_class_size[c] < n) {
            c = k_class_count - 1;
        }
        g_size_to_class[i] = (uint8_t)c;
    }
}

static eco_once_t g_tables_once = ECO_ONCE_INIT;

static void ensure_tables(void)
{
    eco_call_once(&g_tables_once, fill_tables);
}

static int class_of(size_t n)
{
    size_t index;

    if (!tls_tables) {
        ensure_tables();
        tls_tables = 1;
    }
    if (n == 0) {
        n = 1;
    }
    if (n > k_cutoff) {
        return -1;
    }
    index = (n + 7) / 8;
    return (int)g_size_to_class[index];
}

static void *map_pages(void)
{
    void *p;

#if defined(_WIN32)
    p = _aligned_malloc(k_page_size, k_page_size);
    return p;
#else
    p = NULL;
    if (posix_memalign(&p, k_page_size, k_page_size) != 0) {
        return NULL;
    }
    return p;
#endif
}

static void unmap_pages(void *p)
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    free(p);
#endif
}

static Cache *cache(void)
{
#if defined(ECO_CACHE_NO_TLS)
    return &g_cache;
#else
    return &tls;
#endif
}

static void *slot_at(Page *page, uint32_t index)
{
    return (char *)page + k_page_header + (size_t)index * page->slot_size;
}

static void *slot_next(void *slot)
{
    void *next;
    memcpy(&next, slot, sizeof(next));
    return next;
}

static void set_slot_next(void *slot, void *next)
{
    memcpy(slot, &next, sizeof(next));
}

static void *fast_next(void *slot)
{
    return *(void **)slot;
}

static int slot_index_of(const Page *page, const void *p)
{
    ptrdiff_t off;
    uint32_t index;

    off = (const char *)p - ((const char *)page + k_page_header);
    if (off < 0) {
        return -1;
    }
    if (page->slot_size == 0 || (size_t)off % page->slot_size != 0) {
        return -1;
    }
    index = (uint32_t)((size_t)off / page->slot_size);
    if (index >= page->capacity) {
        return -1;
    }
    return (int)index;
}

/* every small page is one 16 KiB mapping. the slot pointer sits in that page
 * because header + capacity * slot_size <= 16 KiB. */
static Page *page_of(void *p)
{
    return (Page *)((uintptr_t)p & ~((uintptr_t)k_page_size - 1));
}

static int is_small_slot(void *p, Page **out)
{
    Page *page;

    if (p == NULL) {
        return 0;
    }
    page = page_of(p);
    // page_of of a non-slot is often an unmapped 16 KiB boundary. live
    // pages are the ones we mapped, so this load is of memory we allocated
    if (!live_has(page)) {
        return 0;
    }
    if (page->magic != k_page_magic) {
        return 0;
    }
    if (page->klass >= k_class_count) {
        return 0;
    }
    if (slot_index_of(page, p) < 0) {
        return 0;
    }
    *out = page;
    return 1;
}

static void destroy_page(Page *page)
{
    live_remove(page);
    page->magic = 0;
    unmap_pages(page);
}

static Large *large_of(void *p)
{
    return (Large *)((char *)p - k_large_header);
}

static void *large_payload(Large *h)
{
    return (char *)h + k_large_header;
}

/* the 16-byte prefix sits at p-16, inside the malloc block. a small slot's
 * p-16 is still in the same 16 KiB page (first slot is at +128). ask this
 * before page_of, so a large malloc never loads an unmapped 16 KiB boundary */
static int is_large_block(void *p, Large **out)
{
    Large *h;

    if (p == NULL) {
        return 0;
    }
    h = large_of(p);
    if (h->magic != k_large_magic || h->klass != k_large) {
        return 0;
    }
    *out = h;
    return 1;
}

static void unlink_page(Cache *t, Page *page)
{
    unsigned klass = page->klass;

    if (page->prev != NULL) {
        page->prev->next = page->next;
    } else if (t->pages[klass] == page) {
        t->pages[klass] = page->next;
    }
    if (page->next != NULL) {
        page->next->prev = page->prev;
    }
    page->next = NULL;
    page->prev = NULL;
    if (t->current[klass] == page) {
        t->current[klass] = NULL;
    }
}

static void link_page(Cache *t, Page *page)
{
    unsigned klass = page->klass;

    page->prev = NULL;
    page->next = t->pages[klass];
    if (page->next != NULL) {
        page->next->prev = page;
    }
    t->pages[klass] = page;
}

static void drain_foreign(Page *page)
{
    void *chain;

    if (atomic_load_explicit(&page->foreign, memory_order_relaxed) == NULL) {
        return;
    }

    chain = atomic_exchange_explicit(&page->foreign, NULL, memory_order_acquire);
    while (chain != NULL) {
        void *next = slot_next(chain);
        set_slot_next(chain, page->free_list);
        page->free_list = chain;
        if (page->used > 0) {
            page->used--;
        }
        chain = next;
    }
}

static void reset_empty(Page *page)
{
    page->bump = 0;
    page->free_list = NULL;
    atomic_store_explicit(&page->foreign, NULL, memory_order_relaxed);
    page->used = 0;
}

static Page *take_central(unsigned klass)
{
    Page *page;

    lock_central();
    page = g_central[klass];
    if (page != NULL) {
        g_central[klass] = page->next;
        if (page->used == 0 && g_central_empty[klass] > 0) {
            g_central_empty[klass]--;
        }
        page->next = NULL;
        page->prev = NULL;
    }
    unlock_central();
    return page;
}

static void give_central_or_unmap(Page *page)
{
    unsigned klass = page->klass;

    page->owner_id = 0;
    reset_empty(page);

    lock_central();
    if (g_central_empty[klass] >= k_central_empty_cap) {
        unlock_central();
        destroy_page(page);
        return;
    }
    page->next = g_central[klass];
    page->prev = NULL;
    g_central[klass] = page;
    g_central_empty[klass]++;
    unlock_central();
}

static Page *new_page(Cache *t, unsigned klass)
{
    Page *page = map_pages();

    if (page == NULL) {
        return NULL;
    }

    memset(page, 0, k_page_header);
    page->magic = k_page_magic;
    page->klass = (uint16_t)klass;
    page->capacity = g_class_cap[klass];
    page->bump = 0;
    page->slot_size = k_class_size[klass];
    page->used = 0;
    page->owner_id = t->id;
    atomic_init(&page->foreign, NULL);
    live_insert(page);
    link_page(t, page);
    return page;
}

static void *pop_slot(Page *page)
{
    void *slot;

    drain_foreign(page);
    slot = page->free_list;
    if (slot != NULL) {
        page->free_list = slot_next(slot);
        page->used++;
        return slot;
    }
    if (page->bump < page->capacity) {
        slot = slot_at(page, page->bump);
        page->bump++;
        page->used++;
        return slot;
    }
    return NULL;
}

static int page_has_room(Page *page)
{
    drain_foreign(page);
    return page->free_list != NULL || page->bump < page->capacity;
}

static Page *find_page(Cache *t, unsigned klass)
{
    Page *page;

    page = t->current[klass];
    if (page != NULL && page_has_room(page)) {
        return page;
    }

    for (page = t->pages[klass]; page != NULL; page = page->next) {
        if (page_has_room(page)) {
            t->current[klass] = page;
            return page;
        }
        if (page->used == 0) {
            t->current[klass] = page;
            reset_empty(page);
            return page;
        }
    }

    page = take_central(klass);
    if (page != NULL) {
        page->owner_id = t->id;
        reset_empty(page);
        link_page(t, page);
        t->current[klass] = page;
        return page;
    }

    page = new_page(t, klass);
    t->current[klass] = page;
    return page;
}

static void *small_alloc(Cache *t, unsigned klass)
{
    Page *page;
    void *slot;

    page = t->current[klass];
    if (page != NULL) {
        slot = page->free_list;
        if (slot != NULL) {
            page->free_list = fast_next(slot);
            page->used++;
            t->hint = page;
            return slot;
        }
        if (page->bump < page->capacity) {
            slot = slot_at(page, page->bump);
            page->bump++;
            page->used++;
            t->hint = page;
            return slot;
        }
    }

    page = find_page(t, klass);
    if (page == NULL) {
        return NULL;
    }
    slot = pop_slot(page);
    if (slot != NULL) {
        t->hint = page;
    }
    return slot;
}

/* pointer identity against this thread's pages. does not load the 16 KiB
 * aligned address, so a foreign or libc pointer cannot SEGV here. */
static ECO_ALWAYS_INLINE Page *tls_page_of(Cache *t, void *p)
{
    Page *page = page_of(p);
    unsigned k;
    Page *it;

    if (t->hint == page) {
        return page;
    }
    for (k = 0; k < k_class_count; k++) {
        if (t->current[k] == page) {
            t->hint = page;
            return page;
        }
    }
    for (k = 0; k < k_class_count; k++) {
        for (it = t->pages[k]; it != NULL; it = it->next) {
            if (it == page) {
                t->hint = page;
                return page;
            }
        }
    }
    return NULL;
}

static void reclaim_if_empty(Cache *t, Page *page)
{
    if (page->used != 0) {
        return;
    }
    // current page: the slot we just freed is on free_list. wiping it
    // forced the next alloc to bump from 0 again, which is the
    // alloc/free/alloc loop in result_owning
    if (t != NULL && t->current[page->klass] == page) {
        return;
    }
    if (t != NULL) {
        unlink_page(t, page);
    }
    give_central_or_unmap(page);
}

static ECO_ALWAYS_INLINE void owner_push(Page *page, void *p)
{
    set_slot_next(p, page->free_list);
    page->free_list = p;
    if (page->used > 0) {
        page->used--;
    }
}

static ECO_ALWAYS_INLINE void small_free(Cache *t, Page *page, void *p)
{
    if ((char *)p < (char *)page + k_page_header) {
        return;
    }

    // current page, this thread: the alloc/free/alloc loop. skip drain
    // (foreign waits for pop_slot) and skip reclaim (the slot stays on
    // free_list). result_owning is this path
    if (page->owner_id == t->id && t->current[page->klass] == page) {
        owner_push(page, p);
        return;
    }

    if (page->owner_id == t->id || page->owner_id == 0) {
        if (page->owner_id == 0) {
            lock_central();
        }
        drain_foreign(page);
        set_slot_next(p, page->free_list);
        page->free_list = p;
        if (page->used > 0) {
            page->used--;
        }
        if (page->used == 0) {
            if (page->owner_id == 0) {
                unlock_central();
                destroy_page(page);
                return;
            }
            reclaim_if_empty(t, page);
        } else if (page->owner_id == 0) {
            unlock_central();
        }
        return;
    }

    {
        void *head = atomic_load_explicit(&page->foreign, memory_order_relaxed);
        do {
            set_slot_next(p, head);
        } while (!atomic_compare_exchange_weak_explicit(
            &page->foreign,
            &head,
            p,
            memory_order_release,
            memory_order_relaxed));
    }
}

static void *fill_large(size_t n)
{
    Large *h;

    if (n > SIZE_MAX - k_large_header) {
        return NULL;
    }
    h = (Large *)malloc(k_large_header + n);
    if (h == NULL) {
        return NULL;
    }
    h->magic = k_large_magic;
    h->klass = k_large;
    h->_pad = 0;
    h->requested = (uint64_t)n;
    return large_payload(h);
}

static void flush_central(void)
{
    unsigned klass;

    lock_central();
    for (klass = 0; klass < k_class_count; klass++) {
        Page *page = g_central[klass];
        g_central[klass] = NULL;
        g_central_empty[klass] = 0;
        while (page != NULL) {
            Page *next = page->next;
            destroy_page(page);
            page = next;
        }
    }
    unlock_central();
}

static void *move_to_fresh(void *p, size_t old_size, size_t n)
{
    void *fresh;
    size_t copy;

    fresh = __eco_cache_alloc(n);
    if (fresh == NULL) {
        return NULL;
    }
    copy = old_size < n ? old_size : n;
    memcpy(fresh, p, copy);
    __eco_cache_free(p);
    return fresh;
}

static void *realloc_small(Page *page, void *p, size_t n, int new_klass)
{
    if (new_klass == (int)page->klass) {
        return p;
    }
    return move_to_fresh(p, page->slot_size, n);
}

static ECO_ALWAYS_INLINE void free_small_locked(Cache *t, Page *page, void *p)
{
    lock_cache();
    small_free(t, page, p);
    unlock_cache();
}

void *__eco_cache_alloc(size_t n)
{
    int klass;
    Cache *t;
    void *p;

    klass = class_of(n);
    if (klass < 0) {
        return fill_large(n == 0 ? 1 : n);
    }

    t = cache();
    if (t->id == 0) {
        t->id = take_id();
    }
    lock_cache();
    p = small_alloc(t, (unsigned)klass);
    unlock_cache();
    return p;
}

void __eco_cache_free(void *p)
{
    Page *page;
    Large *h;
    Cache *t;

    if (p == NULL) {
        return;
    }

    t = cache();
    page = tls_page_of(t, p);
    if (page != NULL) {
        free_small_locked(t, page, p);
        return;
    }

    if (is_large_block(p, &h)) {
        h->magic = 0;
        free(h);
        return;
    }

    if (is_small_slot(p, &page)) {
        free_small_locked(t, page, p);
    }
}

void *__eco_cache_realloc(void *p, size_t n)
{
    Page *page;
    Large *h;
    Cache *t;
    int new_klass;

    if (p == NULL) {
        return __eco_cache_alloc(n);
    }

    if (n == 0) {
        __eco_cache_free(p);
        return NULL;
    }

    new_klass = class_of(n);

    t = cache();
    page = tls_page_of(t, p);
    if (page != NULL && slot_index_of(page, p) >= 0) {
        return realloc_small(page, p, n, new_klass);
    }

    if (is_large_block(p, &h)) {
        if (new_klass >= 0) {
            return move_to_fresh(p, (size_t)h->requested, n);
        }

        if (n > SIZE_MAX - k_large_header) {
            return NULL;
        }

        {
            Large *grown = (Large *)realloc(h, k_large_header + n);
            if (grown == NULL) {
                return NULL;
            }
            grown->klass = k_large;
            grown->requested = (uint64_t)n;
            return large_payload(grown);
        }
    }

    if (is_small_slot(p, &page)) {
        return realloc_small(page, p, n, new_klass);
    }

    return NULL;
}

void __eco_cache_flush(void)
{
    Cache *t = cache();
    unsigned klass;

    lock_cache();
    t->hint = NULL;
    for (klass = 0; klass < k_class_count; klass++) {
        Page *page = t->pages[klass];
        t->pages[klass] = NULL;
        t->current[klass] = NULL;
        while (page != NULL) {
            Page *next = page->next;
            page->next = NULL;
            page->prev = NULL;
            drain_foreign(page);
            if (page->used == 0) {
                page->owner_id = 0;
                destroy_page(page);
            } else {
                page->owner_id = t->id;
                link_page(t, page);
            }
            page = next;
        }
    }
    unlock_cache();
    flush_central();
}
