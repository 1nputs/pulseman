/* The composition (mdrt/mdrt.h): the modules, the sound timeline of a frame, the handshake. */
#include <stdlib.h>
#include <string.h>
#include "mdrt/mdrt.h"
#include "../state.h"

enum { EV_YM, EV_YM_RESET, EV_PSG };
typedef struct event { uint64_t at; uint8_t kind, port, value; } event;

#define CHUNK (16 * MDRT_FM_SAMPLE_MCLKS)                 /* the sound is made a chunk at a time */

#define MAX_MAPS 16
typedef struct bus_map { uint32_t base, size; mdrt_bus_read_fn read; mdrt_bus_write_fn write; void *user; } bus_map;

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
    uint32_t nudge;             /* the default HV counter's nudge, pixel clocks since the handshake */
    uint32_t now;               /* the client's position in the frame, master clocks */
    mdrt_vdp_beam lines;        /* the picture, drawn a line at a time as now passes them */
    int reloaded;               /* the H-interrupt counter was reloaded this frame (the last blank line) */
    int hcount;                 /* the H-interrupt counter */
    int in_hint;
    mdrt_hint_fn hint;
    void *hint_user;
    unsigned requests;          /* the player's, from the last handshake */
    part parts[MAX_PARTS];      /* the client's parts of a state */
    int nparts;
    bus_map maps[MAX_MAPS];     /* the client's regions of the 68k's bus */
    int nmaps;
    struct { mdrt_sink_fn sink; void *user; } rec[3];   /* the traces being recorded */
    int vdp_whole;              /* the VDP trace has a whole frame: FRAMEs from here */
    uint64_t snd_base;          /* the frame the sound trace began at */
    uint8_t *dma;               /* the DMA source bytes read since the last VDP access, for the trace */
    size_t ndma, dma_cap;
};

static uint8_t no_memory(void *user, uint32_t addr) { (void)user; (void)addr; return 0; }
static void dma_note(mdrt *m, uint8_t b);
static uint8_t dma_read(void *user, uint32_t addr)
{
    mdrt *m = user;
    uint8_t b = mdrt_bus_read8(m, addr);
    if (m->rec[MDRT_TRACE_VDP].sink) dma_note(m, b);
    return b;
}

/* the default HV counter: the beam at the client's position in the frame, nudged on at each read
 * as time passes in the game's code even when the client moves no clock (the game reads it as a
 * random source; lockstep replays recorded values through mdrt_set_hv_source), shown as the
 * hardware's counters show it */
static uint16_t beam_hv(void *user)
{
    mdrt *m = user;
    int width = mdrt_vdp_width(&m->vdp) == 320 ? 420 : 342;
    uint32_t lines = m->frame / MDRT_LINE_MCLKS;
    m->nudge += 97;
    uint32_t px = m->nudge + (uint32_t)((uint64_t)m->now * (uint32_t)width / MDRT_LINE_MCLKS);
    uint32_t line = ((uint32_t)mdrt_vdp_height(&m->vdp) + px / (uint32_t)width) % lines, h = px % (uint32_t)width;
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
    if (!m->cfg.read68k) m->cfg.read68k = no_memory;
    mdrt_vdp_set_memory(&m->vdp, dma_read, m);            /* (DMA from the 68k bus: through the map) */
    mdrt_vdp_set_hv_source(&m->vdp, beam_hv, m);
    mdrt_psg_init(&m->psg);
    m->fm = mdrt_fm_new_core(c->fm_core, c->fm_model);
    mdrt_mix_init(&m->mix, m->cfg.audio_rate);
    mdrt_mix_set_clock(&m->mix, master_clock(m));
    mdrt_mix_set_gains(&m->mix, c->fm_gain ? c->fm_gain : MDRT_MIX_FM_GAIN, c->psg_gain ? c->psg_gain : MDRT_MIX_PSG_GAIN);
    mdrt_pads_init(&m->pads, !c->domestic, c->pal);
    mdrt_sram_init(&m->sram, c->sram_size);
    size_t samples = m->frame / MDRT_FM_SAMPLE_MCLKS + 4;
    m->picture = calloc(320 * 240, sizeof *m->picture);
    m->fm_buf = malloc(2 * samples * sizeof *m->fm_buf);
    m->psg_buf = malloc(samples * sizeof *m->psg_buf);
    m->audio = malloc(2 * ((size_t)m->cfg.audio_rate * m->frame / master_clock(m) + 8) * sizeof *m->audio);
    m->cap = 4096;
    m->ev = malloc(m->cap * sizeof *m->ev);
    if (!m->fm || !m->picture || !m->fm_buf || !m->psg_buf || !m->audio || !m->ev) { mdrt_free(m); return NULL; }
    mdrt_vdp_beam_start(&m->lines, &m->vdp, m->picture, 320);
    return m;
}

