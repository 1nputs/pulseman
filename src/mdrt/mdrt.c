/* The composition (mdrt/mdrt.h): the modules, the sound timeline of a frame, the handshake. */
#include <stdlib.h>
#include <string.h>
#include "mdrt/mdrt.h"

enum { EV_YM, EV_YM_RESET, EV_PSG };
typedef struct event { uint64_t at; uint32_t seq; uint8_t kind, port, value; } event;

#define CHUNK (16 * MDRT_FM_SAMPLE_MCLKS)                 /* the sound is made a chunk at a time */

struct mdrt {
    mdrt_config cfg;
    mdrt_vdp vdp;
    mdrt_psg psg;
    mdrt_fm *fm;
    mdrt_mix mix;
    mdrt_pads pads;
    mdrt_sram sram;
    uint32_t frame;             /* master clocks a frame */
    uint64_t frames;
    event *ev;                  /* this frame's sound, in the order it came */
    size_t nev, cap;
    uint32_t seq;
    mdrt_sound_tick_fn tick;
    void *tick_user;
    mdrt_pad_fn pad_src;
    void *pad_user;
    uint16_t *picture;          /* 320 x 240 */
    int16_t *fm_buf, *psg_buf, *audio;
    uint32_t beam;              /* the default HV counter's beam, pixel clocks since the handshake */
};

static uint8_t no_memory(void *user, uint32_t addr) { (void)user; (void)addr; return 0; }

/* the default HV counter: the beam from the vertical blank's start, moved on at each read as time
 * passes in the game's code (the game reads it as a random source; lockstep replays recorded
 * values through mdrt_set_hv_source) */
static uint16_t beam_hv(void *user)
{
    mdrt *m = user;
    int width = mdrt_vdp_width(&m->vdp) == 320 ? 420 : 342;
    uint32_t lines = m->frame / MDRT_LINE_MCLKS;
    m->beam += 97;
    uint32_t line = (224 + m->beam / (uint32_t)width) % lines, h = m->beam % (uint32_t)width;
    return (uint16_t)((line & 0xFF) << 8 | (h >> 1));
}

mdrt *mdrt_new(const mdrt_config *c)
{
    mdrt *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cfg = *c;
    if (!m->cfg.audio_rate) m->cfg.audio_rate = 48000;
    m->frame = (c->pal ? MDRT_LINES_PAL : MDRT_LINES_NTSC) * MDRT_LINE_MCLKS;
    mdrt_vdp_init(&m->vdp, c->pal);
    mdrt_vdp_set_memory(&m->vdp, c->read68k ? c->read68k : no_memory, c->read68k_user);
    mdrt_vdp_set_hv_source(&m->vdp, beam_hv, m);
    mdrt_psg_init(&m->psg);
    m->fm = mdrt_fm_new();
    mdrt_mix_init(&m->mix, m->cfg.audio_rate);
    mdrt_pads_init(&m->pads, 1, c->pal);
    mdrt_sram_init(&m->sram, c->sram_size);
    size_t samples = m->frame / MDRT_FM_SAMPLE_MCLKS + 4;
    m->picture = calloc(320 * 240, sizeof *m->picture);
    m->fm_buf = malloc(2 * samples * sizeof *m->fm_buf);
    m->psg_buf = malloc(samples * sizeof *m->psg_buf);
    m->audio = malloc(2 * ((size_t)m->cfg.audio_rate * m->frame / MDRT_MASTER_CLOCK_NTSC + 8) * sizeof *m->audio);
    m->cap = 4096;
    m->ev = malloc(m->cap * sizeof *m->ev);
    if (!m->fm || !m->picture || !m->fm_buf || !m->psg_buf || !m->audio || !m->ev) { mdrt_free(m); return NULL; }
    return m;
}

void mdrt_free(mdrt *m)
{
    if (!m) return;
    mdrt_fm_free(m->fm);
    free(m->picture); free(m->fm_buf); free(m->psg_buf); free(m->audio); free(m->ev);
    free(m);
}

mdrt_vdp *mdrt_get_vdp(mdrt *m) { return &m->vdp; }
mdrt_pads *mdrt_get_pads(mdrt *m) { return &m->pads; }
mdrt_sram *mdrt_get_sram(mdrt *m) { return &m->sram; }
uint32_t mdrt_frame_mclks(const mdrt *m) { return m->frame; }
uint64_t mdrt_frame_count(const mdrt *m) { return m->frames; }

