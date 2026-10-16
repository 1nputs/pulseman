/* The runtime as a recorder: a test game (an H-interrupt handler, a write after mdrt_wait_line,
 * DMA from its RAM every frame, the pads read every frame, a PSG scale, an FM voice, DAC samples
 * from a sound tick) records its .vdptrace, .padtrace and .sndtrace into build/record/. `make check`
 * then replays them with vdp_trace, pad_trace and mix_trace, as it replays the oracle's: the
 * pictures, VDP states and pad reads must come out the same, the sound sample for sample. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "check.h"
#include "mdrt/mdrt.h"

#define FRAMES 120

static uint8_t ram[0x10000];
static uint8_t read68k(void *user, uint32_t a) { (void)user; return a >= 0xFF0000 ? ram[a & 0xFFFF] : (uint8_t)(a * 7); }

static void sink(void *user, const void *b, size_t n) { if (fwrite(b, 1, n, user) != n) check_failures++; }

typedef struct game { mdrt *m; uint64_t next, base; uint32_t n; int band; } game;

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

static void hint(void *user)
{
    game *g = user;
    mdrt_charge(g->m, 60);                                /* (the handler's way to its write: past the blank) */
    mdrt_vdp_ctrl16(mdrt_get_vdp(g->m), (uint16_t)(0x8700 | (++g->band * 9 & 0x3F)));
}

static uint16_t buttons;
static void pad_source(void *user, uint16_t b[2]) { (void)user; b[0] = buttons; b[1] = (uint16_t)(buttons >> 4); }

