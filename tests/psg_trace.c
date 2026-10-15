/* WP3: the PSG writes of every sound trace in $MDRT_VECTORS recorded from power on with the FM chip
 * left out of the oracle's audio (fm_preamp 0: ../nhl95/tools/oracle/sndtrace.py --fm-preamp 0), replayed
 * through the model, compared with the oracle's audio in 50 ms windows with sound (loudness above
 *  1% of the loudest window's): the loudness (the power in 16 bands from 200 Hz to 5 kHz, after one
 * gain for the whole trace) within 1 dB, the spectrum (those bands; the mean difference over the bands
 * within 30 dB of the window's loudest) within 3 dB, and its centre within 1/3 octave, each in
 * 95% of the windows (a 50 ms window of noise varies by a few dB band by band, and the noise is
 * not in phase with the oracle's: writes land on sample boundaries here); and the whole trace's
 * spectrum within 1.5 dB in every band. The gain is the median over the windows of the loudness
 * ratio. Printed: the gain, which the mixer's PSG level
 * comes from (WP4). */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"
#include "mdrt/psg.h"
#include "sndtrace.h"
#include "spectrum.h"

#define WINDOW 0.05
#define BANDS 16
#define F0 200.0                 /* the bands: 200 Hz to 5 kHz (above, the oracle's resampler rolls */
#define F1 5000.0                /* off: -1.6 dB at 7.3 kHz, -2 dB at 11.6 kHz, -3.6 dB at 14.4 kHz) */

static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