void mdrt_free(mdrt *m)
{
    if (!m) return;
    mdrt_fm_free(m->fm);
    free(m->picture); free(m->fm_buf); free(m->psg_buf); free(m->audio); free(m->ev); free(m->dma);
    free(m);
}

mdrt_vdp *mdrt_get_vdp(mdrt *m) { return &m->vdp; }
mdrt_pads *mdrt_get_pads(mdrt *m) { return &m->pads; }
mdrt_sram *mdrt_get_sram(mdrt *m) { return &m->sram; }
mdrt_fm *mdrt_get_fm(mdrt *m) { return m->fm; }

/* a write to the new chip, then the time the hardware's busy flag asks for (the samples dropped) */
static void fm_copy_write(mdrt_fm *f, int port, uint8_t value, uint32_t mclk)
{
    int16_t spare[2 * 4];
    mdrt_fm_write(f, port, value);
    mdrt_fm_run(f, mclk, spare, 4);
}

int mdrt_set_fm(mdrt *m, const char *core, int model)
{
    mdrt_fm *f = mdrt_fm_new_core(core, model);
    if (!f) return -1;
    /* (the chip latches a frequency's high byte until its low one is written: one latch for the
     * channels, one for channel 3's special mode, so each pair goes together) */
    static const uint8_t order[] = { 0xA4, 0xA0, 0xA5, 0xA1, 0xA6, 0xA2, 0xAC, 0xA8, 0xAD, 0xA9, 0xAE, 0xAA };
    for (int half = 0; half < 2; half++) {               /* every register but the key and DAC data ones */
        for (int r = 0x21; r <= 0xB6; r++) {
            if (r == 0x28 || r == 0x2A || (half && r < 0x30) || (r >= 0xA0 && r < 0xB0)) continue;
            fm_copy_write(f, half * 2, (uint8_t)r, 17 * MDRT_FM_CYCLE_MCLKS);
            fm_copy_write(f, half * 2 + 1, mdrt_fm_reg(m->fm, half, (uint8_t)r), 32 * MDRT_FM_CYCLE_MCLKS);
        }
        for (size_t i = 0; i < sizeof order; i++) {      /* each frequency: its high byte, then its low */
            fm_copy_write(f, half * 2, order[i], 17 * MDRT_FM_CYCLE_MCLKS);
            fm_copy_write(f, half * 2 + 1, mdrt_fm_reg(m->fm, half, order[i]), 32 * MDRT_FM_CYCLE_MCLKS);
        }
    }
    mdrt_fm_free(m->fm);
    m->fm = f;
    m->cfg.fm_core = mdrt_fm_core_of(f);
    m->cfg.fm_model = model;
    return 0;
}
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
static void snd_event(mdrt *m, const event *e, uint32_t t);
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
            if (m->rec[MDRT_TRACE_SOUND].sink) snd_event(m, e, t);
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

/* ---- the 68k's bus by address ---------------------------------------------------------------- */

