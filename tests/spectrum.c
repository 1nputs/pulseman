#include <math.h>
#include <stdlib.h>
#include "spectrum.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* in place, n a power of 2 */
static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int p = i + k, q = p + len / 2;
                double tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr; im[p] += ti;
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

void band_power(const double *x, int n, double rate, double f0, double f1, int nb, double *out)
{
    int m = 1;
    while (m < n) m <<= 1;
    double *re = calloc((size_t)m, sizeof *re), *im = calloc((size_t)m, sizeof *im);
    double mean = 0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (int i = 0; i < n; i++) re[i] = (x[i] - mean) * (0.5 - 0.5 * cos(2 * M_PI * i / (n - 1)));
    fft(re, im, m);
    for (int b = 0; b < nb; b++) out[b] = 0;
    double ratio = log(f1 / f0);
    for (int k = 1; k < m / 2; k++) {
        double f = k * rate / m;
        if (f < f0 || f >= f1) continue;
        int b = (int)(log(f / f0) / ratio * nb);
        if (b >= 0 && b < nb) out[b] += (re[k] * re[k] + im[k] * im[k]) / ((double)n * n);
    }
    free(re); free(im);
}

double ac_rms(const double *x, int n)
{
    double mean = 0, a = 0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (int i = 0; i < n; i++) a += (x[i] - mean) * (x[i] - mean);
    return sqrt(a / n);
}