/* 1 when the trace is one for this test */
static int replay(const char *path, const char *name)
{
    sndtrace t;
    if (!sndtrace_load(&t, path)) { printf("  %s: not a version 3 sndtrace\n", name); check_failures++; return 0; }
    if (t.fm_preamp != 0 || t.psg_preamp == 0 || !t.power_on) { sndtrace_free(&t); return 0; }
    /* the model's output over the trace, the writes at their sample */
    size_t n = (size_t)(t.end / MDRT_PSG_SAMPLE_MCLKS), done = 0;
    int16_t *ours = malloc(n * sizeof *ours);
    mdrt_psg p;
    mdrt_psg_init(&p);
    st_event e;
    long writes = 0;
    while (sndtrace_next(&t, &e)) {
        if (e.tag != ST_PSG) continue;
        size_t at = (size_t)(e.time / MDRT_PSG_SAMPLE_MCLKS);
        if (at > n) at = n;
        if (at > done) { mdrt_psg_render(&p, ours + done, (int)(at - done)); done = at; }
        mdrt_psg_write(&p, e.v);
        writes++;
    }
    if (n > done) mdrt_psg_render(&p, ours + done, (int)(n - done));
    /* windows */
    double our_rate = (double)MDRT_MASTER_CLOCK_NTSC / MDRT_PSG_SAMPLE_MCLKS;
    int lo = (int)(WINDOW * t.rate), lr = (int)(WINDOW * our_rate);
    int nw = (int)(t.frames / (size_t)lo);
    if ((size_t)nw * lr > n) nw = (int)(n / (size_t)lr);
    double *a = malloc((size_t)lo * sizeof *a), *b = malloc((size_t)lr * sizeof *b);
    double *ra = malloc((size_t)nw * sizeof *ra), *rb = malloc((size_t)nw * sizeof *rb);
    double (*pa)[BANDS] = malloc((size_t)nw * sizeof *pa), (*pb)[BANDS] = malloc((size_t)nw * sizeof *pb);
    double loudest = 0;
    for (int w = 0; w < nw; w++) {
        for (int i = 0; i < lo; i++) a[i] = t.audio[2 * ((size_t)w * lo + i)];
        for (int i = 0; i < lr; i++) b[i] = ours[(size_t)w * lr + i];
        band_power(a, lo, t.rate, F0, F1, BANDS, pa[w]);
        band_power(b, lr, our_rate, F0, F1, BANDS, pb[w]);
        ra[w] = rb[w] = 0;                                /* the loudness: the power in the bands */
        for (int k = 0; k < BANDS; k++) { ra[w] += pa[w][k]; rb[w] += pb[w][k]; }
        ra[w] = sqrt(ra[w]); rb[w] = sqrt(rb[w]);
        if (ra[w] > loudest) loudest = ra[w];
    }
    /* the gain: the median ratio over the windows with sound */
    double *ratio = malloc((size_t)nw * sizeof *ratio);
    int active = 0;
    for (int w = 0; w < nw; w++) if (ra[w] > loudest * 0.01 && rb[w] > 0) ratio[active++] = ra[w] / rb[w];
    qsort(ratio, (size_t)active, sizeof *ratio, cmp);
    double gain = active ? ratio[active / 2] : 1;
    int loud_ok = 0, spec_ok = 0, centre_ok = 0, nc = 0;
    double *centre = malloc((size_t)nw * sizeof *centre);
    double worst_spec = 0, sum_spec = 0, avg_a[BANDS] = { 0 }, avg_b[BANDS] = { 0 };
    for (int w = 0; w < nw; w++) {
        if (!(ra[w] > loudest * 0.01)) continue;
        double db = 20 * log10(ra[w] / (gain * rb[w] + 1e-9));
        if (fabs(db) < 1.0) loud_ok++;
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
        if (d > worst_spec) worst_spec = d;
        if (d < 3.0) spec_ok++;
        else if (getenv("PSG_BAD")) printf("    window %d (%.2f s): %.2f dB, oracle rms %.0f\n", w, w * WINDOW, d, ra[w]);
        /* the centre of the spectrum (the power-weighted band) */
        double oct = sb > 0 ? fabs(ca / sa - cb / sb) * log2(F1 / F0) / BANDS : 99;
        centre[nc++] = oct;
        if (oct < 1.0 / 3) centre_ok++;
    }
    /* the whole trace's spectrum, band by band */
    double avg_worst = 0, avg_top = 0;
    for (int k = 0; k < BANDS; k++) if (avg_a[k] > avg_top) avg_top = avg_a[k];
    for (int k = 0; k < BANDS; k++) {
        if (avg_a[k] < avg_top * 1e-3) continue;
        double d = fabs(10 * log10(avg_a[k] / (avg_b[k] + 1e-12)));
        if (d > avg_worst) avg_worst = d;
        if (getenv("PSG_BAD")) printf("    band %2d %6.0f Hz: oracle %6.1f dB, ours %6.1f dB\n", k, F0 * pow(F1 / F0, (k + 0.5) / BANDS),
                                      10 * log10(avg_a[k] / active), 10 * log10(avg_b[k] / active));
    }
    printf("  %s: %ld PSG writes, %d windows of %d ms with sound; gain %.4f (%.1f dB); loudness within 1 dB in %d; "
           "spectrum within 3 dB in %d (mean %.2f dB, worst %.2f dB), centre within 1/3 octave in %d; "
           "the whole trace's spectrum within %.2f dB in every band\n",
           name, writes, active, (int)(WINDOW * 1000), gain, 20 * log10(gain), loud_ok, spec_ok,
           active ? sum_spec / active : 0, worst_spec, centre_ok, avg_worst);
    qsort(centre, (size_t)nc, sizeof *centre, cmp);
    if (getenv("PSG_BAD")) printf("    centre differences: median %.3f, 90%% %.3f, 95%% %.3f octave\n", centre[nc / 2], centre[nc * 9 / 10], centre[nc * 95 / 100]);
    free(centre);
    CHECK(active > 100);
    CHECK(loud_ok >= active * 0.95);
    CHECK(spec_ok >= active * 0.95);
    CHECK(centre_ok >= active * 0.95);
    CHECK(avg_worst < 1.5);
    free(ours); free(a); free(b); free(ra); free(rb); free(pa); free(pb); free(ratio);
    sndtrace_free(&t);
    return 1;
}

int main(void)
{
    int traces = 0;
    const char *dir = check_vectors();
    if (!dir) { printf("psg_trace: no vectors, skipped\n"); return 0; }
    DIR *d = opendir(dir);
    struct dirent *ent;
    char path[4096];
    while (d && (ent = readdir(d))) {
        size_t l = strlen(ent->d_name);
        if (l < 9 || strcmp(ent->d_name + l - 9, ".sndtrace")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        traces += replay(path, ent->d_name);
    }
    if (d) closedir(d);
    if (!traces) printf("psg_trace: no trace from power on without FM (sndtrace.py --fm-preamp 0), skipped\n");
    return check_done("psg_trace");
}