int mdrt_bus_map(mdrt *m, uint32_t base, uint32_t size, mdrt_bus_read_fn read, mdrt_bus_write_fn write, void *user)
{
    if (m->nmaps == MAX_MAPS) return -1;
    m->maps[m->nmaps++] = (bus_map){ base & 0xFFFFFF, size, read, write, user };
    return 0;
}

static const bus_map *mapped(const mdrt *m, uint32_t a)
{
    for (int i = m->nmaps - 1; i >= 0; i--)
        if (a - m->maps[i].base < m->maps[i].size) return &m->maps[i];
    return NULL;
}

enum { AT_MEMORY, AT_VDP, AT_IO, AT_SRAM };
static int area(const mdrt *m, uint32_t a)
{
    if (a >= 0xC00000 && a < 0xC00020) return AT_VDP;
    if (a >= 0xA10000 && a < 0xA10020) return AT_IO;
    if (m->sram.size && a >= 0x200000 && a < 0x400000) return AT_SRAM;
    return AT_MEMORY;
}

/* a word of the VDP's ports: data ($00-$03), control ($04-$07), HV counter ($08-$0F); the PSG is
 * write only */
static uint16_t vdp_read16(mdrt *m, uint32_t a)
{
    a &= 0x1E;
    if (a < 4) return mdrt_vdp_read16(&m->vdp);
    if (a < 8) return mdrt_vdp_status(&m->vdp);
    if (a < 0x10) return mdrt_vdp_hv16(&m->vdp);
    return 0xFFFF;
}
static void vdp_write16(mdrt *m, uint32_t a, uint16_t v)
{
    a &= 0x1E;
    if (a < 4) mdrt_vdp_data16(&m->vdp, v);
    else if (a < 8) mdrt_vdp_ctrl16(&m->vdp, v);
    else if (a >= 0x10 && a < 0x18) mdrt_psg_port(m, m->now, (uint8_t)v);
}

uint8_t mdrt_bus_read8(mdrt *m, uint32_t addr)
{
    uint32_t a = addr & 0xFFFFFF;
    const bus_map *b = mapped(m, a);
    if (b) return b->read ? b->read(b->user, a) : 0xFF;
    switch (area(m, a)) {
    case AT_VDP:
        if ((a & 0x1E) == 8 || (a & 0x1E) == 0xA || (a & 0x1E) == 0xC || (a & 0x1E) == 0xE) return mdrt_vdp_hv8(&m->vdp, (int)(a & 1));
        return (uint8_t)(vdp_read16(m, a) >> ((a & 1) ? 0 : 8));
    case AT_IO: return mdrt_pad_read(&m->pads, a);
    case AT_SRAM: return mdrt_sram_read8(&m->sram, a - 0x200000);
    default: return m->cfg.read68k(m->cfg.read68k_user, a);
    }
}

uint16_t mdrt_bus_read16(mdrt *m, uint32_t addr)
{
    uint32_t a = addr & 0xFFFFFE;
    if (!mapped(m, a) && area(m, a) == AT_VDP) return vdp_read16(m, a);
    if (!mapped(m, a) && area(m, a) == AT_IO) { uint8_t v = mdrt_pad_read(&m->pads, a | 1); return (uint16_t)(v << 8 | v); }
    return (uint16_t)(mdrt_bus_read8(m, a) << 8 | mdrt_bus_read8(m, a + 1));
}

uint32_t mdrt_bus_read32(mdrt *m, uint32_t addr) { return (uint32_t)mdrt_bus_read16(m, addr) << 16 | mdrt_bus_read16(m, addr + 2); }

