/* Quiet frames (mdrt_set_quiet) and the simulation hash (mdrt_sim_hash), for rollback: a test game
 * that leans on everything a frame played again must keep (H-interrupts that change the VDP mid
 * frame, HV counter reads kept as its chance, the clock moved by charges and line waits, the pads
 * read through the I/O ports, FM, PSG and DAC sound from a sound tick) runs 100 frames three
 * ways: as it is; with handshakes 31 to 40 quiet; and with a rollback at frame 60 to the state of
 * frame 50, the ten frames played again quiet. The simulation hash is the same at every frame all
 * three ways; the platform sees no quiet handshake (no present, audio, wait or requests) and every
 * other frame's picture and sound are the same as the first run's. Then: the hash leaves out the
 * FM chip (two FM cores, the same hashes) and the picture, takes in a client's part unless it is
 * marked presentation, and mdrt_platform_input reads the platform's buttons under a pad source. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"

#define FRAMES 100

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
#define FNV0 0xCBF29CE484222325ull

/* what the platform saw, by handshake (1 to FRAMES); 0: nothing */
typedef struct seen {
    uint64_t pic[FRAMES + 2], snd[FRAMES + 2], sim[FRAMES + 2];
    int presents, audios, waits, requests;
    mdrt *m;
} seen;

static void present(void *user, const uint16_t *px, int w, int h, int pitch)
{
    seen *s = user;
    uint64_t f = FNV0;
    for (int y = 0; y < h; y++) f = fnv(f, px + y * pitch, (size_t)w * 2);
    s->pic[mdrt_frame_count(s->m) + 1] = f;               /* (the handshake ending frame n is the n + 1th) */
    s->presents++;
}
static void audio(void *user, const int16_t *x, int n)
{
    seen *s = user;
    s->snd[mdrt_frame_count(s->m) + 1] = fnv(FNV0, x, (size_t)n * 4);
    s->audios++;
}
static int wait(void *user) { ((seen *)user)->waits++; return 0; }
static unsigned requests(void *user) { ((seen *)user)->requests++; return 0; }
static void platform_input(void *user, uint16_t b[MDRT_PADS]) { (void)user; b[0] = MDRT_PAD_A; b[1] = MDRT_PAD_START; }

/* the pads by frame: a pattern that changes now and then */
static void pad_source(void *user, uint16_t b[MDRT_PADS])
{
    uint64_t f = mdrt_frame_count(user) + 1;
    b[0] = (uint16_t)((f / 7) % 3 == 0 ? MDRT_PAD_RIGHT | MDRT_PAD_B : (f / 5) % 2 ? MDRT_PAD_LEFT : 0);
    b[1] = (uint16_t)(f % 11 < 4 ? MDRT_PAD_C : 0);
    b[2] = b[3] = 0;
}

/* the client: everything it keeps from one frame to the next (a part of every state) */
typedef struct game {
    uint32_t f, chance, hints, pads, n;
    uint64_t next, base;
} game;
static game g;
static mdrt *gm;

static size_t game_save(void *user, uint8_t *out) { (void)user; if (out) memcpy(out, &g, sizeof g); return sizeof g; }
static int game_load(void *user, const uint8_t *in, size_t size) { (void)user; if (size != sizeof g) return -1; memcpy(&g, in, size); return 0; }

/* DAC samples every 1800 master clocks */
static void tick(void *user, uint32_t until)
{
    (void)user;
    while (g.next < g.base + until) {
        uint32_t at = (uint32_t)(g.next - g.base);
        mdrt_ym_write(gm, at, 0, 0x2A);
        mdrt_ym_write(gm, at + 765, 1, (g.n++ / 8) & 1 ? 0xA0 : 0x60);
        g.next += 1800;
    }
}

/* a raster effect: each H-interrupt sets the backdrop and the horizontal scroll, and reads the HV
 * counter as chance */
