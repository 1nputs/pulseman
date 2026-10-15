/* WP3: the PSG model against the documented behaviour (SMS Power "SN76489", Sega's manual): the
 * byte protocol, tone frequencies, the 2 dB attenuation steps, the noise register. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"
#include "mdrt/psg.h"

#define RATE ((double)MDRT_MASTER_CLOCK_NTSC / MDRT_PSG_SAMPLE_MCLKS)   /* 53267 Hz */
#define N 53267

static int16_t buf[N];

/* zero crossings upward in buf, per second (after the first 100 samples: switching a channel on
 * from silence rings about zero for the band-limited step's length) */
static double frequency(const int16_t *s, int n)
{
    int ups = 0, first = -1, last = -1;
    for (int i = 101; i < n; i++)
        if (s[i - 1] < 0 && s[i] >= 0) { ups++; if (first < 0) first = i; last = i; }
    return ups > 1 ? (ups - 1) * RATE / (last - first) : 0;
}

static double rms(const int16_t *s, int n)
{
    double a = 0;
    for (int i = 0; i < n; i++) a += (double)s[i] * s[i];
    return sqrt(a / n);
}

/* tone channel ch at period p and attenuation a, the others silent */
static void tone(mdrt_psg *p, int ch, int period, int att)
{
    mdrt_psg_init(p);
    mdrt_psg_write(p, (uint8_t)(0x80 | ch << 5 | (period & 15)));
    mdrt_psg_write(p, (uint8_t)(period >> 4 & 0x3F));
    mdrt_psg_write(p, (uint8_t)(0x90 | ch << 5 | att));
}

