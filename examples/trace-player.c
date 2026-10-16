/* Plays a client's recorded traces live through the runtime: a .vdptrace (the VDP port writes of
 * each frame) and a .sndtrace (the sound chips' writes at their master clocks), both from
 * ../nhl95/tools/oracle/. The pictures loop with the video trace; the sound runs to its end.
 * usage: trace-player [FILE.vdptrace] [FILE.sndtrace] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/platform_sdl2.h"

static uint8_t *load(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*len);
    if (!b || fread(b, 1, (size_t)*len, f) != (size_t)*len) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

/* ---- the video trace ("MDVT") --------------------------------------------------------------- */
enum { T_CTRL16 = 1, T_CTRL32, T_DATA16, T_DATA32, T_READ16, T_STATUS, T_HV8, T_HV16, T_DMASRC, T_FRAME, T_END };
#define STATE (24 + 0x10000 + 128 + 80)
typedef struct video { uint8_t *b; long len, start, pos; const uint8_t *dma; uint32_t left; uint32_t version; } video;

static uint8_t dma_read(void *user, uint32_t addr)
{
    (void)addr;
    video *v = user;
    if (!v->left) return 0;
    v->left--;
    return *v->dma++;
}

static void set_state(mdrt_vdp *vdp, const uint8_t *p)
{
    mdrt_vdp_state s;
    memcpy(s.reg, p, 24);
    memcpy(s.vram, p + 24, 0x10000);
    for (int i = 0; i < 64; i++) s.cram[i] = (uint16_t)(p[24 + 0x10000 + 2 * i] << 8 | p[24 + 0x10000 + 2 * i + 1]);
    for (int i = 0; i < 40; i++) s.vsram[i] = (uint16_t)(p[24 + 0x10000 + 128 + 2 * i] << 8 | p[24 + 0x10000 + 128 + 2 * i + 1]);
    mdrt_vdp_set_state(vdp, &s);
}

/* the writes of one frame, each at its time in the frame (version 2: the composition's clock,
 * so writes during the display land on their lines); at the trace's end, from its start again */
static void video_frame(video *v, mdrt *m)
{
    mdrt_vdp *vdp = mdrt_get_vdp(m);
    for (;;) {
        if (v->pos >= v->len) { v->pos = v->start; set_state(vdp, v->b + 12); }
        const uint8_t *p = v->b + v->pos;
        uint8_t t = *p++;
        if (v->version >= 2 && t >= T_CTRL16 && t <= T_HV16) {
            int32_t at = (int32_t)le32(p);
            p += 4;
            if (at > 0) mdrt_run_until(m, (uint32_t)at);
        }
        switch (t) {
        case T_CTRL16: mdrt_vdp_ctrl16(vdp, le16(p)); p += 2; break;
        case T_CTRL32: mdrt_vdp_ctrl32(vdp, le32(p)); p += 4; break;
        case T_DATA16: mdrt_vdp_data16(vdp, le16(p)); p += 2; break;
        case T_DATA32: mdrt_vdp_data32(vdp, le32(p)); p += 4; break;
        case T_READ16: (void)mdrt_vdp_read16(vdp); p += 2; break;
        case T_STATUS: (void)mdrt_vdp_status(vdp); p += 2; break;
        case T_HV8: p += 2; break;
        case T_HV16: p += 2; break;
        case T_DMASRC: v->left = le32(p); v->dma = p + 4; p += 4 + v->left; break;
        case T_FRAME: p += STATE; p += 4 + (size_t)le16(p) * le16(p + 2) * 2; v->pos = p - v->b; return;
        default: v->pos = v->len; continue;                /* T_END */
        }
        v->pos = p - v->b;
    }
}

/* ---- the sound trace ("MDST" version 3) ------------------------------------------------------ */
#define SND_HEADER (20 + 0x2000 + 12)
typedef struct sound { uint8_t *b; long len, pos; uint64_t high; uint32_t last; int in_reset; } sound;

/* the events up to the end of frame f, at their times in it; 0 at the trace's end */
static int sound_frame(sound *s, mdrt *m, uint64_t f)
{
    uint64_t from = f * mdrt_frame_mclks(m), to = from + mdrt_frame_mclks(m);
    while (s->pos < s->len) {
        const uint8_t *p = s->b + s->pos;
        uint8_t t = p[0];
        if (t == 7) { s->pos = s->len; break; }
        uint32_t t32 = le32(p + 1);
        uint64_t time = (t32 < s->last && s->last - t32 > 0x80000000u ? s->high + (1ull << 32) : s->high) | t32;
        if (time >= to) return 1;
        if (t32 < s->last && s->last - t32 > 0x80000000u) s->high += 1ull << 32;
        s->last = t32;
        uint32_t at = time > from ? (uint32_t)(time - from) : 0;
        p += 5;
        switch (t) {
        case 1: mdrt_ym_write(m, at, p[0], p[1]); p += 2; break;
        case 2: mdrt_psg_port(m, at, p[0]); p += 1; break;
        case 3: p += 3; break;
        case 4: p += 2; break;
        case 5:                                           /* the Z80's reset line resets the FM chip */
            if (!(le16(p) & 0x100)) { mdrt_ym_reset(m, at); s->in_reset = 1; }
            else if (s->in_reset) { mdrt_ym_reset(m, at); s->in_reset = 0; }
            p += 2;
            break;
        case 6: p += 4 + (size_t)le32(p) * 4; break;
        default: s->pos = s->len; return 0;
        }
        s->pos = p - s->b;
    }
    return 0;
}

static video vid;
static sound snd;

static void game(mdrt *m, void *user)
{
    (void)user;
    mdrt_vdp *vdp = mdrt_get_vdp(m);
    if (vid.b) { mdrt_vdp_set_memory(vdp, dma_read, &vid); set_state(vdp, vid.b + 12); }
    for (uint64_t f = 0;; f++) {
        if (vid.b) video_frame(&vid, m);
        int more = snd.b ? sound_frame(&snd, m, f) : 1;
        if (mdrt_wait_vblank(m) || !more) break;
        mdrt_vblank_end(m);
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        long len;
        uint8_t *b = load(argv[i], &len);
        if (!b) return 1;
        if (len > 12 && !memcmp(b, "MDVT", 4) && (le32(b + 4) == 1 || le32(b + 4) == 2)) { vid = (video){ b, len, 12 + STATE, 12 + STATE, NULL, 0, le32(b + 4) }; }
        else if (len > SND_HEADER && !memcmp(b, "MDST", 4) && le32(b + 4) == 3) { snd = (sound){ b, len, SND_HEADER, 0, 0, 1 }; }
        else { fprintf(stderr, "%s: not a version 1 or 2 .vdptrace or a version 3 .sndtrace\n", argv[i]); return 1; }
    }
    if (!vid.b && !snd.b) { fprintf(stderr, "usage: %s [FILE.vdptrace] [FILE.sndtrace]\n", argv[0]); return 2; }
    mdrt_config c = { 0 };
    mdrt_sdl_options o = { "md-runtime trace player", 3, 0, NULL };
    return mdrt_sdl_run(&c, &o, game, NULL);
}
