/* The FM cores behind src/fm/fm.c: Nuked-OPN2 (nuked.c) and ymfm (ymfm.cpp), each built in when the
 * build lists it (make FM_CORES="nuked ymfm", the default) and chosen at run time. */
#ifndef MDRT_FM_CORE_H
#define MDRT_FM_CORE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fm_core fm_core;

typedef struct fm_core_ops {
    const char *name;
    fm_core *(*create)(int model);                     /* MDRT_FM_YM2612 or MDRT_FM_YM3438 */
    void (*destroy)(fm_core *c);
    void (*reset)(fm_core *c);
    void (*write)(fm_core *c, int port, uint8_t value);
    /* one internal clock: what it adds to the sample (a sample sums 24 of them) */
    void (*clock)(fm_core *c, int32_t out[2]);
    /* the core's whole state as bytes: save returns the size (out NULL: only that); load returns
     * 0, or -1 when the bytes are not this core's (then nothing changed) */
    size_t (*save)(fm_core *c, uint8_t *out);
    int (*load)(fm_core *c, const uint8_t *in, size_t size);
} fm_core_ops;

#ifdef MDRT_HAVE_NUKED
extern const fm_core_ops fm_core_nuked;
#endif
#ifdef MDRT_HAVE_YMFM
extern const fm_core_ops fm_core_ymfm;
#endif

#ifdef __cplusplus
}
#endif
#endif