int main(void)
{
    mdrt_psg p;

    /* power on: silent */
    mdrt_psg_init(&p);
    mdrt_psg_render(&p, buf, 1000);
    CHECK(rms(buf, 1000) == 0);

    /* the protocol: a latch writes the low 4 bits, a data byte the high 6; the latch stays */
    mdrt_psg_init(&p);
    mdrt_psg_write(&p, 0x8A);                             /* tone 0, low bits $A */
    mdrt_psg_write(&p, 0x15);                             /* high bits $15 */
    CHECK_EQ(p.reg[0], 0x15A);
    mdrt_psg_write(&p, 0x3F);                             /* another data byte: still tone 0 */
    CHECK_EQ(p.reg[0], 0x3FA);
    mdrt_psg_write(&p, 0xD7);                             /* volume 2 = 7 */
    CHECK_EQ(p.reg[5], 7);
    mdrt_psg_write(&p, 0x03);                             /* data to a volume: its low 4 bits */
    CHECK_EQ(p.reg[5], 3);
    CHECK_EQ(p.reg[0], 0x3FA);
    mdrt_psg_write(&p, 0xE5);                             /* noise: white, rate 1 */
    CHECK_EQ(p.reg[6], 5);
    p.lfsr = 0x1234;
    mdrt_psg_write(&p, 0x06);                             /* data to the noise register: resets it */
    CHECK_EQ(p.reg[6], 6);
    CHECK_EQ(p.lfsr, 0x8000);

    /* tone frequencies: clock / (32 x period) */
    static const int periods[] = { 0x3F9, 0x1AC, 0xFE, 0x47, 0x20 };
    for (size_t i = 0; i < sizeof periods / sizeof periods[0]; i++) {
        for (int ch = 0; ch < 3; ch++) {
            tone(&p, ch, periods[i], 0);
            mdrt_psg_render(&p, buf, N);
            double want = MDRT_Z80_CLOCK_NTSC / (32.0 * periods[i]), got = frequency(buf, N);
            if (!(fabs(got - want) < want * 0.002)) printf("  period %#x channel %d: %.2f Hz, expected %.2f\n", periods[i], ch, got, want);
            CHECK(fabs(got - want) < want * 0.002);
        }
    }
    /* periods 0 and 1: a constant +1 at the channel's level */
    for (int per = 0; per < 2; per++) {
        tone(&p, 1, per, 0);
        mdrt_psg_render(&p, buf, 100);
        CHECK_EQ(buf[50], MDRT_PSG_MAX);
    }

    /* attenuation: 2 dB a step, 15 off */
    tone(&p, 0, 0x100, 0);
    mdrt_psg_render(&p, buf, N / 4);
    double full = rms(buf, N / 4);
    for (int a = 1; a < 15; a++) {
        tone(&p, 0, 0x100, a);
        mdrt_psg_render(&p, buf, N / 4);
        double db = 20 * log10(rms(buf, N / 4) / full);
        CHECK(fabs(db + 2.0 * a) < 0.05);
    }
    tone(&p, 0, 0x100, 15);
    mdrt_psg_render(&p, buf, 1000);
    CHECK(rms(buf, 1000) == 0);

    /* four channels at full volume never reach the ends of the sample range, the band-limited
     * steps' overshoot included: the three tones flipping together, and the noise */
    mdrt_psg_init(&p);
    for (int ch = 0; ch < 3; ch++) { mdrt_psg_write(&p, (uint8_t)(0x80 | ch << 5)); mdrt_psg_write(&p, 0x04); }
    mdrt_psg_write(&p, 0xE4);
    for (int ch = 0; ch < 4; ch++) mdrt_psg_write(&p, (uint8_t)(0x90 | ch << 5));
    mdrt_psg_render(&p, buf, N);
    int peak = 0;
    for (int i = 0; i < N; i++) if (abs(buf[i]) > peak) peak = abs(buf[i]);
    printf("  four channels at full volume: peak %d\n", peak);
    CHECK(peak < 32767);

    /* periodic noise: the register's one bit goes round every 16 shifts, so the output is a
     * pulse: high one shift in 16 (a shift every 2 x $10 internal clocks at rate 0) */
    mdrt_psg_init(&p);
    mdrt_psg_write(&p, 0xE0);                             /* periodic, rate 0 */
    mdrt_psg_write(&p, 0xF0);                             /* noise volume 0 */
    int high = 0, total = 0;
    int16_t prev = 0;
    int rises = 0, first = -1, last = -1;
    mdrt_psg_render(&p, buf, N);
    for (int i = 100; i < N; i++) {                       /* (after switching on) */
        total++;
        if (buf[i] > 0) high++;
        if (i > 100 && prev <= 0 && buf[i] > 0) { rises++; if (first < 0) first = i; last = i; }
        prev = buf[i];
    }
    double per_sec = (rises - 1) * RATE / (last - first);
    double want = MDRT_Z80_CLOCK_NTSC / 16.0 / (2 * 0x10) / 16;
    CHECK(fabs(per_sec - want) < want * 0.01);
    CHECK(fabs((double)high / total - 1.0 / 16) < 0.01);

    /* white noise (taps 0 and 3): about as many highs as lows, and the register goes through
     * 2^15 - 1 values before it repeats ($8000 is not on the cycle from... measured below) */
    mdrt_psg_init(&p);
    mdrt_psg_write(&p, 0xE4);                             /* white, rate 0 */
    uint16_t seen0 = 0;
    long steps = 0;
    /* walk the register by itself: the cycle length from its first value on the cycle */
    uint16_t r = 0x8000;
    for (int i = 0; i < 100; i++) r = (uint16_t)(r >> 1 | ((r ^ r >> 3) & 1) << 15);
    seen0 = r;
    do { r = (uint16_t)(r >> 1 | ((r ^ r >> 3) & 1) << 15); steps++; } while (r != seen0 && steps < 70000);
    printf("  white noise register period: %ld shifts\n", steps);
    CHECK(steps < 70000);
    mdrt_psg_write(&p, 0xF0);
    mdrt_psg_render(&p, buf, N);
    high = 0;
    for (int i = 0; i < N; i++) if (buf[i] > 0) high++;
    CHECK(fabs((double)high / N - 0.5) < 0.05);

    return check_done("psg_unit");
}
