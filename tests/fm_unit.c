/* WP4: the FM chip behind mdrt/fm.h: writes reach the core in order through the right ports (a
 * voice on channel 1 through ports 0 / 1 panned left, one on channel 4 through ports 2 / 3 panned
 * right, each side at its own pitch), the DAC, the register shadow, the reset. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/fm.h"
#include "mdrt/mdrt.h"

#define RATE ((double)MDRT_MASTER_CLOCK_NTSC / MDRT_FM_SAMPLE_MCLKS)
#define N 26633                                           /* half a second */

static int16_t buf[2 * (N + 64)];
static int got;

/* a register write as the Z80 makes it: address, a wait, data, the busy time */
static void reg(mdrt_fm *f, int part, uint8_t r, uint8_t v)
{
    mdrt_fm_write(f, part * 2, r);
    got += mdrt_fm_run(f, 17 * MDRT_FM_CYCLE_MCLKS, buf + 2 * got, N - got);
    mdrt_fm_write(f, part * 2 + 1, v);
    got += mdrt_fm_run(f, 32 * MDRT_FM_CYCLE_MCLKS, buf + 2 * got, N - got);
}

/* a sine voice (algorithm 7, operator 4 alone) on channel ch of part at block / fnum, panned */
static void voice(mdrt_fm *f, int part, int ch, int block, int fnum, uint8_t pan)
{
    for (int op = 0; op < 16; op += 4) {
        reg(f, part, (uint8_t)(0x30 + op + ch), 0x01);    /* DT 0, MUL 1 */
        reg(f, part, (uint8_t)(0x40 + op + ch), op == 12 ? 0x00 : 0x7F);   /* only operator 4 sounds */
        reg(f, part, (uint8_t)(0x50 + op + ch), 0x1F);    /* attack at once */
        reg(f, part, (uint8_t)(0x60 + op + ch), 0x00);
        reg(f, part, (uint8_t)(0x70 + op + ch), 0x00);
        reg(f, part, (uint8_t)(0x80 + op + ch), 0x0F);
    }
    reg(f, part, (uint8_t)(0xB0 + ch), 0x07);
    reg(f, part, (uint8_t)(0xB4 + ch), pan);
    reg(f, part, (uint8_t)(0xA4 + ch), (uint8_t)(block << 3 | fnum >> 8));
    reg(f, part, (uint8_t)(0xA0 + ch), (uint8_t)fnum);
}

/* upward zero crossings per second of one side, after the first 2000 samples */
static double frequency(int side, int n)
{
    int ups = 0, first = -1, last = -1;
    for (int i = 2001; i < n; i++)
        if (buf[2 * (i - 1) + side] < 0 && buf[2 * i + side] >= 0) { ups++; if (first < 0) first = i; last = i; }
    return ups > 1 ? (ups - 1) * RATE / (last - first) : 0;
}

/* about the mean: the YM2612's DAC adds an offset (the "ladder": its idle slots sit a step above
 * zero), which the mixer's DC blocker takes out */
static double rms(int side, int from, int n)
{
    double a = 0, m = 0;
    for (int i = from; i < from + n; i++) m += buf[2 * i + side];
    m /= n;
    for (int i = from; i < from + n; i++) a += (buf[2 * i + side] - m) * (buf[2 * i + side] - m);
    return sqrt(a / n);
}

/* the mean of the left side over n samples: the DAC's offset in silence */
static double mean(int from, int n)
{
    double m = 0;
    for (int i = from; i < from + n; i++) m += buf[2 * i];
    return m / n;
}

