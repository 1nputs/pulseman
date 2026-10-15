#include <math.h>
#include <stdio.h>
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
        if (b >= 0 && b < nb) out[b] += (re[k] * re[k] + im[k] * im[k]) / ((double)n * m);   /* (Parseval: any n, m) */
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

#define WINDOW 0.05
#define BANDS 16
#define F0 200.0
#define F1 5000.0

static int cmp_double(const void *x, const void *y) { double p = *(const double *)x, q = *(const double *)y; return p < q ? -1 : p > q; }

void audio_compare(const int16_t *a, size_t n_a, int stride_a, double rate_a,
                   const int16_t *b, size_t n_b, int stride_b, double rate_b, audio_cmp *r, int verbose)
{
    int la = (int)(WINDOW * rate_a), lb = (int)(WINDOW * rate_b);   /* each window starts at its time */
    int nw = (int)((double)n_a / rate_a / WINDOW) - 1;
    if ((double)nw * WINDOW * rate_b + lb > (double)n_b) nw = (int)(((double)n_b - lb) / rate_b / WINDOW);
    double *xa = malloc((size_t)la * sizeof *xa), *xb = malloc((size_t)lb * sizeof *xb);
    double *ra = malloc((size_t)nw * sizeof *ra), *rb = malloc((size_t)nw * sizeof *rb);
    double (*pa)[BANDS] = malloc((size_t)nw * sizeof *pa), (*pb)[BANDS] = malloc((size_t)nw * sizeof *pb);
    double loudest = 0;
    for (int w = 0; w < nw; w++) {
        size_t sa0 = (size_t)llround(w * WINDOW * rate_a), sb0 = (size_t)llround(w * WINDOW * rate_b);
        for (int i = 0; i < la; i++) xa[i] = a[(sa0 + (size_t)i) * stride_a];
        for (int i = 0; i < lb; i++) xb[i] = b[(sb0 + (size_t)i) * stride_b];
        band_power(xa, la, rate_a, F0, F1, BANDS, pa[w]);
        band_power(xb, lb, rate_b, F0, F1, BANDS, pb[w]);
        ra[w] = rb[w] = 0;
        for (int k = 0; k < BANDS; k++) { ra[w] += pa[w][k]; rb[w] += pb[w][k]; }
        ra[w] = sqrt(ra[w]); rb[w] = sqrt(rb[w]);
        if (ra[w] > loudest) loudest = ra[w];
    }
    double *ratio = malloc(((size_t)nw + 1) * sizeof *ratio);
    int active = 0;
    for (int w = 0; w < nw; w++) if (ra[w] > loudest * 0.01 && rb[w] > 0) ratio[active++] = ra[w] / rb[w];
    qsort(ratio, (size_t)active, sizeof *ratio, cmp_double);
    double gain = active ? ratio[active / 2] : 1, sum_spec = 0, avg_a[BANDS] = { 0 }, avg_b[BANDS] = { 0 };
    r->windows = 0; r->loud_ok = r->spec_ok = r->centre_ok = 0;
    for (int w = 0; w < nw; w++) {
        if (!(ra[w] > loudest * 0.01)) continue;
        r->windows++;
        double ldb = 20 * log10(ra[w] / (gain * rb[w] + 1e-9));
        if (fabs(ldb) < 1.0) r->loud_ok++;
        else if (verbose) printf("    window %d (%.2f s): loudness %+.2f dB off (oracle %.0f)\n", w, w * WINDOW, ldb, ra[w]);
        double top = 0, ca = 0, cb = 0, sa = 0, sb = 0;
        for (int k = 0; k < BANDS; k++) {
            if (pa[w][k] > top) top = pa[w][k];
            avg_a[k] += pa[w][k]; avg_b[k] += gain * gain * pb[w][k];
            ca += k * pa[w][k]; sa += pa[w][k]; cb += k * pb[w][k]; sb += pb[w][k];
        }
        double dsum = 0; int dn = 0;
        for (int k = 0; k < BANDS; k++) {
            if (pa[w][k] < top * 1e-3) continue;
            dsum += fabs(10 * log10(pa[w][k] / (gain * gain * pb[w][k] + 1e-12))); dn++;
        }
        double d = dn ? dsum / dn : 0;
        sum_spec += d;
        if (d < 3.0) r->spec_ok++;
        else if (verbose) printf("    window %d (%.2f s): spectrum %.2f dB off\n", w, w * WINDOW, d);
        double oct = sb > 0 ? fabs(ca / sa - cb / sb) * log2(F1 / F0) / BANDS : 99;
        if (oct < 1.0 / 3) r->centre_ok++;
    }
    double avg_top = 0;
    r->trace_worst = 0;
    for (int k = 0; k < BANDS; k++) if (avg_a[k] > avg_top) avg_top = avg_a[k];
    for (int k = 0; k < BANDS; k++) {
        if (avg_a[k] < avg_top * 1e-3) continue;
        double d = fabs(10 * log10(avg_a[k] / (avg_b[k] + 1e-12)));
        if (d > r->trace_worst) r->trace_worst = d;
        if (verbose) printf("    band %2d %6.0f Hz: oracle %6.1f dB, ours %6.1f dB\n", k, F0 * pow(F1 / F0, (k + 0.5) / BANDS),
                            10 * log10(avg_a[k] / r->windows), 10 * log10(avg_b[k] / r->windows));
    }
    r->gain = gain;
    r->spec_mean = r->windows ? sum_spec / r->windows : 0;
    free(xa); free(xb); free(ra); free(rb); free(pa); free(pb); free(ratio);
}
