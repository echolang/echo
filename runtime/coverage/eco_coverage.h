#ifndef ECO_COVERAGE_H
#define ECO_COVERAGE_H

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECO_COV_MAGIC "ECOCOV"
#define ECO_COV_VERSION 2u
#define ECO_COV_HEADER_BYTES 160u
#define ECO_COV_TEST_ID_BYTES 128u

typedef struct eco_cov_header {
    char magic[8];
    uint32_t version;
    uint32_t n_modules;
    uint64_t reserved;
    char test_id[ECO_COV_TEST_ID_BYTES];
    uint8_t pad[8];
} eco_cov_header;

typedef struct eco_cov_reg {
    uint32_t *counts;
    uint32_t **base;
    uint32_t n;
    uint32_t listed;
    const char *module;
    const char *id;
    struct eco_cov_reg *next;
} eco_cov_reg;

void __eco_cov_register(eco_cov_reg *reg);
void __eco_cov_start(void);

#ifdef __cplusplus
}
#endif

#endif
