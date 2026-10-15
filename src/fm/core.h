/* The FM core behind src/fm/fm.c: Nuked-OPN2 (nuked.c) or ymfm (ymfm.cpp), chosen at build time. */
#ifndef MDRT_FM_CORE_H
#define MDRT_FM_CORE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fm_core fm_core;

fm_core *fm_core_new(void);
void fm_core_free(fm_core *c);
void fm_core_reset(fm_core *c);
void fm_core_write(fm_core *c, int port, uint8_t value);
/* one internal clock: what it adds to the sample (a sample sums 24 of them) */
void fm_core_clock(fm_core *c, int32_t out[2]);
const char *fm_core_name(void);

#ifdef __cplusplus
}
#endif
#endif
