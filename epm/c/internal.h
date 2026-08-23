/*
 * Shared by the POSIX and Windows process runners. Not part of the Echo FFI.
 */

#ifndef EPM_SHIM_INTERNAL_H
#define EPM_SHIM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char **v;
    int32_t n;
    int32_t cap;
} epm_argv;

int epm_grow(char **buf, size_t *cap, size_t need);

#endif