static void push(mdrt *m, uint64_t at, int kind, int port, uint8_t value)
{
    if (m->nev == m->cap) {
        event *e = realloc(m->ev, 2 * m->cap * sizeof *e);
        if (!e) return;                                   /* (out of memory: the write is lost) */
        m->ev = e;
        m->cap *= 2;
    }
    m->ev[m->nev++] = (event){ at, m->seq++, (uint8_t)kind, (uint8_t)port, value };
}

void mdrt_ym_write(mdrt *m, uint32_t at, int port, uint8_t value) { push(m, at, EV_YM, port & 3, value); }
void mdrt_ym_reset(mdrt *m, uint32_t at) { push(m, at, EV_YM_RESET, 0, 0); }
void mdrt_psg_port(mdrt *m, uint32_t at, uint8_t value) { push(m, at, EV_PSG, 0, value); }
void mdrt_set_sound_tick(mdrt *m, mdrt_sound_tick_fn fn, void *user) { m->tick = fn; m->tick_user = user; }
void mdrt_set_hv_source(mdrt *m, mdrt_hv_fn fn, void *user)
{
    if (fn) mdrt_vdp_set_hv_source(&m->vdp, fn, user);
    else mdrt_vdp_set_hv_source(&m->vdp, beam_hv, m);
}
void mdrt_set_pad_source(mdrt *m, mdrt_pad_fn fn, void *user) { m->pad_src = fn; m->pad_user = user; }

static int by_time(const void *x, const void *y)
{
    const event *a = x, *b = y;
    if (a->at != b->at) return a->at < b->at ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

/* the chips run for mclk master clocks; their samples go through the mixer to the frame's audio */
static int run_chips(mdrt *m, uint32_t mclk, int made)
{
    int n = mdrt_fm_run(m->fm, mclk, m->fm_buf, (int)(m->frame / MDRT_FM_SAMPLE_MCLKS + 4));
    mdrt_psg_render(&m->psg, m->psg_buf, n);
    return made + mdrt_mix_run(&m->mix, m->fm_buf, m->psg_buf, n, m->audio + 2 * made);
}

/* the frame's sound: the client's processor through the tick, the writes at their times */
static int make_sound(mdrt *m)
{
    int made = 0;
    uint32_t t = 0;
    size_t next = 0;
    while (t < m->frame) {
        uint32_t until = t + CHUNK < m->frame ? t + CHUNK : m->frame;
        if (m->tick) m->tick(m->tick_user, until);
        qsort(m->ev + next, m->nev - next, sizeof *m->ev, by_time);
        while (next < m->nev && m->ev[next].at < until) {
            event *e = &m->ev[next++];
            if (e->at > t) { made = run_chips(m, (uint32_t)e->at - t, made); t = (uint32_t)e->at; }
            if (e->kind == EV_YM) mdrt_fm_write(m->fm, e->port, e->value);
            else if (e->kind == EV_YM_RESET) mdrt_fm_reset(m->fm);
            else mdrt_psg_write(&m->psg, e->value);
        }
        made = run_chips(m, until - t, made);
        t = until;
    }
    /* what comes after the frame's end belongs to the next */
    size_t keep = 0;
    for (size_t i = next; i < m->nev; i++) {
        m->ev[keep] = m->ev[i];
        m->ev[keep++].at -= m->frame;
    }
    m->nev = keep;
    return made;
}

void mdrt_wait_vblank(mdrt *m)
{
    mdrt_vdp_render(&m->vdp, m->picture, 320);
    int made = make_sound(m);
    const mdrt_platform *p = &m->cfg.platform;
    if (p->present) p->present(p->user, m->picture, mdrt_vdp_width(&m->vdp), mdrt_vdp_height(&m->vdp), 320);
    if (p->audio) p->audio(p->user, m->audio, made);
    if (p->wait) p->wait(p->user);
    uint16_t buttons[2] = { m->pads.buttons[0], m->pads.buttons[1] };
    if (m->pad_src) m->pad_src(m->pad_user, buttons);
    else if (p->input) p->input(p->user, buttons);
    mdrt_pad_set(&m->pads, 0, buttons[0]);
    mdrt_pad_set(&m->pads, 1, buttons[1]);
    mdrt_pads_frame(&m->pads);
    m->frames++;
    m->beam = 0;
    mdrt_vdp_set_position(&m->vdp, 224, 0);
    mdrt_vdp_set_vblank(&m->vdp, 1);
}

void mdrt_vblank_end(mdrt *m) { mdrt_vdp_set_vblank(&m->vdp, 0); }