static void hint(void *user)
{
    (void)user;
    mdrt_vdp *v = mdrt_get_vdp(gm);
    g.hints++;
    mdrt_vdp_ctrl16(v, (uint16_t)(0x8700 | (g.hints & 15)));
    mdrt_vdp_ctrl32(v, 0x7C000002);
    mdrt_vdp_data16(v, (uint16_t)(g.hints * 3));
    g.chance = g.chance * 31 + mdrt_vdp_hv16(v);
}

/* the pads as a game reads them: TH high, then low */
static uint16_t read_pad(int port)
{
    mdrt_pads *p = mdrt_get_pads(gm);
    uint32_t data = 0xA10003 + 2 * (uint32_t)port, ctrl = 0xA10009 + 2 * (uint32_t)port;
    mdrt_pad_write(p, ctrl, 0x40);
    mdrt_pad_write(p, data, 0x40);
    uint8_t hi = mdrt_pad_read(p, data);
    mdrt_pad_write(p, data, 0x00);
    uint8_t lo = mdrt_pad_read(p, data);
    return (uint16_t)(hi << 8 | lo);
}

static mdrt *console(seen *s, const char *core)
{
    mdrt_config c = { 0 };
    c.fm_core = core;
    c.sram_size = 0x100;
    c.platform = (mdrt_platform){ .user = s, .present = present, .audio = audio, .wait = wait, .requests = requests,
                                  .input = platform_input };
    mdrt *m = mdrt_new(&c);
    s->m = m;
    gm = m;
    memset(&g, 0, sizeof g);
    mdrt_set_sound_tick(m, tick, NULL);
    mdrt_set_hint(m, hint, NULL);
    mdrt_set_pad_source(m, pad_source, m);
    CHECK_EQ(mdrt_add_state_part(m, "GAME", game_save, game_load, NULL), 0);
    return m;
}