void mdrt_bus_write8(mdrt *m, uint32_t addr, uint8_t value)
{
    uint32_t a = addr & 0xFFFFFF;
    const bus_map *b = mapped(m, a);
    if (b) { if (b->write) b->write(b->user, a, value); return; }
    switch (area(m, a)) {
    case AT_VDP:                                          /* a byte to the VDP's ports: on both halves */
        if ((a & 0x18) == 0x10) { if (a & 1) mdrt_psg_port(m, m->now, value); }
        else vdp_write16(m, a, (uint16_t)(value << 8 | value));
        break;
    case AT_IO: mdrt_pad_write(&m->pads, a, value); break;
    case AT_SRAM: mdrt_sram_write8(&m->sram, a - 0x200000, value); break;
    default: break;
    }
}

void mdrt_bus_write16(mdrt *m, uint32_t addr, uint16_t value)
{
    uint32_t a = addr & 0xFFFFFE;
    if (!mapped(m, a) && area(m, a) == AT_VDP) { vdp_write16(m, a, value); return; }
    if (!mapped(m, a) && area(m, a) != AT_MEMORY) { mdrt_bus_write8(m, a | 1, (uint8_t)value); return; }
    mdrt_bus_write8(m, a, (uint8_t)(value >> 8));
    mdrt_bus_write8(m, a + 1, (uint8_t)value);
}

void mdrt_bus_write32(mdrt *m, uint32_t addr, uint32_t value)
{
    uint32_t a = addr & 0xFFFFFE;
    if (!mapped(m, a) && area(m, a) == AT_VDP && (a & 0x1E) < 8) {   /* a move.l to the data or control port */
        if ((a & 0x1E) < 4) mdrt_vdp_data32(&m->vdp, value);
        else mdrt_vdp_ctrl32(&m->vdp, value);
        return;
    }
    mdrt_bus_write16(m, a, (uint16_t)(value >> 16));
    mdrt_bus_write16(m, a + 2, (uint16_t)value);
}

/* ---- the clock: lines drawn and H-interrupts as it passes them ------------------------------- */

static void hint_now(mdrt *m)
{
    if (!m->hint || m->in_hint || !(mdrt_vdp_reg(&m->vdp, 0) & 0x10)) return;
    m->in_hint = 1;
    m->hint(m->hint_user);
    m->in_hint = 0;
}

/* on to `to`: the counter's reload on the last line of the vertical blank, then each display line
 * drawn at its start and the counter counted (an H-interrupt when it runs out, with the clock at
 * that line's start) */
static void advance(mdrt *m, uint32_t to)
{
    if (to > m->frame) to = m->frame;
    int height = mdrt_vdp_height(&m->vdp);
    for (;;) {
        uint32_t at;
        if (!m->reloaded) at = mdrt_vdp_line_start(&m->vdp, -1);
        else if (m->lines.drawn < height) at = mdrt_vdp_line_start(&m->vdp, m->lines.drawn);
        else break;
        if (at > to) break;
        if (at > m->now) m->now = at;
        m->lines.now = m->now;
        if (!m->reloaded) {
            m->reloaded = 1;
            m->hcount = mdrt_vdp_reg(&m->vdp, 10);
            continue;
        }
        mdrt_vdp_beam_line(&m->lines);
        if (m->hcount == 0) { m->hcount = mdrt_vdp_reg(&m->vdp, 10); hint_now(m); }
        else m->hcount--;
    }
    if (to > m->now) m->now = to;
    m->lines.now = m->now;
}

uint32_t mdrt_now(const mdrt *m) { return m->now; }
void mdrt_run_until(mdrt *m, uint32_t mclk) { advance(m, mclk); }
int mdrt_charge(mdrt *m, uint32_t cycles)
{
    uint64_t to = (uint64_t)m->now + (uint64_t)cycles * 7;
    advance(m, to > m->frame ? m->frame : (uint32_t)to);
    return m->now >= m->frame;
}
int mdrt_wait_line(mdrt *m, int line)
{
    int lines = mdrt_vdp_lines(&m->vdp), height = mdrt_vdp_height(&m->vdp);
    if (line < 0 || line >= lines) return 1;
    uint32_t at = line < height ? mdrt_vdp_line_start(&m->vdp, line) : (uint32_t)(line - height) * MDRT_LINE_MCLKS;
    if (at < m->now) return 1;
    advance(m, at);
    return 0;
}
void mdrt_set_hint(mdrt *m, mdrt_hint_fn fn, void *user) { m->hint = fn; m->hint_user = user; }

