/* Comparing audio by spectrum and envelope (the sound tests: the runtime's chips round
 * differently from the oracle's, so they are not compared sample for sample). */
#ifndef MDRT_TESTS_SPECTRUM_H
#define MDRT_TESTS_SPECTRUM_H

/* the power in nb bands spaced evenly in log frequency from f0 to f1 Hz, of n samples at rate Hz
 * (Hann window, zero padded to a power of 2) */
void band_power(const double *x, int n, double rate, double f0, double f1, int nb, double *out);
/* the RMS of n samples about their mean */
double ac_rms(const double *x, int n);

#endif
