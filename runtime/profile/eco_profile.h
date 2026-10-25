#ifndef ECO_PROFILE_H
#define ECO_PROFILE_H

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECO_PROF_MAGIC "ECOPROF"
#define ECO_PROF_VERSION 1u
#define ECO_PROF_HEADER_BYTES 64u

#define ECO_DUMP_MAGIC "ECODUMP"
#define ECO_DUMP_VERSION 1u
#define ECO_DUMP_HEADER_BYTES 64u

#define ECO_PROF_FLAG_SYMBOLIZED 1u
#define ECO_PROF_FLAG_CALLS 2u

#define ECO_PROF_TAG(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define ECO_PROF_TAG_STRS ECO_PROF_TAG('S', 'T', 'R', 'S')
#define ECO_PROF_TAG_BINY ECO_PROF_TAG('B', 'I', 'N', 'Y')
#define ECO_PROF_TAG_SITE ECO_PROF_TAG('S', 'I', 'T', 'E')
#define ECO_PROF_TAG_FRAM ECO_PROF_TAG('F', 'R', 'A', 'M')
#define ECO_PROF_TAG_STAK ECO_PROF_TAG('S', 'T', 'A', 'K')
#define ECO_PROF_TAG_EVNT ECO_PROF_TAG('E', 'V', 'N', 'T')
#define ECO_PROF_TAG_LIVE ECO_PROF_TAG('L', 'I', 'V', 'E')
#define ECO_PROF_TAG_SMPL ECO_PROF_TAG('S', 'M', 'P', 'L')
#define ECO_PROF_TAG_CCTN ECO_PROF_TAG('C', 'C', 'T', 'N')
#define ECO_PROF_TAG_ADDR ECO_PROF_TAG('A', 'D', 'D', 'R')
#define ECO_PROF_TAG_INLN ECO_PROF_TAG('I', 'N', 'L', 'N')

enum {
    ECO_PROF_KIND_ALLOC = 1,
    ECO_PROF_KIND_REALLOC = 2,
    ECO_PROF_KIND_FREE = 3,
    ECO_PROF_KIND_COPY_BYTES = 4,
    ECO_PROF_KIND_COPY_CTOR = 5,
    ECO_PROF_KIND_CALL = 6
};

enum {
    ECO_PROF_STATUS_OK = 0,
    ECO_PROF_STATUS_EXIT = 1,
    ECO_PROF_STATUS_ABORT = 2
};

#define ECO_PROF_SITE_BIT 0x80000000u

typedef struct eco_site {
    uint32_t kind;
    uint32_t pad;
    uint64_t type_bytes;
    const char *type_name;
    const void *function;
} eco_site;

void __eco_prof_start(void);
void __eco_prof_finish(int status);
void __eco_prof_alloc(void *block, uint64_t size);
void __eco_prof_realloc(void *old_block, void *block, uint64_t size);
void __eco_prof_free(void *block);
void __eco_prof_copy(const eco_site *site);
void __eco_prof_enter(const eco_site *site);
void __eco_prof_exit(void);

extern const unsigned char __eco_prof_anchor;

#ifdef __cplusplus
}
#endif

#endif