static void rec_frame(mdrt *m, int made);
static void rec_buttons(mdrt *m);

int mdrt_wait_vblank(mdrt *m)
{
    advance(m, m->frame);                                 /* the rest of the picture */
    int made = make_sound(m);
    rec_frame(m, made);
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
    rec_buttons(m);
    m->requests = p->requests ? p->requests(p->user) : 0;
    m->frames++;
    m->nudge = 0;
    m->now = 0;
    mdrt_vdp_beam_start(&m->lines, &m->vdp, m->picture, 320);
    m->reloaded = 0;
    mdrt_vdp_set_position(&m->vdp, 224, 0);
    mdrt_vdp_set_vblank(&m->vdp, 1);
    if (m->rec[MDRT_TRACE_VDP].sink) m->vdp_whole = 1;
    if (m->hcount == 0) hint_now(m);                      /* (the oracle checks the counter as the frame begins too) */
    return stop;
}

void mdrt_vblank_end(mdrt *m) { mdrt_vdp_set_vblank(&m->vdp, 0); }
unsigned mdrt_requests(const mdrt *m) { return m->requests; }

/* ---- states -------------------------------------------------------------------------------
 * "MDRS", version u32 = 2, then sections: tag[4], size u32, bytes. In this order: CONS (PAL u8,
 * the frame's length u32, frames u64, the HV nudge u32; the clock: the position u32, the lines
 * drawn u32, the counter reloaded u8, the H-interrupt counter u32, the last line's colours (320
 * u8), then the lines drawn so far, 320 colours u16 each), VDP, PSG, FM (the chip's wrapper, its core's
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
    st_u32(&o, 2);
    size_t at = begin(&o, "CONS");
    st_u8(&o, (uint8_t)(m->cfg.pal ? 1 : 0)); st_u32(&o, m->frame); st_u64(&o, m->frames); st_u32(&o, m->nudge);
    st_u32(&o, m->now); st_u32(&o, (uint32_t)m->lines.drawn); st_u8(&o, (uint8_t)m->reloaded); st_u32(&o, (uint32_t)m->hcount);
    st_put(&o, m->lines.idx, 320);
    st_u16s(&o, m->picture, (size_t)m->lines.drawn * 320);
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
    if (memcmp(magic, "MDRS", 4) || st_g32(&in) != 2) return -1;
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
    uint32_t now = st_g32(&c), drawn = st_g32(&c);
    int reloaded = st_g8(&c);
    uint32_t hcount = st_g32(&c);
    uint8_t idx[320];
    st_get(&c, idx, 320);
    if (c.bad || drawn > 240 || c.n + (size_t)drawn * 640 != len[0] || pal != (m->cfg.pal ? 1 : 0) || frame != m->frame || now > frame) return -1;
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
    m->nudge = beam;
    m->now = now;
    m->lines.now = now;
    m->lines.drawn = (int)drawn;
    memcpy(m->lines.idx, idx, 320);
    m->reloaded = reloaded;
    m->hcount = (int)hcount;
    st_g16s(&c, m->picture, (size_t)drawn * 320);
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

/* ---- the recorder ------------------------------------------------------------------------------ */

static void put(mdrt *m, int trace, const void *b, size_t n) { m->rec[trace].sink(m->rec[trace].user, b, n); }
static void put8(mdrt *m, int trace, uint8_t v) { put(m, trace, &v, 1); }
static void put16(mdrt *m, int trace, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(m, trace, b, 2); }
static void put32(mdrt *m, int trace, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; put(m, trace, b, 4); }

