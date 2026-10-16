/* The whole console's state as a value: a test game (a scrolling picture, a PSG scale, an FM voice
 * and DAC samples from a sound tick, all mid-note when the state is taken) runs 120 frames; its
 * state at frame 60, loaded again, gives the same pictures, VDP states and sound for frames 60-120
 * bit for bit. A state that is not this console's (bad magic, cut short, another part list,
 * another FM core) is refused and leaves the console as it was; a state taken at another audio
 * rate loads, with the same pictures. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"

#define FRAMES 120
#define AT 60

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/* what each frame showed and sounded like */
typedef struct seen { uint64_t pic[FRAMES + 1], snd[FRAMES + 1], vdp[FRAMES + 1]; int frame; } seen;

static void present(void *user, const uint16_t *px, int w, int h, int pitch)
{
    seen *s = user;
    uint64_t f = 0xCBF29CE484222325ull;
    for (int y = 0; y < h; y++) f = fnv(f, px + y * pitch, (size_t)w * 2);
    s->pic[s->frame] = f;
}
static void audio(void *user, const int16_t *x, int n)
{
    seen *s = user;
    s->snd[s->frame] = fnv(0xCBF29CE484222325ull, x, (size_t)n * 4);
}

/* the client: its frame count and its sound processor, a part of every state */
typedef struct game { mdrt *m; uint32_t f; uint64_t next, base; uint32_t n; } game;

static size_t game_save(void *user, uint8_t *out)
{
    game *g = user;
    if (out) { memcpy(out, &g->f, 4); memcpy(out + 4, &g->next, 8); memcpy(out + 12, &g->base, 8); memcpy(out + 20, &g->n, 4); }
    return 24;
}
static int game_load(void *user, const uint8_t *in, size_t size)
{
    game *g = user;
    if (size != 24) return -1;
    memcpy(&g->f, in, 4); memcpy(&g->next, in + 4, 8); memcpy(&g->base, in + 12, 8); memcpy(&g->n, in + 20, 4);
    return 0;
}

/* DAC samples every 1800 master clocks */
static void tick(void *user, uint32_t until)
{
    game *g = user;
    while (g->next < g->base + until) {
        uint32_t at = (uint32_t)(g->next - g->base);
        mdrt_ym_write(g->m, at, 0, 0x2A);
        mdrt_ym_write(g->m, at + 765, 1, (g->n++ / 8) & 1 ? 0xA0 : 0x60);
        g->next += 1800;
    }
}

static mdrt *console(seen *s, game *g, uint32_t rate, int extra_part)
{
    mdrt_config c = { 0 };
    c.audio_rate = rate;
    c.sram_size = 0x100;
    c.platform = (mdrt_platform){ .user = s, .present = present, .audio = audio };
    mdrt *m = mdrt_new(&c);
    memset(g, 0, sizeof *g);
    g->m = m;
    mdrt_set_sound_tick(m, tick, g);
    CHECK_EQ(mdrt_add_state_part(m, "GAME", game_save, game_load, g), 0);
    CHECK_EQ(mdrt_add_state_part(m, "GAME", game_save, game_load, g), -1);   /* (a tag once) */
    static uint8_t more[16];
    if (extra_part) CHECK_EQ(mdrt_add_state_memory(m, "MORE", more, sizeof more), 0);
    return m;
}

/* frame f's work, in the vertical blank */
static void frame(game *g)
{
    mdrt *m = g->m;
    mdrt_vdp *v = mdrt_get_vdp(m);
    uint32_t f = g->f;
    if (f == 0) {
        static const uint8_t regs[] = { 0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
        for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
        mdrt_vdp_ctrl32(v, 0xC0000000);
        for (int c = 0; c < 16; c++) mdrt_vdp_data16(v, (uint16_t)((c & 7) << 1 | (c >> 1 & 7) << 5 | (7 - (c & 7)) << 9));
        mdrt_vdp_ctrl32(v, 0x40200000);
        for (int i = 0; i < 16; i++) mdrt_vdp_data16(v, (uint16_t)(0x1234 * (i + 1)));
        mdrt_vdp_ctrl32(v, 0x40000003);
        for (int i = 0; i < 64 * 32; i++) mdrt_vdp_data16(v, (uint16_t)(1 + (i & 1)));
        mdrt_ym_write(m, 0, 0, 0x2B); mdrt_ym_write(m, 700, 1, 0x80);
        /* an FM voice on channel 2: algorithm 7, every operator loud, a slow release */
        static const uint8_t patch[][2] = { { 0xB1, 0x07 }, { 0xB5, 0xC0 }, { 0x31, 0x01 }, { 0x35, 0x01 }, { 0x39, 0x01 }, { 0x3D, 0x01 },
            { 0x41, 0x10 }, { 0x45, 0x10 }, { 0x49, 0x10 }, { 0x4D, 0x10 }, { 0x51, 0x1F }, { 0x55, 0x1F }, { 0x59, 0x1F }, { 0x5D, 0x1F },
            { 0x81, 0x13 }, { 0x85, 0x13 }, { 0x89, 0x13 }, { 0x8D, 0x13 }, { 0xA5, 0x22 }, { 0xA1, 0x69 } };
        uint32_t at = 2000;
        for (size_t i = 0; i < sizeof patch / sizeof *patch; i++, at += 1400) {
            mdrt_ym_write(m, at, 0, patch[i][0]);
            mdrt_ym_write(m, at + 600, 1, patch[i][1]);
        }
    }
    mdrt_vdp_ctrl32(v, 0x5C000003);
    mdrt_vdp_data16(v, (uint16_t)(f * 3));
    if (f % 20 == 0) {
        int n = 254 - (int)(f / 20) * 20;
        mdrt_psg_port(m, 2000, (uint8_t)(0x80 | (n & 15)));
        mdrt_psg_port(m, 2100, (uint8_t)(n >> 4));
        mdrt_psg_port(m, 2200, 0x90);
    }
    if (f % 40 == 30) { mdrt_ym_write(m, 3000, 0, 0x28); mdrt_ym_write(m, 3600, 1, 0xF1); }   /* key on */
    if (f % 40 == 15) { mdrt_ym_write(m, 3000, 0, 0x28); mdrt_ym_write(m, 3600, 1, 0x01); }   /* key off */
    mdrt_ym_write(m, mdrt_frame_mclks(m) + 500, 0, 0x2A);   /* (a write for the next frame: carried over) */
    mdrt_sram_write8(mdrt_get_sram(m), 2 * (f & 0x7F) + 1, (uint8_t)f);
}

static void run_to(seen *s, game *g, uint32_t to)
{
    while (g->f < to) {
        frame(g);
        mdrt_vblank_end(g->m);
        s->frame = (int)g->f + 1;
        mdrt_wait_vblank(g->m);
        mdrt_vdp_state vs;
        mdrt_vdp_get_state(mdrt_get_vdp(g->m), &vs);
        s->vdp[g->f + 1] = fnv(0xCBF29CE484222325ull, &vs, sizeof vs);
        g->base += mdrt_frame_mclks(g->m);
        g->f++;
    }
}

/* (memmem is not C11) */
static uint8_t *find(uint8_t *b, size_t n, const char *what)
{
    size_t k = strlen(what);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(b + i, what, k)) return b + i;
    return NULL;
}

