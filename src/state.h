/* Writing and reading the modules' states (internal): little-endian fields one after another, the
 * same bytes on every host. A writer with no buffer only counts, which gives the size. A reader
 * that runs past the end marks itself bad and reads zeros. */
#ifndef MDRT_STATE_H
#define MDRT_STATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct st_out { uint8_t *p; size_t n; } st_out;
typedef struct st_in { const uint8_t *p; size_t n, size; int bad; } st_in;

static inline void st_put(st_out *o, const void *src, size_t k) { if (o->p) memcpy(o->p + o->n, src, k); o->n += k; }
static inline void st_u8(st_out *o, uint8_t v) { if (o->p) o->p[o->n] = v; o->n++; }
static inline void st_u16(st_out *o, uint16_t v) { st_u8(o, (uint8_t)v); st_u8(o, (uint8_t)(v >> 8)); }
static inline void st_u32(st_out *o, uint32_t v) { st_u16(o, (uint16_t)v); st_u16(o, (uint16_t)(v >> 16)); }
static inline void st_u64(st_out *o, uint64_t v) { st_u32(o, (uint32_t)v); st_u32(o, (uint32_t)(v >> 32)); }

static inline int st_have(st_in *i, size_t k)
{
    if (i->bad || i->size - i->n < k) { i->bad = 1; return 0; }
    return 1;
}
static inline void st_get(st_in *i, void *dst, size_t k)
{
    if (st_have(i, k)) { memcpy(dst, i->p + i->n, k); i->n += k; }
    else memset(dst, 0, k);
}
static inline uint8_t st_g8(st_in *i) { return st_have(i, 1) ? i->p[i->n++] : 0; }
static inline uint16_t st_g16(st_in *i) { uint16_t v = st_g8(i); return (uint16_t)(v | st_g8(i) << 8); }
static inline uint32_t st_g32(st_in *i) { uint32_t v = st_g16(i); return v | (uint32_t)st_g16(i) << 16; }
static inline uint64_t st_g64(st_in *i) { uint64_t v = st_g32(i); return v | (uint64_t)st_g32(i) << 32; }

/* arrays of 16-bit and 32-bit values */
static inline void st_u16s(st_out *o, const uint16_t *v, size_t n) { for (size_t k = 0; k < n; k++) st_u16(o, v[k]); }
static inline void st_g16s(st_in *i, uint16_t *v, size_t n) { for (size_t k = 0; k < n; k++) v[k] = st_g16(i); }
static inline void st_u32s(st_out *o, const int32_t *v, size_t n) { for (size_t k = 0; k < n; k++) st_u32(o, (uint32_t)v[k]); }
static inline void st_g32s(st_in *i, int32_t *v, size_t n) { for (size_t k = 0; k < n; k++) v[k] = (int32_t)st_g32(i); }

#endif