/* the game's frame: its vertical blank work, then its logic through the display */
static void frame(void)
{
    mdrt_vdp *v = mdrt_get_vdp(gm);
    if (g.f == 0) {
        static const uint8_t regs[] = { 0x14, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
        for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
        mdrt_vdp_ctrl32(v, 0xC0000000);
        for (int c = 0; c < 16; c++) mdrt_vdp_data16(v, (uint16_t)((c & 7) << 1 | (c >> 1 & 7) << 5 | (7 - (c & 7)) << 9));
        mdrt_vdp_ctrl32(v, 0x40200000);
        for (int i = 0; i < 16; i++) mdrt_vdp_data16(v, (uint16_t)(0x1234 * (i + 1)));
        mdrt_vdp_ctrl32(v, 0x40000003);
        for (int i = 0; i < 64 * 32; i++) mdrt_vdp_data16(v, (uint16_t)(1 + (i & 1)));
        mdrt_ym_write(gm, 0, 0, 0x2B); mdrt_ym_write(gm, 700, 1, 0x80);
        static const uint8_t patch[][2] = { { 0xB1, 0x07 }, { 0xB5, 0xC0 }, { 0x31, 0x01 }, { 0x35, 0x01 }, { 0x39, 0x01 }, { 0x3D, 0x01 },
            { 0x41, 0x10 }, { 0x45, 0x10 }, { 0x49, 0x10 }, { 0x4D, 0x10 }, { 0x51, 0x1F }, { 0x55, 0x1F }, { 0x59, 0x1F }, { 0x5D, 0x1F },
            { 0x81, 0x13 }, { 0x85, 0x13 }, { 0x89, 0x13 }, { 0x8D, 0x13 }, { 0xA5, 0x22 }, { 0xA1, 0x69 } };
        uint32_t at = 2000;
        for (size_t i = 0; i < sizeof patch / sizeof *patch; i++, at += 1400) {
            mdrt_ym_write(gm, at, 0, patch[i][0]);
            mdrt_ym_write(gm, at + 600, 1, patch[i][1]);
        }
    }
    g.pads = g.pads * 7 + read_pad(0) + 3u * read_pad(1);
    mdrt_vdp_ctrl32(v, 0x5C000003);
    mdrt_vdp_data16(v, (uint16_t)(g.f * 3 + (g.pads & 7)));
    if (g.f % 20 == 0) {
        int n = 254 - (int)(g.f / 20) * 20;
        mdrt_psg_port(gm, 2000, (uint8_t)(0x80 | (n & 15)));
        mdrt_psg_port(gm, 2100, (uint8_t)(n >> 4));
        mdrt_psg_port(gm, 2200, 0x90);
    }
    if (g.f % 40 == 30 || (g.pads & 0x40)) { mdrt_ym_write(gm, 3000, 0, 0x28); mdrt_ym_write(gm, 3600, 1, 0xF1); }   /* key on */
    if (g.f % 40 == 15) { mdrt_ym_write(gm, 3000, 0, 0x28); mdrt_ym_write(gm, 3600, 1, 0x01); }                    /* key off */
    mdrt_vblank_end(gm);
    /* the logic, through the display: the clock moved, a line waited for, the HV counter read */
    for (int i = 0; i < 40 + (int)(g.pads % 13); i++) mdrt_charge(gm, 400);
    g.chance = g.chance * 31 + mdrt_vdp_hv16(v);
    mdrt_wait_line(gm, 120 + (int)(g.f % 50));
    g.chance = g.chance * 31 + mdrt_vdp_hv16(v);
    mdrt_vdp_ctrl32(v, 0x40000003);                       /* (written while the beam is in the display) */
    mdrt_vdp_data16(v, (uint16_t)(1 + (g.chance & 1)));
    mdrt_sram_write8(mdrt_get_sram(gm), 2 * (g.f & 0x7F) + 1, (uint8_t)g.chance);
}

/* one frame and its handshake; the simulation's hash after it */
static void step(seen *s)
{
    frame();
    mdrt_wait_vblank(gm);
    g.base += mdrt_frame_mclks(gm);
    g.f++;
    s->sim[mdrt_frame_count(gm)] = mdrt_sim_hash(gm);
}

static void run_plain(seen *s, const char *core)
{
    memset(s, 0, sizeof *s);
    console(s, core);
    while (mdrt_frame_count(gm) < FRAMES) step(s);
}

static int same_sim(const seen *a, const seen *b)
{
    int n = 0;
    for (int f = 1; f <= FRAMES; f++) n += a->sim[f] == b->sim[f];
    return n;
}

static void run(void)
{
    static seen a, b, c;
    run_plain(&a, NULL);
    CHECK_EQ(a.presents, FRAMES);
    CHECK(a.sim[FRAMES] != a.sim[FRAMES - 1] && a.pic[FRAMES] != a.pic[FRAMES - 1] && g.hints > 1000);
    mdrt_free(gm);

    /* handshakes 31 to 40 quiet: the frames from 30 to 39 not drawn, 40 drawn whole */
    memset(&b, 0, sizeof b);
    console(&b, NULL);
    while (mdrt_frame_count(gm) < FRAMES) {
        if (mdrt_frame_count(gm) == 30) { mdrt_set_quiet(gm, 10); CHECK_EQ(mdrt_quiet(gm), 10); }
        step(&b);
    }
    CHECK_EQ(mdrt_quiet(gm), 0);
    int shown = 0, quiet = 0;
    for (int h = 1; h <= FRAMES; h++) {
        if (h >= 31 && h <= 40) quiet += !b.pic[h] && !b.snd[h];
        else shown += b.pic[h] == a.pic[h] && b.snd[h] == a.snd[h];
    }
    printf("  quiet handshakes 31-40: the simulation the same at %d of %d frames; %d of 10 not shown, %d of %d others as before\n",
           same_sim(&a, &b), FRAMES, quiet, shown, FRAMES - 10);
    CHECK_EQ(same_sim(&a, &b), FRAMES);
    CHECK_EQ(quiet, 10);
    CHECK_EQ(shown, FRAMES - 10);
    CHECK_EQ(b.presents, FRAMES - 10); CHECK_EQ(b.audios, FRAMES - 10);
    CHECK_EQ(b.waits, FRAMES - 10); CHECK_EQ(b.requests, FRAMES - 10);

    mdrt_free(gm);

    /* a rollback: at frame 60, frame 50's state, the ten frames played again quiet */
    memset(&c, 0, sizeof c);
    console(&c, NULL);
    while (mdrt_frame_count(gm) < 50) step(&c);
    size_t n = mdrt_get_state(gm, NULL, 0);
    uint8_t *st = malloc(n);
    CHECK_EQ(mdrt_get_state(gm, st, n), n);
    while (mdrt_frame_count(gm) < 60) step(&c);
    int before = c.presents;
    memset(c.pic + 51, 0, sizeof c.pic[0] * 10);         /* (the handshakes played again must show nothing) */
    memset(c.snd + 51, 0, sizeof c.snd[0] * 10);
    memset(c.sim + 51, 0, sizeof c.sim[0] * 10);
    CHECK_EQ(mdrt_set_state(gm, st, n), 0);
    CHECK_EQ(mdrt_frame_count(gm), 50);
    mdrt_set_quiet(gm, 10);
    while (mdrt_frame_count(gm) < FRAMES) step(&c);
    CHECK_EQ(c.presents - before, FRAMES - 60);
    free(st);
    uint64_t sim2 = mdrt_sim_hash(gm);
    shown = 0;
    for (int h = 51; h <= FRAMES; h++) shown += h <= 60 ? !c.pic[h] : c.pic[h] == a.pic[h] && c.snd[h] == a.snd[h];
    printf("  a rollback from 60 to 50: the simulation the same at %d of %d frames; the frames after as before: %d of %d\n",
           same_sim(&a, &c), FRAMES, shown, FRAMES - 50);
    CHECK_EQ(same_sim(&a, &c), FRAMES);
    CHECK_EQ(shown, FRAMES - 50);

    /* a client part marked presentation is left out; the console's FM section in or out */
    CHECK_EQ(mdrt_set_state_part_kind(gm, "GAME", MDRT_PART_PRESENTATION), 0);
    CHECK(mdrt_sim_hash(gm) != sim2);
    CHECK_EQ(mdrt_set_state_part_kind(gm, "GAME", MDRT_PART_SIMULATION), 0);
    CHECK_EQ(mdrt_sim_hash(gm), sim2);
    CHECK_EQ(mdrt_set_state_part_kind(gm, "FM  ", MDRT_PART_SIMULATION), 0);
    CHECK(mdrt_sim_hash(gm) != sim2);
    CHECK_EQ(mdrt_set_state_part_kind(gm, "NONE", MDRT_PART_SIMULATION), -1);
    CHECK_EQ(mdrt_set_state_part_kind(gm, "CONS", MDRT_PART_PRESENTATION), -1);

    /* the platform's buttons under a pad source */
    uint16_t pb[MDRT_PADS];
    mdrt_platform_input(gm, pb);
    CHECK_EQ(pb[0], MDRT_PAD_A); CHECK_EQ(pb[1], MDRT_PAD_START); CHECK_EQ(pb[2], 0);
    mdrt_free(gm);

    /* another FM core: other sound, the same simulation */
    const char *other = NULL;
    for (int i = 0; mdrt_fm_cores(i); i++)
        if (strcmp(mdrt_fm_cores(i), mdrt_fm_cores(0))) other = mdrt_fm_cores(i);
    if (other) {
        static seen d;
        run_plain(&d, other);
        int differ = 0;
        for (int h = 1; h <= FRAMES; h++) differ += d.snd[h] != a.snd[h];
        printf("  %s against %s: the simulation the same at %d of %d frames (the sound differs at %d)\n", other, mdrt_fm_cores(0),
               same_sim(&a, &d), FRAMES, differ);
        CHECK_EQ(same_sim(&a, &d), FRAMES);
        CHECK(differ > 0);
        mdrt_free(gm);
    }
}

int main(void)
{
    run();
    return check_done("quiet_test");
}
