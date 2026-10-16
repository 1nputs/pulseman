/* WP4: the YM2612 port writes of every sound trace in $MDRT_VECTORS recorded from power on with
 * the PSG left out of the oracle's audio (psg_preamp 0: ../ninety-four/tools/oracle/sndtrace.py
 * --psg-preamp 0), replayed through the FM chip at their master clocks (the Z80's reset line
 * resets the chip too), compared with the oracle's audio side by side (tests/spectrum.h). The
 * oracle's FM core is another one (MAME's YM2612 in Genesis Plus GX), so the comparison is by
 * loudness and spectrum (99% of the windows, the whole trace within 1 dB), like the PSG's; a
 * trace recorded with the oracle on Nuked-OPN2 too (sndtrace.py --ym2612 nuked) compares the same
 * core. Printed: the gain, the mixer's FM level (mdrt/mix.h).
 * FM_DEBUG=1 prints the windows off and the bands. */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/fm.h"
#include "mdrt/mdrt.h"
#include "sndtrace.h"
#include "spectrum.h"

static int replay(const char *path, const char *name, const char *core)
{
    sndtrace t;
    if (!sndtrace_load(&t, path)) { printf("  %s: not a version 3 sndtrace\n", name); check_failures++; return 0; }
    if (t.psg_preamp != 0 || t.fm_preamp == 0 || !t.power_on) { sndtrace_free(&t); return 0; }
    size_t n = (size_t)(t.end / MDRT_FM_SAMPLE_MCLKS) + 2, done = 0;
    int16_t *ours = calloc(2 * n, sizeof *ours);
    mdrt_fm *f = mdrt_fm_new_core(core, MDRT_FM_YM2612);
    uint64_t now = 0;
    int in_reset = 1;
    long writes = 0;
    st_event e;
    while (sndtrace_next(&t, &e)) {
        if (e.tag != ST_YM && e.tag != ST_Z80RESET) continue;
        while (e.time > now) {                            /* run the chip up to the write */
            uint64_t step = e.time - now > 1u << 30 ? 1u << 30 : e.time - now;
            done += (size_t)mdrt_fm_run(f, (uint32_t)step, ours + 2 * done, (int)(n - done));
            now += step;
        }
        if (e.tag == ST_YM) { mdrt_fm_write(f, e.a, e.v); writes++; }
        else if (e.addr & 0x100) { if (in_reset) mdrt_fm_reset(f); in_reset = 0; }   /* released */
        else in_reset = 1;
    }
    while (t.end > now) {
        uint64_t step = t.end - now > 1u << 30 ? 1u << 30 : t.end - now;
        done += (size_t)mdrt_fm_run(f, (uint32_t)step, ours + 2 * done, (int)(n - done));
        now += step;
    }
    double rate = (double)MDRT_MASTER_CLOCK_NTSC / MDRT_FM_SAMPLE_MCLKS;
    for (int side = 0; side < 2; side++) {
        audio_cmp r;
        audio_compare(t.audio + side, t.frames, 2, t.rate, ours + side, done, 2, rate, &r, getenv("FM_DEBUG") != NULL);
        printf("  %s, %s, %s: %ld port writes, %d windows of 50 ms with sound; gain %.4f (%.1f dB); loudness within 1 dB in %d, "
               "spectrum within 3 dB in %d (mean %.2f dB), centre within 1/3 octave in %d; the whole trace's spectrum "
               "within %.2f dB in every band\n", name, core, side ? "right" : "left", writes, r.windows, r.gain, 20 * log10(r.gain),
               r.loud_ok, r.spec_ok, r.spec_mean, r.centre_ok, r.trace_worst);
        CHECK(r.windows > 100);
        CHECK(r.loud_ok >= r.windows * 0.99);
        CHECK(r.spec_ok >= r.windows * 0.99);
        CHECK(r.centre_ok >= r.windows * 0.99);
        CHECK(r.trace_worst < 1.0);
    }
    mdrt_fm_free(f);
    free(ours);
    sndtrace_free(&t);
    return 1;
}

int main(void)
{
    int traces = 0;
    const char *dir = check_vectors();
    if (!dir) return check_skip("fm_trace", "no vectors (MDRT_VECTORS)");
    DIR *d = opendir(dir);
    struct dirent *ent;
    char path[4096];
    while (d && (ent = readdir(d))) {
        size_t l = strlen(ent->d_name);
        if (l < 9 || strcmp(ent->d_name + l - 9, ".sndtrace")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        for (int i = 0; mdrt_fm_cores(i); i++) traces += replay(path, ent->d_name, mdrt_fm_cores(i));
    }
    if (d) closedir(d);
    if (!traces) check_skip("fm_trace", "no trace from power on without the PSG (sndtrace.py --psg-preamp 0)");
    return check_done("fm_trace");
}