static void dma_note(mdrt *m, uint8_t b)
{
    if (m->ndma == m->dma_cap) {
        uint8_t *d = realloc(m->dma, m->dma_cap ? 2 * m->dma_cap : 4096);
        if (!d) return;
        m->dma = d;
        m->dma_cap = m->dma_cap ? 2 * m->dma_cap : 4096;
    }
    m->dma[m->ndma++] = b;
}

/* the VDP's state as the trace has it: registers, VRAM, CRAM and VSRAM big-endian */
static void vdp_state(mdrt *m)
{
    mdrt_vdp_state s;
    mdrt_vdp_get_state(&m->vdp, &s);
    put(m, MDRT_TRACE_VDP, s.reg, 24);
    put(m, MDRT_TRACE_VDP, s.vram, sizeof s.vram);
    uint8_t w[128];
    for (int i = 0; i < 64; i++) { w[2 * i] = (uint8_t)(s.cram[i] >> 8); w[2 * i + 1] = (uint8_t)s.cram[i]; }
    put(m, MDRT_TRACE_VDP, w, 128);
    for (int i = 0; i < 40; i++) { w[2 * i] = (uint8_t)(s.vsram[i] >> 8); w[2 * i + 1] = (uint8_t)s.vsram[i]; }
    put(m, MDRT_TRACE_VDP, w, 80);
}

/* a port access: the DMA source it read first (the trace puts DMASRC before the access that starts
 * the DMA), then the access at its time */
static void vdp_seen(void *user, int access, uint32_t value)
{
    mdrt *m = user;
    if (m->ndma) {
        put8(m, MDRT_TRACE_VDP, 9);
        put32(m, MDRT_TRACE_VDP, (uint32_t)m->ndma);
        put(m, MDRT_TRACE_VDP, m->dma, m->ndma);
        m->ndma = 0;
    }
    put8(m, MDRT_TRACE_VDP, (uint8_t)access);
    put32(m, MDRT_TRACE_VDP, m->now);
    if (access == MDRT_VDP_CTRL32 || access == MDRT_VDP_DATA32) put32(m, MDRT_TRACE_VDP, value);
    else if (access == MDRT_VDP_HV8) { put8(m, MDRT_TRACE_VDP, (uint8_t)(value >> 8)); put8(m, MDRT_TRACE_VDP, (uint8_t)value); }
    else put16(m, MDRT_TRACE_VDP, (uint16_t)value);
}

static void pads_seen(void *user, int write, uint32_t addr, uint8_t value)
{
    mdrt *m = user;
    put8(m, MDRT_TRACE_PADS, write ? 2 : 3);
    put32(m, MDRT_TRACE_PADS, addr);
    put8(m, MDRT_TRACE_PADS, 1);
    put32(m, MDRT_TRACE_PADS, value);
}

static uint32_t snd_time(const mdrt *m, uint64_t at) { return (uint32_t)((m->frames - m->snd_base) * m->frame + at); }

static void snd_event(mdrt *m, const event *e, uint32_t t)
{
    if (e->kind == EV_YM) {
        put8(m, MDRT_TRACE_SOUND, 1); put32(m, MDRT_TRACE_SOUND, snd_time(m, t));
        put8(m, MDRT_TRACE_SOUND, e->port); put8(m, MDRT_TRACE_SOUND, e->value);
    } else if (e->kind == EV_PSG) {
        put8(m, MDRT_TRACE_SOUND, 2); put32(m, MDRT_TRACE_SOUND, snd_time(m, t));
        put8(m, MDRT_TRACE_SOUND, e->value);
    } else {                                              /* the reset: as the Z80's reset line, held and let go */
        put8(m, MDRT_TRACE_SOUND, 5); put32(m, MDRT_TRACE_SOUND, snd_time(m, t)); put16(m, MDRT_TRACE_SOUND, 0);
        put8(m, MDRT_TRACE_SOUND, 5); put32(m, MDRT_TRACE_SOUND, snd_time(m, t)); put16(m, MDRT_TRACE_SOUND, 0x100);
    }
}

