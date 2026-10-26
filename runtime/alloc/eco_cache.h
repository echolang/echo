#ifndef ECO_CACHE_H
#define ECO_CACHE_H

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* native page heap behind `--alloc-cache`. small sizes (≤ 14336 B) are slots in
 * one 16 KiB page; larger blocks are libc malloc with a 16-byte prefix so realloc
 * can grow in place. a pointer this allocator did not issue: free is a no-op,
 * realloc returns null. MemoryCodegen is the only caller. */
void *__eco_cache_alloc(size_t n);
void *__eco_cache_realloc(void *p, size_t n);
void __eco_cache_free(void *p);

/* give every empty page back. tests reset between cases. pages with live slots
 * stay. a thread that exits leaks its pages until process exit. */
void __eco_cache_flush(void);

#ifdef __cplusplus
}
#endif

#endif
