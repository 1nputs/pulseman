/* The composition (mdrt/mdrt.h): the modules, the sound timeline of a frame, the handshake. */
#include <stdlib.h>
#include <string.h>
#include "mdrt/mdrt.h"
#include "../state.h"

enum { EV_YM, EV_YM_RESET, EV_PSG };
typedef struct event { uint64_t at; uint8_t kind, port, value; } event;

#define CHUNK (16 * MDRT_FM_SAMPLE_MCLKS)                 /* the sound is made a chunk at a time */

#define MAX_PARTS 16
typedef struct part {
    char tag[4];
    mdrt_part_save_fn save;
    mdrt_part_load_fn load;
    void *user;
    void *mem;                  /* mdrt_add_state_memory's block */
    size_t mem_size;
} part;

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
    event *ev;                  /* this frame's sound: by time, and in the order it came at one time */
    size_t nev, cap;
    size_t next;                /* the next event the sound takes (while it is made; else 0) */
    mdrt_sound_tick_fn tick;
    void *tick_user;
    mdrt_pad_fn pad_src;
    void *pad_user;
    uint16_t *picture;          /* 320 x 240 */
    int16_t *fm_buf, *psg_buf, *audio;
    uint32_t beam;              /* the default HV counter's beam, pixel clocks since the handshake */
    unsigned requests;          /* the player's, from the last handshake */
    part parts[MAX_PARTS];      /* the client's parts of a state */
    int nparts;
};

static uint8_t no_memory(void *user, uint32_t addr) { (void)user; (void)addr; return 0; }

/* the default HV counter: the beam from the vertical blank's start, moved on at each read as time
 * passes in the game's code (the game reads it as a random source; lockstep replays recorded
 * values through mdrt_set_hv_source), shown as the hardware's counters show it */
static uint16_t beam_hv(void *user)
{
    mdrt *m = user;
    int width = mdrt_vdp_width(&m->vdp) == 320 ? 420 : 342;
    uint32_t lines = m->frame / MDRT_LINE_MCLKS;
    m->beam += 97;
    uint32_t line = (224 + m->beam / (uint32_t)width) % lines, h = m->beam % (uint32_t)width;
    return mdrt_vdp_hv_at(&m->vdp, line, h);
}

static uint32_t master_clock(const mdrt *m) { return m->cfg.pal ? MDRT_MASTER_CLOCK_PAL : MDRT_MASTER_CLOCK_NTSC; }

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
    mdrt_mix_set_clock(&m->mix, master_clock(m));
    mdrt_mix_set_gains(&m->mix, c->fm_gain ? c->fm_gain : MDRT_MIX_FM_GAIN, c->psg_gain ? c->psg_gain : MDRT_MIX_PSG_GAIN);
    mdrt_pads_init(&m->pads, 1, c->pal);
    mdrt_sram_init(&m->sram, c->sram_size);
    size_t samples = m->frame / MDRT_FM_SAMPLE_MCLKS + 4;
    m->picture = calloc(320 * 240, sizeof *m->picture);
    m->fm_buf = malloc(2 * samples * sizeof *m->fm_buf);
    m->psg_buf = malloc(samples * sizeof *m->psg_buf);
    m->audio = malloc(2 * ((size_t)m->cfg.audio_rate * m->frame / master_clock(m) + 8) * sizeof *m->audio);
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
uint32_t mdrt_master_clock(const mdrt *m) { return master_clock(m); }
uint32_t mdrt_frame_mclks(const mdrt *m) { return m->frame; }
uint64_t mdrt_frame_count(const mdrt *m) { return m->frames; }

/* an event into the list by its time, after those at the same time; never before the next one
 * the sound takes (a write late for the sound being made is made at once) */