static void core_checks(const char *core, int model)
{
    printf("  FM core %s, %s:\n", core, model == MDRT_FM_YM3438 ? "YM3438" : "YM2612");
    mdrt_fm *f = mdrt_fm_new_core(core, model);
    CHECK(f != NULL);
    if (!f) return;
    CHECK(!strcmp(mdrt_fm_core_of(f), core));
    CHECK_EQ(mdrt_fm_model(f), model);

    /* silent after power on */
    got = mdrt_fm_run(f, 1000 * MDRT_FM_SAMPLE_MCLKS, buf, N);
    CHECK_EQ(got, 1000);
    CHECK(rms(0, 0, 1000) < 50 && rms(1, 0, 1000) < 50);

    /* 440 Hz on channel 1 (part I) left, 660 Hz on channel 4 (part II's first) right:
     * f = fnum x rate x 2^(block - 1) / 2^20 */
    got = 0;
    int fa = (int)lround(440.0 * (1 << 20) / (RATE * 8)), fb = (int)lround(660.0 * (1 << 20) / (RATE * 8));
    voice(f, 0, 0, 4, fa, 0x80);
    voice(f, 1, 0, 4, fb, 0x40);
    CHECK_EQ(mdrt_fm_reg(f, 0, 0xA0), (uint8_t)fa);
    CHECK_EQ(mdrt_fm_reg(f, 1, 0xA0), (uint8_t)fb);
    CHECK_EQ(mdrt_fm_reg(f, 1, 0xB4), 0x40);
    got = 0;
    reg(f, 0, 0x28, 0xF0);                                /* key on channel 1 */
    reg(f, 0, 0x28, 0xF4);                                /* and channel 4 */
    got += mdrt_fm_run(f, (uint32_t)(N - got) * MDRT_FM_SAMPLE_MCLKS, buf + 2 * got, N - got);
    CHECK_EQ(got, N);
    double l = frequency(0, N), r = frequency(1, N);
    double want_l = fa * RATE * 8 / (1 << 20), want_r = fb * RATE * 8 / (1 << 20);
    printf("  left %.2f Hz (expected %.2f), right %.2f Hz (expected %.2f); levels %.0f / %.0f\n",
           l, want_l, r, want_r, rms(0, 2000, N - 2000), rms(1, 2000, N - 2000));
    CHECK(fabs(l - want_l) < 0.5);
    CHECK(fabs(r - want_r) < 0.5);
    CHECK(rms(0, 2000, N - 2000) > 3000);                 /* a sine at full scale: 4608 / sqrt 2 */
    CHECK(fabs(rms(0, 2000, N - 2000) - rms(1, 2000, N - 2000)) < rms(0, 2000, N - 2000) * 0.05);

    /* the reset: silence, the shadow cleared */
    mdrt_fm_reset(f);
    got = mdrt_fm_run(f, 2000 * MDRT_FM_SAMPLE_MCLKS, buf, N);
    CHECK(rms(0, 1000, 1000) < 50 && rms(1, 1000, 1000) < 50);
    CHECK_EQ(mdrt_fm_reg(f, 0, 0xA0), 0);

    /* the DAC (channel 6, both sides): $2B bit 7 on, $2A the sample; a square of 100 samples.
     * A channel at full scale is +-256 x 3 (the chip's 9 bits, its YM2612 gain) x 6 (fm.c) */
    got = 0;
    reg(f, 0, 0x2B, 0x80);
    reg(f, 1, 0xB6, 0xC0);                                /* channel 6 to both sides */
    int k = 0;
    while (got < 4000) {
        mdrt_fm_write(f, 0, 0x2A);
        got += mdrt_fm_run(f, 17 * MDRT_FM_CYCLE_MCLKS, buf + 2 * got, N - got);
        mdrt_fm_write(f, 1, (k++ / 2) & 1 ? 0xF0 : 0x10);
        got += mdrt_fm_run(f, 50 * MDRT_FM_SAMPLE_MCLKS - 17 * MDRT_FM_CYCLE_MCLKS, buf + 2 * got, N - got);
    }
    double hi = 0, lo = 0;
    for (int i = 1000; i < 1100; i++) { if (buf[2 * i] > hi) hi = buf[2 * i]; if (buf[2 * i] < lo) lo = buf[2 * i]; }
    printf("  DAC: %.0f to %.0f\n", lo, hi);
    CHECK(hi - lo > 2 * 0.85 * 4608 * 112 / 128);         /* $F0 and $10: +-112 of 128 */
    CHECK(fabs(frequency(0, 4000) - RATE / 200) < 2);
    mdrt_fm_free(f);
}

int main(void)
{
    printf("  cores: the default %s;", mdrt_fm_core());
    int n = 0;
    for (const char *c; (c = mdrt_fm_cores(n)); n++) printf(" %s", c);
    printf("\n");
    CHECK(n >= 1);
    CHECK(mdrt_fm_new_core("none", MDRT_FM_YM2612) == NULL);
    CHECK(mdrt_fm_new_core(mdrt_fm_core(), 7) == NULL);
    for (int i = 0; i < n; i++)
        for (int model = MDRT_FM_YM2612; model <= MDRT_FM_YM3438; model++) core_checks(mdrt_fm_cores(i), model);
    /* the models: the YM2612's DAC "ladder" puts its idle output a step off zero, the YM3438's
     * does not (Nuked-OPN2's models; ymfm's) */
    for (int i = 0; i < n; i++) {
        double off[2];
        for (int model = 0; model < 2; model++) {
            mdrt_fm *f = mdrt_fm_new_core(mdrt_fm_cores(i), model);
            got = mdrt_fm_run(f, 1000 * MDRT_FM_SAMPLE_MCLKS, buf, N);
            off[model] = mean(0, got);
            mdrt_fm_free(f);
        }
        printf("  %s: the idle output's offset %.1f (YM2612), %.1f (YM3438)\n", mdrt_fm_cores(i), off[0], off[1]);
        CHECK(off[0] > 100);
        CHECK(fabs(off[1]) < 1);
    }
    return check_done("fm_unit");
}
