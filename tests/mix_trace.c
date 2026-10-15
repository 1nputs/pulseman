/* WP4: every sound trace in $MDRT_VECTORS recorded from power on with both chips in the oracle's
 * audio: the YM2612 and PSG writes replayed at their master clocks through the FM chip, the PSG and
 * the mixer at the oracle's rate, compared with the oracle's audio side by side (tests/spectrum.h:
 * the balance of the two chips shows in the loudness and the spectrum), and no sample clipped.
 * MIX_DEBUG=1 prints the windows off and the bands; MIX_WAV=file writes ours as a .wav. */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/fm.h"
#include "mdrt/mdrt.h"
#include "mdrt/mix.h"
#include "mdrt/psg.h"
#include "sndtrace.h"
#include "spectrum.h"

static void wav(const char *path, const int16_t *s, size_t frames, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)(frames * 4), v;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t h[2] = { 1, 2 }; fwrite(h, 2, 2, f);
    fwrite(&rate, 4, 1, f); v = rate * 4; fwrite(&v, 4, 1, f);
    h[0] = 4; h[1] = 16; fwrite(h, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s, 4, frames, f);                              /* (a little-endian host) */
    fclose(f);
}

static int replay(const char *path, const char *name)
{
    sndtrace t;
    if (!sndtrace_load(&t, path)) { printf("  %s: not a version 3 sndtrace\n", name); check_failures++; return 0; }
    if (t.psg_preamp == 0 || t.fm_preamp == 0 || !t.power_on) { sndtrace_free(&t); return 0; }
    size_t cap = (size_t)((double)t.end / MDRT_MASTER_CLOCK_NTSC * t.rate) + 64, made = 0;
    int16_t *ours = calloc(2 * cap, sizeof *ours);
    mdrt_fm *f = mdrt_fm_new();
    mdrt_psg p;
    mdrt_psg_init(&p);
    mdrt_mix m;
    mdrt_mix_init(&m, t.rate);
    static int16_t fm[2 * 4096], psg[4096];
    uint64_t now = 0;
    int in_reset = 1;
    st_event e;
    int more = 1;
    while (more) {
        more = sndtrace_next(&t, &e);
        uint64_t to = more ? e.time : t.end;
        while (to > now) {                                /* the chips up to the event, in pieces */
            uint32_t step = to - now > 1000u * MDRT_FM_SAMPLE_MCLKS ? 1000u * MDRT_FM_SAMPLE_MCLKS : (uint32_t)(to - now);
            int n = mdrt_fm_run(f, step, fm, 4096);
            mdrt_psg_render(&p, psg, n);
            if (made + (size_t)n * t.rate / 53267 + 2 < cap) made += (size_t)mdrt_mix_run(&m, fm, psg, n, ours + 2 * made);
            now += step;
        }
        if (!more) break;
        if (e.tag == ST_YM) mdrt_fm_write(f, e.a, e.v);
        else if (e.tag == ST_PSG) mdrt_psg_write(&p, e.v);
        else if (e.tag == ST_Z80RESET) {                  /* the Z80's reset line resets the FM chip */
            if (!(e.addr & 0x100)) { mdrt_fm_reset(f); in_reset = 1; }
            else if (in_reset) { mdrt_fm_reset(f); in_reset = 0; }
        }
    }
    size_t clipped = 0;
    int peak = 0;
    for (size_t i = 0; i < 2 * made; i++) {
        if (ours[i] == 32767 || ours[i] == -32768) clipped++;
        if (abs(ours[i]) > peak) peak = abs(ours[i]);
    }
    if (getenv("MIX_WAV")) wav(getenv("MIX_WAV"), ours, made, t.rate);
    for (int side = 0; side < 2; side++) {
        audio_cmp r;
        audio_compare(t.audio + side, t.frames, 2, t.rate, ours + side, made, 2, t.rate, &r, getenv("MIX_DEBUG") != NULL);
        printf("  %s %s: %d windows of 50 ms with sound; gain %.4f (%.1f dB); loudness within 1 dB in %d, spectrum within 3 dB "
               "in %d (mean %.2f dB), centre within 1/3 octave in %d; the whole trace's spectrum within %.2f dB in every band\n",
               name, side ? "right" : "left", r.windows, r.gain, 20 * log10(r.gain), r.loud_ok, r.spec_ok, r.spec_mean,
               r.centre_ok, r.trace_worst);
        CHECK(r.windows > 100);
        CHECK(r.loud_ok >= r.windows * 0.99);
        CHECK(r.spec_ok >= r.windows * 0.99);
        CHECK(r.centre_ok >= r.windows * 0.99);
        CHECK(r.trace_worst < 1.0);
    }
    printf("  %s: %zu samples at %u Hz, peak %d, %zu clipped\n", name, made, t.rate, peak, clipped);
    CHECK_EQ(clipped, 0);
    mdrt_fm_free(f);
    free(ours);
    sndtrace_free(&t);
    return 1;
}

int main(void)
{
    int traces = 0;
    const char *dir = check_vectors();
    if (!dir) { printf("mix_trace: no vectors, skipped\n"); return 0; }
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
    if (!traces) printf("mix_trace: no trace from power on with both chips, skipped\n");
    return check_done("mix_trace");
}
