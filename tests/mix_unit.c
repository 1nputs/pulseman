/* The mixer: the PSG at its gain against the FM, the DC blocker, the resampler (a tone keeps
 * its pitch and level, one above the output's Nyquist frequency is gone), the output's length. */
#include <math.h>
#include <stdlib.h>
#include "check.h"
#include "mdrt/mdrt.h"
#include "mdrt/mix.h"

#define IN_RATE ((double)MDRT_MASTER_CLOCK_NTSC / 1008)
#define N 53267                                           /* a second of input */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t fm[2 * N], psg[N], out[2 * 50000];

/* one second of a tone at hz, amplitude a, on the FM's left (psg 0) or the PSG (psg 1) */
/* (the FM's gain is above 1: the levels below are taken back to the input's) */
static int run_with(uint32_t rate, double hz, double a, int to_psg, int dc, uint32_t clock, int32_t fm_gain)
{
    for (int i = 0; i < N; i++) {
        int16_t v = (int16_t)lround(dc + a * sin(2 * M_PI * hz * i / IN_RATE));
        fm[2 * i] = to_psg ? 0 : v; fm[2 * i + 1] = 0;
        psg[i] = to_psg ? v : 0;
    }
    mdrt_mix m;
    mdrt_mix_init(&m, rate);
    if (clock) mdrt_mix_set_clock(&m, clock);
    if (fm_gain) mdrt_mix_set_gains(&m, fm_gain, MDRT_MIX_PSG_GAIN);
    return mdrt_mix_run(&m, fm, psg, N, out);
}

static int run(uint32_t rate, double hz, double a, int to_psg, int dc) { return run_with(rate, hz, a, to_psg, dc, 0, 0); }

static double rms(int side, int from, int to)
{
    double s = 0;
    for (int i = from; i < to; i++) s += (double)out[2 * i + side] * out[2 * i + side];
    return sqrt(s / (to - from));
}

static double frequency(int side, int from, int to, uint32_t rate)
{
    int ups = 0, first = -1, last = -1;
    for (int i = from + 1; i < to; i++)
        if (out[2 * (i - 1) + side] < 0 && out[2 * i + side] >= 0) { ups++; if (first < 0) first = i; last = i; }
    return ups > 1 ? (ups - 1) * (double)rate / (last - first) : 0;
}

/* a PAL console: the same input samples last longer (its master clock is slower); the gains */
static void pal_and_gains(void)
{
    double in_rate = (double)MDRT_MASTER_CLOCK_PAL / 1008;
    int n = run_with(48000, 1000, 10000, 0, 0, MDRT_MASTER_CLOCK_PAL, 0);
    double f = frequency(0, 4800, n, 48000);
    printf("  PAL: %d samples for %d in (expected %.0f); the 1 kHz of NTSC input at %.2f Hz\n", n, N, N * 48000.0 / in_rate - 16 * 48000.0 / in_rate, f);
    CHECK(abs(n - (int)(N * 48000.0 / in_rate - 16 * 48000.0 / in_rate)) <= 2);
    CHECK(fabs(f - 1000 * in_rate / IN_RATE) < 0.05);     /* (the tone was made for NTSC's rate) */
    n = run_with(48000, 1000, 5000, 0, 0, 0, 2 * MDRT_MIX_FM_GAIN);
    double level = rms(0, 4800, n) * sqrt(2) * 32768 / MDRT_MIX_FM_GAIN;
    CHECK(fabs(level - 10000) < 50);
}

int main(void)
{
    pal_and_gains();
    static const uint32_t rates[] = { 48000, 44100, 32000 };
    for (size_t r = 0; r < 3; r++) {
        uint32_t rate = rates[r];
        int n = run(rate, 1000, 10000, 0, 0);
        CHECK(abs(n - (int)(rate - 16 * rate / IN_RATE)) <= 2);   /* a second, less the 16 samples' latency */
        double f = frequency(0, rate / 10, n, rate), level = rms(0, rate / 10, n) * sqrt(2) * 32768 / MDRT_MIX_FM_GAIN;
        printf("  %u Hz: %d samples; 1 kHz in: %.3f Hz, amplitude %.1f, right %.1f\n", rate, n, f, level, rms(1, rate / 10, n));
        CHECK(fabs(f - 1000) < 0.05);
        CHECK(fabs(level - 10000) < 50);
        CHECK(rms(1, rate / 10, n) < 1);
        /* 15 kHz passes (the low-pass is at 20 kHz) */
        n = run(rate, 15000, 10000, 0, 0);
        double pass = rms(0, rate / 10, n) * sqrt(2) * 32768 / MDRT_MIX_FM_GAIN;
        if (rate > 32000) CHECK(fabs(20 * log10(pass / 10000)) < 0.5);
        /* above the output's Nyquist frequency: gone (26 kHz, past the low-pass's transition) */
        n = run(rate, 26000, 10000, 0, 0);
        double stop = rms(0, rate / 10, n) * sqrt(2) * 32768 / MDRT_MIX_FM_GAIN;
        printf("  %u Hz: 15 kHz at %.2f dB, 26 kHz at %.1f dB\n", rate, 20 * log10(pass / 10000), 20 * log10(stop / 10000 + 1e-9));
        if (rate >= 44100) CHECK(20 * log10(stop / 10000 + 1e-9) < -60);
    }
    /* the PSG at its gain */
    int n = run(48000, 1000, 10000, 1, 0);
    double level = rms(0, 4800, n) * sqrt(2);
    CHECK(fabs(level - 10000.0 * MDRT_MIX_PSG_GAIN / 32768) < 10);
    CHECK(fabs(rms(1, 4800, n) * sqrt(2) - level) < 1);      /* on both sides */
    /* DC goes: a constant 8000 is gone after 0.3 s (the high-pass is at 10 Hz) */
    n = run(48000, 1000, 0, 0, 8000);
    int peak = 0;
    for (int i = 14400; i < n; i++) if (abs(out[2 * i]) > peak) peak = abs(out[2 * i]);
    printf("  DC 8000: %d after 0.3 s\n", peak);
    CHECK(peak <= 2);
    return check_done("mix_unit");
}
