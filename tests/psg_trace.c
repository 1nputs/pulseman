/* WP3: the PSG writes of every sound trace in $MDRT_VECTORS recorded from power on with the FM chip
 * left out of the oracle's audio (fm_preamp 0: ../nhl95/tools/oracle/sndtrace.py --fm-preamp 0),
 * replayed through the model, compared with the oracle's audio (tests/spectrum.h): the loudness
 * within 1 dB, each window's spectrum within 3 dB and its centre within 1/3 octave, each in 99% of
 * the windows, and the whole trace's spectrum within 1 dB in every band. Printed: the gain, which
 * with fm_trace's gives the mixer's balance (mdrt/mix.h). PSG_DEBUG=1 prints the windows off and
 * the bands. */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"
#include "mdrt/psg.h"
#include "sndtrace.h"
#include "spectrum.h"

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
    audio_cmp r;
    audio_compare(t.audio, t.frames, 2, t.rate, ours, n, 1, (double)MDRT_MASTER_CLOCK_NTSC / MDRT_PSG_SAMPLE_MCLKS,
                  &r, getenv("PSG_DEBUG") != NULL);
    printf("  %s: %ld PSG writes, %d windows of 50 ms with sound; gain %.4f (%.1f dB); loudness within 1 dB in %d, "
           "spectrum within 3 dB in %d (mean %.2f dB), centre within 1/3 octave in %d; the whole trace's spectrum "
           "within %.2f dB in every band\n", name, writes, r.windows, r.gain, 20 * log10(r.gain), r.loud_ok, r.spec_ok,
           r.spec_mean, r.centre_ok, r.trace_worst);
    CHECK(r.windows > 100);
    CHECK(r.loud_ok >= r.windows * 0.99);
    CHECK(r.spec_ok >= r.windows * 0.99);
    CHECK(r.centre_ok >= r.windows * 0.99);
    CHECK(r.trace_worst < 1.0);
    free(ours);
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
