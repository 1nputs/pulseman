#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sndtrace.h"

static uint32_t u32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

#define HEADER (20 + 0x2000 + 12)

/* the time stored modulo 2^32, unwrapped (events come in order give or take a few hundred clocks) */
static uint64_t unwrap(sndtrace *t, uint32_t v)
{
    if (v < t->last && t->last - v > 0x80000000u) t->high += 1ull << 32;
    t->last = v;
    return t->high | v;
}

int sndtrace_load(sndtrace *t, const char *path)
{
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    t->data = malloc((size_t)len);
    t->len = fread(t->data, 1, (size_t)len, f);
    fclose(f);
    if (t->len < HEADER || memcmp(t->data, "MDST", 4) || u32(t->data + 4) != 3) { sndtrace_free(t); return 0; }
    t->rate = u32(t->data + 8);
    t->fm_preamp = u16(t->data + 12);
    t->psg_preamp = u16(t->data + 14);
    t->power_on = u32(t->data + 16) & 1;
    t->flags = u32(t->data + 16);
    /* the audio, gathered */
    size_t cap = 1 << 20;
    t->audio = malloc(cap * 4);
    size_t o = HEADER;
    while (o < t->len) {
        uint8_t tag = t->data[o++];
        if (tag == ST_END) break;
        uint64_t time = unwrap(t, u32(t->data + o)); o += 4;
        switch (tag) {
        case ST_YM: o += 2; break;
        case ST_PSG: o += 1; break;
        case ST_Z80RAM: o += 3; break;
        case ST_BUSREQ: case ST_Z80RESET: o += 2; break;
        case ST_AUDIO: {
            uint32_t n = u32(t->data + o); o += 4;
            while (t->frames + n > cap) t->audio = realloc(t->audio, (cap *= 2) * 4);
            memcpy(t->audio + 2 * t->frames, t->data + o, (size_t)n * 4);
            t->frames += n;
            o += (size_t)n * 4;
            t->end = time;
            break;
        }
        default: sndtrace_free(t); return 0;
        }
    }
    t->pos = HEADER;
    t->high = 0; t->last = 0;
    return 1;
}

int sndtrace_next(sndtrace *t, st_event *e)
{
    if (t->pos >= t->len) return 0;
    const uint8_t *d = t->data;
    memset(e, 0, sizeof *e);
    e->tag = d[t->pos++];
    if (e->tag == ST_END) { t->pos = t->len; return 0; }
    e->time = unwrap(t, u32(d + t->pos)); t->pos += 4;
    switch (e->tag) {
    case ST_YM: e->a = d[t->pos]; e->v = d[t->pos + 1]; t->pos += 2; break;
    case ST_PSG: e->v = d[t->pos]; t->pos += 1; break;
    case ST_Z80RAM: e->addr = u16(d + t->pos); e->v = d[t->pos + 2]; t->pos += 3; break;
    case ST_BUSREQ: case ST_Z80RESET: e->addr = u16(d + t->pos); t->pos += 2; break;
    case ST_AUDIO: t->pos += 4 + (size_t)u32(d + t->pos) * 4; break;
    default: t->pos = t->len; return 0;
    }
    return 1;
}

void sndtrace_free(sndtrace *t)
{
    free(t->data);
    free(t->audio);
    memset(t, 0, sizeof *t);
}