static uint8_t *take(mdrt *m, size_t *n)
{
    *n = mdrt_get_state(m, NULL, 0);
    uint8_t *b = malloc(*n);
    CHECK_EQ(mdrt_get_state(m, b, *n), *n);
    return b;
}

int main(void)
{
    static seen a, b;
    game ga;
    mdrt *m = console(&a, &ga, 48000, 0);
    run_to(&a, &ga, AT);
    size_t n;
    uint8_t *st = take(m, &n);
    CHECK_EQ(mdrt_get_state(m, st, n - 1), n);           /* (too small: only the size) */
    run_to(&a, &ga, FRAMES);
    printf("  a state of %zu bytes at frame %d\n", n, AT);

    /* the state again, into the same console after it ran on, and into a new one */
    memcpy(&b, &a, sizeof b);
    CHECK_EQ(mdrt_set_state(m, st, n), 0);
    CHECK_EQ(ga.f, AT);
    CHECK_EQ(mdrt_frame_count(m), AT);
    run_to(&b, &ga, FRAMES);
    int same = 0;
    for (int f = AT + 1; f <= FRAMES; f++) same += a.pic[f] == b.pic[f] && a.snd[f] == b.snd[f] && a.vdp[f] == b.vdp[f];
    printf("  loaded into the same console: %d of %d frames the same\n", same, FRAMES - AT);
    CHECK_EQ(same, FRAMES - AT);
    CHECK(a.snd[FRAMES] != a.snd[FRAMES - 1] && a.pic[FRAMES] != a.pic[FRAMES - 1]);   /* (it was changing) */

    static seen c;
    game gc;
    mdrt *m2 = console(&c, &gc, 48000, 0);
    CHECK_EQ(mdrt_set_state(m2, st, n), 0);
    run_to(&c, &gc, FRAMES);
    same = 0;
    for (int f = AT + 1; f <= FRAMES; f++) same += a.pic[f] == c.pic[f] && a.snd[f] == c.snd[f] && a.vdp[f] == c.vdp[f];
    printf("  loaded into a new console: %d of %d frames the same\n", same, FRAMES - AT);
    CHECK_EQ(same, FRAMES - AT);
    size_t n2;
    uint8_t *after = take(m2, &n2);
    run_to(&c, &gc, FRAMES);                              /* (nothing more: it is there) */

    /* refused, and nothing changed */
    uint8_t *bad = malloc(n);
    memcpy(bad, st, n); bad[0] = 'X';
    CHECK_EQ(mdrt_set_state(m2, bad, n), -1);
    CHECK_EQ(mdrt_set_state(m2, st, n - 1), -1);
    CHECK_EQ(mdrt_set_state(m2, NULL, 0), -1);
    memcpy(bad, st, n);
    uint8_t *core = find(bad, n, "nuked");
    if (!core) core = find(bad, n, "ymfm");
    CHECK(core != NULL);
    if (core) core[0] ^= 0x20;                            /* another FM core's state */
    CHECK_EQ(mdrt_set_state(m2, bad, n), -1);
    size_t n3;
    uint8_t *now = take(m2, &n3);
    CHECK(n3 == n2 && !memcmp(now, after, n2));
    static seen d;
    game gd;
    mdrt *m3 = console(&d, &gd, 48000, 1);                /* another part list */
    CHECK_EQ(mdrt_set_state(m3, st, n), -1);
    CHECK_EQ(gd.f, 0);

    /* another audio rate: the mixer starts afresh; the pictures are the same */
    static seen e;
    game ge;
    mdrt *m4 = console(&e, &ge, 44100, 0);
    CHECK_EQ(mdrt_set_state(m4, st, n), 0);
    run_to(&e, &ge, FRAMES);
    same = 0;
    for (int f = AT + 1; f <= FRAMES; f++) same += a.pic[f] == e.pic[f] && a.vdp[f] == e.vdp[f];
    CHECK_EQ(same, FRAMES - AT);

    free(st); free(bad); free(after); free(now);
    mdrt_free(m); mdrt_free(m2); mdrt_free(m3); mdrt_free(m4);
    return check_done("state_test");
}
