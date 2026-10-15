/* Comparing audio by spectrum and envelope (the sound tests: the runtime's chips round
 * differently from the oracle's, so they are not compared sample for sample). */
#ifndef MDRT_TESTS_SPECTRUM_H
#define MDRT_TESTS_SPECTRUM_H
#include <stddef.h>
#include <stdint.h>

/* the power in nb bands spaced evenly in log frequency from f0 to f1 Hz, of n samples at rate Hz
 * (Hann window, zero padded to a power of 2) */
void band_power(const double *x, int n, double rate, double f0, double f1, int nb, double *out);
/* the RMS of n samples about their mean */
double ac_rms(const double *x, int n);

/* A comparison of our audio with the oracle's, in 50 ms windows with sound (loudness above 1% of
 * the loudest window's), over 16 bands from 200 Hz to 5 kHz (above, the oracle's resampler rolls
 * off: docs/HARDWARE.md): the loudness (power in the bands) after one gain for the whole trace
 * (the median ratio), each window's spectrum (the mean difference over the bands within 30 dB of
 * its loudest), its centre (the power-weighted band, in octaves), and the whole trace's spectrum
 * band by band. */
typedef struct audio_cmp {
    int windows;                /* with sound */
    double gain;                /* the oracle's level over ours */
    int loud_ok;                /* windows with the loudness within 1 dB */
    int spec_ok;                /* windows with the spectrum within 3 dB */
    int centre_ok;              /* windows with the centre within 1/3 octave */
    double spec_mean;           /* the windows' mean spectral difference, dB */
    double trace_worst;         /* the whole trace's worst band, dB */
} audio_cmp;
/* a: the oracle's (n_a samples, every stride_a-th value, at rate_a); b: ours likewise */
void audio_compare(const int16_t *a, size_t n_a, int stride_a, double rate_a,
                   const int16_t *b, size_t n_b, int stride_b, double rate_b, audio_cmp *r, int verbose);

#endif