static void push(mdrt *m, uint64_t at, int kind, int port, uint8_t value)
{
    if (m->nev == m->cap) {
        event *e = realloc(m->ev, 2 * m->cap * sizeof *e);
        if (!e) return;                                   /* (out of memory: the write is lost) */
        m->ev = e;
        m->cap *= 2;
    }
    size_t i = m->nev++;
    while (i > m->next && m->ev[i - 1].at > at) { m->ev[i] = m->ev[i - 1]; i--; }
    m->ev[i] = (event){ at, (uint8_t)kind, (uint8_t)port, value };
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
    m->next = 0;
    while (t < m->frame) {
        uint32_t until = t + CHUNK < m->frame ? t + CHUNK : m->frame;
        if (m->tick) m->tick(m->tick_user, until);
        while (m->next < m->nev && m->ev[m->next].at < until) {
            event *e = &m->ev[m->next++];
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
    for (size_t i = m->next; i < m->nev; i++) {
        m->ev[keep] = m->ev[i];
        m->ev[keep++].at -= m->frame;
    }
    m->nev = keep;
    m->next = 0;
    return made;
}

int mdrt_wait_vblank(mdrt *m)
{
    mdrt_vdp_render(&m->vdp, m->picture, 320);
    int made = make_sound(m);
    const mdrt_platform *p = &m->cfg.platform;
    if (p->present) p->present(p->user, m->picture, mdrt_vdp_width(&m->vdp), mdrt_vdp_height(&m->vdp), 320);
    if (p->audio) p->audio(p->user, m->audio, made);
    int stop = p->wait ? p->wait(p->user) : 0;
    uint16_t buttons[2] = { m->pads.buttons[0], m->pads.buttons[1] };
    if (m->pad_src) m->pad_src(m->pad_user, buttons);
    else if (p->input) p->input(p->user, buttons);
    mdrt_pad_set(&m->pads, 0, buttons[0]);
    mdrt_pad_set(&m->pads, 1, buttons[1]);
    mdrt_pads_frame(&m->pads);
    m->requests = p->requests ? p->requests(p->user) : 0;
    m->frames++;
    m->beam = 0;
    mdrt_vdp_set_position(&m->vdp, 224, 0);
    mdrt_vdp_set_vblank(&m->vdp, 1);
    return stop;
}

void mdrt_vblank_end(mdrt *m) { mdrt_vdp_set_vblank(&m->vdp, 0); }
unsigned mdrt_requests(const mdrt *m) { return m->requests; }

/* ---- states -------------------------------------------------------------------------------
 * "MDRS", version u32 = 1, then sections: tag[4], size u32, bytes. In this order: CONS (PAL u8,
 * the frame's length u32, frames u64, the beam u32), VDP, PSG, FM (the chip's wrapper, its core's
 * name and the core's state), MIX, SND (the sound to come: count u32, then each at u64 (master
 * clocks into this frame), kind u8, port u8, value u8), PADS, SRAM, then the client's parts in the
 * order they were added. Integers little-endian. */

static size_t mem_save(void *user, uint8_t *out)
{
    part *p = user;
    if (out) memcpy(out, p->mem, p->mem_size);
    return p->mem_size;
}
static int mem_load(void *user, const uint8_t *in, size_t size)
{
    part *p = user;
    if (size != p->mem_size) return -1;
    memcpy(p->mem, in, size);
    return 0;
}

static int add_part(mdrt *m, const char *tag, mdrt_part_save_fn save, mdrt_part_load_fn load, void *user, void *mem, size_t size)
{
    if (m->nparts == MAX_PARTS || strlen(tag) != 4) return -1;
    for (int i = 0; i < m->nparts; i++) if (!memcmp(m->parts[i].tag, tag, 4)) return -1;
    part *p = &m->parts[m->nparts++];
    memcpy(p->tag, tag, 4);
    p->save = save; p->load = load; p->user = mem ? p : user;
    p->mem = mem; p->mem_size = size;
    return 0;
}
int mdrt_add_state_part(mdrt *m, const char *tag, mdrt_part_save_fn save, mdrt_part_load_fn load, void *user)
{
    return add_part(m, tag, save, load, user, NULL, 0);
}
int mdrt_add_state_memory(mdrt *m, const char *tag, void *data, size_t size)
{
    return add_part(m, tag, mem_save, mem_load, NULL, data, size);
}

/* a section: its tag and size, then what write puts there */
static size_t begin(st_out *o, const char *tag) { st_put(o, tag, 4); st_u32(o, 0); return o->n; }
static void end(st_out *o, size_t at)
{
    if (!o->p) return;
    uint32_t n = (uint32_t)(o->n - at);
    for (int i = 0; i < 4; i++) o->p[at - 4 + (size_t)i] = (uint8_t)(n >> (8 * i));
}
static uint8_t *here(st_out *o) { return o->p ? o->p + o->n : NULL; }

static size_t write_state(mdrt *m, uint8_t *out)
{
    st_out o = { out, 0 };
    st_put(&o, "MDRS", 4);
    st_u32(&o, 1);
    size_t at = begin(&o, "CONS");
    st_u8(&o, (uint8_t)(m->cfg.pal ? 1 : 0)); st_u32(&o, m->frame); st_u64(&o, m->frames); st_u32(&o, m->beam);
    end(&o, at);
    at = begin(&o, "VDP "); o.n += mdrt_vdp_save(&m->vdp, here(&o)); end(&o, at);
    at = begin(&o, "PSG "); o.n += mdrt_psg_save(&m->psg, here(&o)); end(&o, at);
    at = begin(&o, "FM  "); o.n += mdrt_fm_save(m->fm, here(&o)); end(&o, at);
    at = begin(&o, "MIX "); o.n += mdrt_mix_save(&m->mix, here(&o)); end(&o, at);
    at = begin(&o, "SND ");
    st_u32(&o, (uint32_t)m->nev);
    for (size_t i = 0; i < m->nev; i++) { st_u64(&o, m->ev[i].at); st_u8(&o, m->ev[i].kind); st_u8(&o, m->ev[i].port); st_u8(&o, m->ev[i].value); }
    end(&o, at);
    at = begin(&o, "PADS"); o.n += mdrt_pads_save(&m->pads, here(&o)); end(&o, at);
    at = begin(&o, "SRAM"); o.n += mdrt_sram_save(&m->sram, here(&o)); end(&o, at);
    for (int i = 0; i < m->nparts; i++) {
        char tag[5];
        memcpy(tag, m->parts[i].tag, 4); tag[4] = 0;
        at = begin(&o, tag); o.n += m->parts[i].save(m->parts[i].user, here(&o)); end(&o, at);
    }
    return o.n;
}

size_t mdrt_get_state(mdrt *m, void *buf, size_t size)
{
    size_t need = write_state(m, NULL);
    if (buf && size >= need) write_state(m, buf);
    return need;
}

/* the sections of a state, in order, against the tags this console's states have */
#define SECTIONS (8 + MAX_PARTS)
static int sections(const mdrt *m, const uint8_t *b, size_t size, const uint8_t *data[], size_t len[])
{
    static const char fixed[8][5] = { "CONS", "VDP ", "PSG ", "FM  ", "MIX ", "SND ", "PADS", "SRAM" };
    st_in in = { b, 0, size, 0 };
    char magic[4];
    st_get(&in, magic, 4);
    if (memcmp(magic, "MDRS", 4) || st_g32(&in) != 1) return -1;
    int n = 8 + m->nparts;
    for (int i = 0; i < n; i++) {
        char tag[4];
        st_get(&in, tag, 4);
        uint32_t k = st_g32(&in);
        if (in.bad || memcmp(tag, i < 8 ? fixed[i] : m->parts[i - 8].tag, 4) || !st_have(&in, k)) return -1;
        data[i] = b + in.n;
        len[i] = k;
        in.n += k;
    }
    return in.n == size ? 0 : -1;
}

/* the sections into the console; nonzero when one is refused (the console is then part loaded) */
static int load_sections(mdrt *m, const uint8_t *data[], const size_t len[])
{
    st_in c = { data[0], 0, len[0], 0 };
    int pal = st_g8(&c);
    uint32_t frame = st_g32(&c);
    uint64_t frames = st_g64(&c);
    uint32_t beam = st_g32(&c);
    if (c.bad || c.n != len[0] || pal != (m->cfg.pal ? 1 : 0) || frame != m->frame) return -1;
    st_in s = { data[5], 0, len[5], 0 };
    uint32_t nev = st_g32(&s);
    if (s.bad || (size_t)nev * 11 != len[5] - 4) return -1;
    if (nev > m->cap) {
        event *e = realloc(m->ev, nev * sizeof *e);
        if (!e) return -1;
        m->ev = e;
        m->cap = nev;
    }
    if (mdrt_vdp_load(&m->vdp, data[1], len[1]) || mdrt_psg_load(&m->psg, data[2], len[2]) ||
        mdrt_fm_load(m->fm, data[3], len[3]) || mdrt_pads_load(&m->pads, data[6], len[6]) ||
        mdrt_sram_load(&m->sram, data[7], len[7]))
        return -1;
    /* the mixer is presentation only: a state from another audio rate starts it afresh */
    st_in x = { data[4], 0, len[4], 0 };
    uint32_t rate = st_g32(&x), clock = st_g32(&x);
    if (rate != m->mix.rate || clock != m->mix.clock || mdrt_mix_load(&m->mix, data[4], len[4])) {
        int32_t fg = m->mix.fm_gain, pg = m->mix.psg_gain;
        uint32_t ck = m->mix.clock;
        mdrt_mix_init(&m->mix, m->cfg.audio_rate);
        mdrt_mix_set_clock(&m->mix, ck);
        mdrt_mix_set_gains(&m->mix, fg, pg);
    }
    m->nev = nev;
    for (uint32_t i = 0; i < nev; i++) {
        m->ev[i].at = st_g64(&s);
        m->ev[i].kind = st_g8(&s); m->ev[i].port = st_g8(&s); m->ev[i].value = st_g8(&s);
    }
    m->next = 0;
    m->frames = frames;
    m->beam = beam;
    for (int i = 0; i < m->nparts; i++)
        if (m->parts[i].load(m->parts[i].user, data[8 + i], len[8 + i])) return -1;
    return 0;
}

int mdrt_set_state(mdrt *m, const void *buf, size_t size)
{
    const uint8_t *data[SECTIONS];
    size_t len[SECTIONS];
    if (!buf || sections(m, buf, size, data, len)) return -1;
    /* the console as it is, to put back if a section is refused part way */
    size_t n = write_state(m, NULL);
    uint8_t *was = malloc(n);
    if (!was) return -1;
    write_state(m, was);
    int bad = load_sections(m, data, len);
    if (bad) {
        const uint8_t *wd[SECTIONS];
        size_t wl[SECTIONS];
        if (!sections(m, was, n, wd, wl)) load_sections(m, wd, wl);
    }
    free(was);
    return bad ? -1 : 0;
}
