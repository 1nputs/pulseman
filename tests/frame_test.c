/* WP6: a test game on the composition, as a client would run one: it draws a moving pattern
 * through the VDP's ports, plays a scale on the PSG and on the FM chip with timed writes, has a
 * "sound processor" (the sound tick) stream DAC samples every 1800 master clocks like NHL '94's
 * Z80 does, and blocks in mdrt_wait_vblank. Headless, 600 frames (10 s): the audio's length
 * follows the frame rate with no drift, the run is deterministic (two runs, the same pictures and
 * samples), and the notes come out at their pitches. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"

#define FRAMES 600
#define RATE 48000

typedef struct run {
    uint64_t pic_hash, snd_hash;
    long samples;
    int per_frame_min, per_frame_max, frames_seen;
    uint64_t last_pic;
    int pictures_differ;
    int16_t *all;               /* every sample, for the pitch check */
    long cap;
} run;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static void present(void *user, const uint16_t *px, int w, int h, int pitch)
{
    run *r = user;
    uint64_t f = 0xCBF29CE484222325ull;
    for (int y = 0; y < h; y++) f = fnv(f, px + y * pitch, (size_t)w * 2);
    if (r->frames_seen && f != r->last_pic) r->pictures_differ++;
    r->last_pic = f;
    r->pic_hash = fnv(r->pic_hash, &f, sizeof f);
}

static void audio(void *user, const int16_t *s, int frames)
{
    run *r = user;
    r->snd_hash = fnv(r->snd_hash, s, (size_t)frames * 4);
    if (r->samples + frames <= r->cap) memcpy(r->all + 2 * r->samples, s, (size_t)frames * 4);
    r->samples += frames;
    if (r->frames_seen > 2) {                             /* (after the mixer's start) */
        if (frames < r->per_frame_min) r->per_frame_min = frames;
        if (frames > r->per_frame_max) r->per_frame_max = frames;
    }
    r->frames_seen++;
}

/* the sound processor: a DAC sample every 1800 master clocks (a square wave of 16 samples), from
 * where it stopped to `until` */
typedef struct dac { mdrt *m; uint64_t next, base; int n; } dac;
static void tick(void *user, uint32_t until)
{
    dac *d = user;
    while (d->next < d->base + until) {
        uint32_t at = (uint32_t)(d->next - d->base);
        mdrt_ym_write(d->m, at, 0, 0x2A);
        mdrt_ym_write(d->m, at + 765, 1, (d->n++ / 8) & 1 ? 0xA0 : 0x60);
        d->next += 1800;
    }
}

static const int notes[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };   /* C major */

static void game(mdrt *m)
{
    mdrt_vdp *v = mdrt_get_vdp(m);
    static const uint8_t regs[] = { 0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
    for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
    mdrt_vdp_ctrl32(v, 0xC0000000);                       /* CRAM: a few colours */
    for (int c = 0; c < 16; c++) mdrt_vdp_data16(v, (uint16_t)((c & 7) << 1 | (c >> 1 & 7) << 5 | (7 - (c & 7)) << 9));
    mdrt_vdp_ctrl32(v, 0x40200000);                       /* tile 1: stripes */
    for (int i = 0; i < 16; i++) mdrt_vdp_data16(v, (uint16_t)(0x1234 * (i + 1)));
    mdrt_vdp_ctrl32(v, 0x40000003);                       /* plane A at $C000: tile 1 everywhere */
    for (int i = 0; i < 64 * 32; i++) mdrt_vdp_data16(v, 1);
    dac d = { m, 0, 0, 0 };
    mdrt_set_sound_tick(m, tick, &d);
    mdrt_ym_write(m, 0, 0, 0x2B); mdrt_ym_write(m, 700, 1, 0x80);   /* the DAC on */
    for (int f = 0; f < FRAMES; f++) {
        /* in the vertical blank: scroll, and the next note every 30 frames */
        mdrt_vdp_ctrl32(v, 0x5C000003);                   /* the hscroll table, $DC00 (register 13) */
        mdrt_vdp_data16(v, (uint16_t)(f * 3));
        if (f % 30 == 0) {
            int hz = notes[(f / 30) % 8], n = (int)lround(MDRT_Z80_CLOCK_NTSC / (32.0 * hz));
            mdrt_psg_port(m, 2000, (uint8_t)(0x80 | (n & 15)));
            mdrt_psg_port(m, 2100, (uint8_t)(n >> 4));
            mdrt_psg_port(m, 2200, 0x90);                 /* tone 0 at full volume */
        }
        mdrt_vblank_end(m);
        mdrt_wait_vblank(m);
        d.base += mdrt_frame_mclks(m);
    }
}

static void play(run *r)
{
    memset(r, 0, sizeof *r);
    r->per_frame_min = 1 << 30;
    r->cap = (long)RATE * 11;
    r->all = calloc((size_t)r->cap * 2, sizeof *r->all);
    mdrt_config c = { 0 };
    c.audio_rate = RATE;
    c.platform = (mdrt_platform){ .user = r, .present = present, .audio = audio };
    mdrt *m = mdrt_new(&c);
    game(m);
    mdrt_free(m);
}

int main(void)
{
    run a, b;
    play(&a);
    play(&b);
    double expect = FRAMES * 896040.0 / MDRT_MASTER_CLOCK_NTSC * RATE - 16.0 * RATE / (MDRT_MASTER_CLOCK_NTSC / 1008.0);
    printf("  %d frames: %ld samples at %d Hz (expected %.1f), %d to %d a frame; pictures changing in %d frames\n",
           FRAMES, a.samples, RATE, expect, a.per_frame_min, a.per_frame_max, a.pictures_differ);
    CHECK(fabs(a.samples - expect) <= 2);
    CHECK(a.per_frame_min >= 800 && a.per_frame_max <= 802);   /* 801.04 a frame: a frame is 888.9 FM samples */
    CHECK(a.pictures_differ >= FRAMES - 2);
    CHECK(a.pic_hash == b.pic_hash);
    CHECK(a.snd_hash == b.snd_hash);
    CHECK_EQ(a.samples, b.samples);
    /* the scale: each note's pitch (zero crossings of the left side, after the DAC's square is
     * taken out by averaging over its period... the PSG note dominates above 200 Hz) */
    int good = 0;
    for (int k = 1; k < 8; k++) {
        long from = (long)(k * 30 * 801.04) + 2000, to = from + 8000;
        double best = 0; int best_hz = 0;
        for (int i = 0; i < 8; i++) {                     /* the note whose Goertzel power is highest */
            double w = 2 * 3.14159265358979 * notes[i] / RATE, s1 = 0, s2 = 0;
            for (long j = from; j < to; j++) { double s0 = a.all[2 * j] + 2 * cos(w) * s1 - s2; s2 = s1; s1 = s0; }
            double pw = s1 * s1 + s2 * s2 - 2 * cos(w) * s1 * s2;
            if (pw > best) { best = pw; best_hz = notes[i]; }
        }
        if (best_hz == notes[k % 8]) good++;
    }
    printf("  the scale: %d / 7 notes at their pitch\n", good);
    CHECK_EQ(good, 7);
    free(a.all); free(b.all);
    return check_done("frame_test");
}
