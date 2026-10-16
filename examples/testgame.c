/* The WP6 test game in a window: a moving pattern and a C major scale on the PSG, with DAC
 * samples streamed from the sound tick. Runs until the window closes (or for N frames: testgame N).
 * It checks the platform: 59.92 frames a second paced by the sound, clean sound, the keys (the
 * held buttons show as a coloured bar). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "mdrt/platform_sdl2.h"

typedef struct dac { mdrt *m; uint64_t next, base; int n; } dac;
static void tick(void *user, uint32_t until)
{
    dac *d = user;
    while (d->next < d->base + until) {
        uint32_t at = (uint32_t)(d->next - d->base);
        mdrt_ym_write(d->m, at, 0, 0x2A);
        mdrt_ym_write(d->m, at + 765, 1, (uint8_t)(0x80 + 24 * sin(d->n++ * 0.05)));   /* a quiet 300 Hz sine */
        d->next += 1800;
    }
}

static long frames = -1;

static void game(mdrt *m, void *user)
{
    (void)user;
    static const int notes[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
    mdrt_vdp *v = mdrt_get_vdp(m);
    static const uint8_t regs[] = { 0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
    for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
    mdrt_vdp_ctrl32(v, 0xC0000000);
    for (int c = 0; c < 16; c++) mdrt_vdp_data16(v, (uint16_t)((c & 7) << 1 | (c >> 1 & 7) << 5 | (7 - (c & 7)) << 9));
    mdrt_vdp_ctrl32(v, 0x40200000);
    for (int i = 0; i < 16; i++) mdrt_vdp_data16(v, (uint16_t)(0x1234 * (i + 1)));
    mdrt_vdp_ctrl32(v, 0x40000003);
    for (int i = 0; i < 64 * 32; i++) mdrt_vdp_data16(v, 1);
    dac d = { m, 0, 0, 0 };
    mdrt_set_sound_tick(m, tick, &d);
    mdrt_ym_write(m, 0, 0, 0x2B); mdrt_ym_write(m, 700, 1, 0x80);
    mdrt_ym_write(m, 1400, 2, 0xB6); mdrt_ym_write(m, 2100, 3, 0xC0);   /* channel 6 to both sides */
    for (long f = 0; frames < 0 || f < frames; f++) {
        mdrt_vdp_ctrl32(v, 0x5C000003);
        mdrt_vdp_data16(v, (uint16_t)(f * 3));
        uint16_t held = (uint16_t)(mdrt_pad_read(mdrt_get_pads(m), 0xA10003) ^ 0x7F);   /* (TH high: C B R L D U) */
        mdrt_vdp_ctrl32(v, 0xC0000000);                   /* the backdrop shows the buttons */
        mdrt_vdp_data16(v, (uint16_t)(held & 0x3F) << 3);
        if (f % 30 == 0) {
            int hz = notes[(f / 30) % 8], n = (int)lround(MDRT_Z80_CLOCK_NTSC / (32.0 * hz));
            mdrt_psg_port(m, 2000, (uint8_t)(0x80 | (n & 15)));
            mdrt_psg_port(m, 2100, (uint8_t)(n >> 4));
            mdrt_psg_port(m, 2200, 0x92);
        }
        mdrt_vblank_end(m);
        if (mdrt_wait_vblank(m)) break;
        d.base += mdrt_frame_mclks(m);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) frames = atol(argv[1]);
    mdrt_config c = { 0 };
    mdrt_sdl_options o = { "pulseman test game", 3, 0, NULL };
    return mdrt_sdl_run(&c, &o, game, NULL);
}