/* the handshake: the frame's picture and state, its audio */
static void rec_frame(mdrt *m, int made)
{
    if (m->rec[MDRT_TRACE_VDP].sink && m->vdp_whole) {
        int w = mdrt_vdp_width(&m->vdp), h = mdrt_vdp_height(&m->vdp);
        put8(m, MDRT_TRACE_VDP, 10);
        vdp_state(m);
        put16(m, MDRT_TRACE_VDP, (uint16_t)w);
        put16(m, MDRT_TRACE_VDP, (uint16_t)h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) put16(m, MDRT_TRACE_VDP, m->picture[y * 320 + x]);
    }
    if (m->rec[MDRT_TRACE_SOUND].sink) {
        put8(m, MDRT_TRACE_SOUND, 6);
        put32(m, MDRT_TRACE_SOUND, snd_time(m, m->frame));
        put32(m, MDRT_TRACE_SOUND, (uint32_t)made);
        for (int i = 0; i < 2 * made; i++) put16(m, MDRT_TRACE_SOUND, (uint16_t)m->audio[i]);
    }
}

static void rec_buttons(mdrt *m)
{
    if (!m->rec[MDRT_TRACE_PADS].sink) return;
    put8(m, MDRT_TRACE_PADS, 1);
    put16(m, MDRT_TRACE_PADS, m->pads.buttons[0]);
    put16(m, MDRT_TRACE_PADS, m->pads.buttons[1]);
}

void mdrt_record(mdrt *m, int trace, mdrt_sink_fn sink, void *user)
{
    if (trace < 0 || trace > MDRT_TRACE_SOUND) return;
    if (m->rec[trace].sink) {                             /* the one being recorded ends */
        static const uint8_t end[3] = { 11, 4, 7 };
        put8(m, trace, end[trace]);
        m->rec[trace].sink = NULL;
        if (trace == MDRT_TRACE_VDP) mdrt_vdp_set_observer(&m->vdp, NULL, NULL);
        if (trace == MDRT_TRACE_PADS) mdrt_pads_set_observer(&m->pads, NULL, NULL);
    }
    if (!sink) return;
    m->rec[trace].sink = sink;
    m->rec[trace].user = user;
    if (trace == MDRT_TRACE_VDP) {
        put(m, trace, "MDVT", 4);
        put32(m, trace, 2);
        uint8_t region[4] = { (uint8_t)(m->cfg.pal ? 1 : 0), 0, 0, 0 };
        put(m, trace, region, 4);
        vdp_state(m);
        m->ndma = 0;
        m->vdp_whole = m->now == 0 && m->lines.drawn == 0;   /* (at a frame's start: this frame is whole) */
        mdrt_vdp_set_observer(&m->vdp, vdp_seen, m);
    } else if (trace == MDRT_TRACE_PADS) {
        put(m, trace, "MDPT", 4);
        put32(m, trace, 1);
        put(m, trace, m->pads.reg, 16);
        mdrt_pads_set_observer(&m->pads, pads_seen, m);
    } else {
        put(m, trace, "MDST", 4);
        put32(m, trace, 3);
        put32(m, trace, m->cfg.audio_rate);
        put16(m, trace, 100);                             /* (both chips in the audio) */
        put16(m, trace, 150);
        put32(m, trace, (m->frames == 0 ? 1u : 0u) | (strcmp(mdrt_fm_core_of(m->fm), "nuked") ? 0u : 2u) | 4u);
        static const uint8_t none[0x2000 + 12];          /* (no Z80: its RAM and registers as zeros) */
        put(m, trace, none, sizeof none);
        m->snd_base = m->frames;
    }
}