int main(void)
{
    mkdir("build", 0777);
    mkdir("build/record", 0777);
    FILE *fv = fopen("build/record/record.vdptrace", "wb"), *fp = fopen("build/record/record.padtrace", "wb"),
         *fs = fopen("build/record/record.sndtrace", "wb");
    CHECK(fv && fp && fs);
    if (!fv || !fp || !fs) return check_done("record_test");
    mdrt_config c = { 0 };
    c.read68k = read68k;
    c.audio_rate = 44100;
    mdrt *m = mdrt_new(&c);
    game g = { m, 0, 0, 0, 0 };
    mdrt_set_sound_tick(m, tick, &g);
    mdrt_set_hint(m, hint, &g);
    mdrt_set_pad_source(m, pad_source, NULL);
    mdrt_record(m, MDRT_TRACE_VDP, sink, fv);
    mdrt_record(m, MDRT_TRACE_PADS, sink, fp);
    mdrt_record(m, MDRT_TRACE_SOUND, sink, fs);

    mdrt_vdp *v = mdrt_get_vdp(m);
    static const uint8_t regs[] = { 0x14, 0x74, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
    for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
    mdrt_vdp_ctrl32(v, 0xC0000000);
    for (int i = 0; i < 64; i++) mdrt_vdp_data16(v, (uint16_t)((i & 7) << 1 | (i >> 3 & 7) << 5 | (i * 3 & 7) << 9));
    mdrt_vdp_ctrl32(v, 0x40000003);                       /* plane A: tiles 1 and 2 */
    for (int i = 0; i < 64 * 32; i++) mdrt_vdp_data16(v, (uint16_t)(1 + (i % 3 == 0) + ((i / 64) % 4 << 13)));
    mdrt_ym_write(m, 0, 0, 0x2B); mdrt_ym_write(m, 700, 1, 0x80);
    static const uint8_t patch[][2] = { { 0xB0, 0x07 }, { 0xB4, 0xC0 }, { 0x30, 0x01 }, { 0x34, 0x02 }, { 0x38, 0x01 }, { 0x3C, 0x03 },
        { 0x40, 0x14 }, { 0x44, 0x14 }, { 0x48, 0x14 }, { 0x4C, 0x14 }, { 0x50, 0x1F }, { 0x54, 0x1F }, { 0x58, 0x1F }, { 0x5C, 0x1F },
        { 0x80, 0x24 }, { 0x84, 0x24 }, { 0x88, 0x24 }, { 0x8C, 0x24 }, { 0xA4, 0x23 }, { 0xA0, 0x40 } };
    uint32_t at = 2000;
    for (size_t i = 0; i < sizeof patch / sizeof *patch; i++, at += 1400) { mdrt_ym_write(m, at, 0, patch[i][0]); mdrt_ym_write(m, at + 600, 1, patch[i][1]); }
    mdrt_pad_write(mdrt_get_pads(m), 0xA10009, 0x40);     /* TH an output */
    for (int f = 0; f < FRAMES; f++) {
        /* the vertical blank: tile 1 animated in RAM and sent by DMA, the scroll, the notes */
        for (int i = 0; i < 32; i++) ram[0x100 + i] = (uint8_t)((i * 17 + f * 5) ^ (i >> 2));
        mdrt_vdp_ctrl32(v, 0x93109400);                   /* 16 words */
        mdrt_vdp_ctrl32(v, 0x95809680);                   /* from $FF0100: >> 1 = $7F8080 */
        mdrt_vdp_ctrl16(v, 0x977F);
        mdrt_vdp_ctrl32(v, 0x40200080);                   /* VRAM $0020 (tile 1), DMA */
        mdrt_vdp_ctrl32(v, 0x7C000003);                   /* the scroll table, $FC00 */
        mdrt_vdp_data16(v, (uint16_t)(f * 2));
        mdrt_vdp_ctrl16(v, 0x8700);
        g.band = 0;
        if (f % 15 == 0) {
            int n = 300 - f;
            mdrt_psg_port(m, mdrt_now(m) + 2000, (uint8_t)(0x80 | (n & 15)));
            mdrt_psg_port(m, mdrt_now(m) + 2100, (uint8_t)(n >> 4));
            mdrt_psg_port(m, mdrt_now(m) + 2200, 0x92);
        }
        if (f % 30 == 10) { mdrt_ym_write(m, 3000, 0, 0x28); mdrt_ym_write(m, 3600, 1, 0xF0); }
        if (f % 30 == 25) { mdrt_ym_write(m, 3000, 0, 0x28); mdrt_ym_write(m, 3600, 1, 0x00); }
        /* the pads, both TH levels */
        buttons = (uint16_t)(f * 37 & 0xFF);
        mdrt_pad_write(mdrt_get_pads(m), 0xA10003, 0x40);
        uint8_t b1 = mdrt_pad_read(mdrt_get_pads(m), 0xA10003);
        mdrt_pad_write(mdrt_get_pads(m), 0xA10003, 0x00);
        uint8_t b2 = mdrt_pad_read(mdrt_get_pads(m), 0xA10003);
        ram[0x200 + f] = b1 ^ b2;
        mdrt_vblank_end(m);
        /* in the display: plane A moved down from line 150, after a read of the HV counter */
        mdrt_wait_line(m, 150);
        (void)mdrt_vdp_hv16(v);
        mdrt_charge(m, 40);
        mdrt_vdp_ctrl32(v, 0x40000010);
        mdrt_vdp_data16(v, (uint16_t)(f + 20));
        mdrt_vdp_ctrl32(v, 0x40000010);                   /* (and back for the next frame, from line 200) */
        mdrt_wait_line(m, 200);
        mdrt_charge(m, 200);
        mdrt_vdp_data16(v, 0);
        mdrt_wait_vblank(m);
        g.base += mdrt_frame_mclks(m);
    }
    mdrt_record(m, MDRT_TRACE_VDP, NULL, NULL);
    mdrt_record(m, MDRT_TRACE_PADS, NULL, NULL);
    mdrt_record(m, MDRT_TRACE_SOUND, NULL, NULL);
    long sizes[3] = { ftell(fv), ftell(fp), ftell(fs) };
    fclose(fv); fclose(fp); fclose(fs);
    printf("  build/record/: record.vdptrace %ld bytes, record.padtrace %ld, record.sndtrace %ld (%d frames; make check replays them)\n",
           sizes[0], sizes[1], sizes[2], FRAMES);
    CHECK(sizes[0] > FRAMES * (320 * 224 * 2) && sizes[1] > FRAMES * 5 && sizes[2] > FRAMES * 735 * 4);
    mdrt_free(m);
    return check_done("record_test");
}
