/* The VDP's registers, ports and DMA (genvdp.txt, Sega2.doc "VDP"; checked against the vectors,
 * docs/HARDWARE.md). */
#include <string.h>
#include "mdrt/vdp.h"
#include "../state.h"

void mdrt_vdp_init(mdrt_vdp *v, int pal)
{
    memset(v, 0, sizeof *v);
    v->pal = pal ? 1 : 0;
}

void mdrt_vdp_set_memory(mdrt_vdp *v, mdrt_read8_fn fn, void *user) { v->read8 = fn; v->read8_user = user; }
void mdrt_vdp_set_write_hook(mdrt_vdp *v, mdrt_vdp_write_fn fn, void *user) { v->write_hook = fn; v->write_user = user; }
void mdrt_vdp_set_hv_source(mdrt_vdp *v, mdrt_hv_fn fn, void *user) { v->hv = fn; v->hv_user = user; }
void mdrt_vdp_set_position(mdrt_vdp *v, uint16_t line, uint16_t hpos) { v->line = line; v->hpos = hpos; }
void mdrt_vdp_set_vblank(mdrt_vdp *v, int on) { v->vblank = on ? 1 : 0; }

int mdrt_vdp_lines(const mdrt_vdp *v) { return v->pal ? 313 : 262; }
uint32_t mdrt_vdp_line_start(const mdrt_vdp *v, int n) { return (uint32_t)(mdrt_vdp_lines(v) - mdrt_vdp_height(v) + n) * 3420u; }
int mdrt_vdp_width(const mdrt_vdp *v) { return (v->s.reg[12] & 0x81) ? 320 : 256; }   /* R12 RS0 / RS1 */
int mdrt_vdp_height(const mdrt_vdp *v) { return (v->pal && (v->s.reg[1] & 8)) ? 240 : 224; }   /* R1 M2: V30 */

/* the sprite attribute table's base and size: R5, 80 entries in H40 (bit 0 ignored), 64 in H32 */
static uint32_t sat_base(const mdrt_vdp *v)
{
    return (v->s.reg[12] & 0x81) ? (uint32_t)(v->s.reg[5] & 0x7E) << 9 : (uint32_t)(v->s.reg[5] & 0x7F) << 9;
}
static uint32_t sat_size(const mdrt_vdp *v) { return (v->s.reg[12] & 0x81) ? 0x280 : 0x200; }

/* a VRAM byte write; bytes 0-3 of each sprite entry are also latched in the sprite cache, which
 * the sprite renderer reads for Y, size and link (genvdp.txt "sprite attribute table") */
static void vram_byte(mdrt_vdp *v, uint16_t a, uint8_t b)
{
    v->s.vram[a] = b;
    uint32_t base = sat_base(v), off = (uint32_t)a - base;
    if (a >= base && off < sat_size(v) && (off & 7) < 4) v->sat[off] = b;
}

/* a word through the FIFO to VRAM, CRAM or VSRAM (CD3-CD0); the address steps by R15 */
static void bus_write(mdrt_vdp *v, uint16_t w)
{
    v->fifo[v->fifo_idx] = w;
    v->fifo_idx = (uint8_t)((v->fifo_idx + 1) & 3);
    switch (v->code & 0x0F) {
    case 0x01: {                                          /* VRAM: an odd address swaps the bytes */
        uint16_t a = v->addr & 0xFFFE;
        if (v->addr & 1) w = (uint16_t)(w >> 8 | w << 8);
        vram_byte(v, a, (uint8_t)(w >> 8));
        vram_byte(v, (uint16_t)(a + 1), (uint8_t)w);
        break;
    }
    case 0x03: {                                          /* CRAM: 64 words of 9 bits */
        int i = (v->addr >> 1) & 0x3F;
        unsigned changed = v->s.cram[i] ^ (w & 0x0EEE);
        v->s.cram[i] = w & 0x0EEE;
        if (v->write_hook) v->write_hook(v->write_user, 64 + i, changed);
        break;
    }
    case 0x05:                                            /* VSRAM: 40 words of 11 bits; past them ignored */
        if ((v->addr & 0x7E) < 0x50) v->s.vsram[(v->addr & 0x7E) >> 1] = w & 0x07FF;
        break;
    }
    v->addr = (uint16_t)(v->addr + v->s.reg[15]);
}

/* the DMA length (R19-R20, 0 meaning 65536) */
static uint32_t dma_length(const mdrt_vdp *v)
{
    uint32_t n = (uint32_t)(v->s.reg[20] << 8 | v->s.reg[19]);
    return n ? n : 0x10000;
}

/* after a DMA the source registers have advanced by the length and the length is 0 */
static void dma_end(mdrt_vdp *v)
{
    uint16_t end = (uint16_t)(v->s.reg[21] + (v->s.reg[22] << 8) + v->s.reg[19] + (v->s.reg[20] << 8));
    v->s.reg[21] = (uint8_t)end;
    v->s.reg[22] = (uint8_t)(end >> 8);
    v->s.reg[19] = v->s.reg[20] = 0;
}

/* 68k bus to VDP: words from (R23 bits 6-0) << 17 plus the word source in R21-R22, which counts
 * within its 128 KB bank */
static void dma_68k(mdrt_vdp *v)
{
    uint32_t n = dma_length(v);
    uint32_t bank = (uint32_t)(v->s.reg[23] & 0x7F) << 17;
    uint32_t src = (uint32_t)(v->s.reg[22] << 8 | v->s.reg[21]) << 1;
    do {
        uint16_t w = 0;
        if (v->read8) w = (uint16_t)(v->read8(v->read8_user, bank | src) << 8 | v->read8(v->read8_user, bank | (src + 1)));
        src = (src + 2) & 0x1FFFF;
        bus_write(v, w);
    } while (--n);
    dma_end(v);
}

/* VRAM copy (CD4 set): bytes from the source in R21-R22 to the address, each at the adjacent
 * (odd / even swapped) location */
static void dma_copy(mdrt_vdp *v)
{
    if (!(v->code & 0x10)) return;
    uint32_t n = dma_length(v);
    uint16_t src = (uint16_t)(v->s.reg[22] << 8 | v->s.reg[21]);
    do {
        vram_byte(v, v->addr ^ 1, v->s.vram[src ^ 1]);
        src++;
        v->addr = (uint16_t)(v->addr + v->s.reg[15]);
    } while (--n);
    dma_end(v);
}

/* VRAM fill: after the data port write that starts it, the high byte of that word goes to the
 * adjacent byte of each address, stepping by R15 */
static void dma_fill(mdrt_vdp *v, uint16_t w)
{
    uint32_t n = dma_length(v);
    if ((v->code & 0x0F) == 0x01) {
        do {
            vram_byte(v, v->addr ^ 1, (uint8_t)(w >> 8));
            v->addr = (uint16_t)(v->addr + v->s.reg[15]);
        } while (--n);
    }
    dma_end(v);
}

/* register writes: 100r rrrr vvvv vvvv; registers 24-31 do not exist */
static void reg_write(mdrt_vdp *v, uint16_t w)
{
    int r = (w >> 8) & 0x1F;
    if (r >= 24) return;
    unsigned changed = v->s.reg[r] ^ (uint8_t)w;
    v->s.reg[r] = (uint8_t)w;
    if (v->write_hook) v->write_hook(v->write_user, r, changed);
}

void mdrt_vdp_ctrl16(mdrt_vdp *v, uint16_t w)
{
    if (!v->pending) {
        /* the first command word (CD1-CD0, A13-A0), or a register write, which also loads them */
        v->addr = (uint16_t)(v->addr_hi | (w & 0x3FFF));
        v->code = (uint8_t)((v->code & 0x3C) | (w >> 14));
        if ((w & 0xC000) == 0x8000) reg_write(v, w);
        else v->pending = (v->s.reg[1] & 4) ? 1 : 0;      /* mode 5 */
        return;
    }
    /* the second command word: CD5-CD2, A15-A14 */
    v->pending = 0;
    v->addr_hi = (uint16_t)((w & 3) << 14);
    v->addr = (uint16_t)(v->addr_hi | (v->addr & 0x3FFF));
    v->code = (uint8_t)((v->code & 0x03) | ((w >> 2) & 0x3C));
    if ((v->code & 0x20) && (v->s.reg[1] & 0x10)) {       /* CD5 with DMA enabled (R1 M1) */
        switch (v->s.reg[23] >> 6) {
        case 2: v->fill = 1; break;
        case 3: dma_copy(v); break;
        default: dma_68k(v); break;
        }
    }
}

void mdrt_vdp_ctrl32(mdrt_vdp *v, uint32_t l) { mdrt_vdp_ctrl16(v, (uint16_t)(l >> 16)); mdrt_vdp_ctrl16(v, (uint16_t)l); }

void mdrt_vdp_data16(mdrt_vdp *v, uint16_t w)
{
    v->pending = 0;
    bus_write(v, w);
    if (v->fill) {
        v->fill = 0;
        dma_fill(v, w);
    }
}

void mdrt_vdp_data32(mdrt_vdp *v, uint32_t l) { mdrt_vdp_data16(v, (uint16_t)(l >> 16)); mdrt_vdp_data16(v, (uint16_t)l); }

/* data port reads (CD0 clear): VRAM words, VSRAM (11 bits) and CRAM (9 bits) with the bits they do
 * not drive taken from the next FIFO entry */
uint16_t mdrt_vdp_read16(mdrt_vdp *v)
{
    uint16_t w = 0;
    v->pending = 0;
    switch (v->code & 0x1F) {
    case 0x00: {
        uint16_t a = v->addr & 0xFFFE;
        w = (uint16_t)(v->s.vram[a] << 8 | v->s.vram[a + 1]);
        break;
    }
    case 0x04: {
        int i = v->addr & 0x7E;
        if (i >= 0x50) i = 0;
        w = (uint16_t)((v->s.vsram[i >> 1] & 0x07FF) | (v->fifo[v->fifo_idx] & ~0x07FF));
        break;
    }
    case 0x08:
        w = (uint16_t)(v->s.cram[(v->addr >> 1) & 0x3F] | (v->fifo[v->fifo_idx] & ~0x0EEE));
        break;
    }
    v->addr = (uint16_t)(v->addr + v->s.reg[15]);
    return w;
}

/* the status word: FIFO empty (9), vint pending (7), vblank (3, also while the display is off),
 * PAL (0); DMA busy (1) never, the FIFO never full. Reading it ends a half-written command. */
uint16_t mdrt_vdp_status(mdrt_vdp *v)
{
    v->pending = 0;
    uint16_t s = 0x3400 | 0x0200 | v->pal;
    if (v->vblank || !(v->s.reg[1] & 0x40)) s |= 0x0008;
    if (v->vint_pending) s |= 0x0080;
    return s;
}

/* the V counter at line l: NTSC V28 counts 0-$EA then $E5-$FF; PAL V28 0-$102 then $1CA-$1FF,
 * V30 0-$10A then $1D2-$1FF (8 bits shown) */
static uint8_t v_counter(const mdrt_vdp *v, unsigned l)
{
    if (!v->pal) return (uint8_t)(l <= 0xEA ? l : l - 6);
    if (mdrt_vdp_height(v) == 240) return (uint8_t)(l <= 0x10A ? l : l + 0x1D2 - 0x10B);
    return (uint8_t)(l <= 0x102 ? l : l + 0x1CA - 0x103);
}

/* the H counter at pixel clock hpos (it counts every second one): H32 counts 0-$93 then $E9-$FF,
 * H40 0-$B6 then $E4-$FF */
static uint8_t h_counter(const mdrt_vdp *v, unsigned hpos)
{
    unsigned h = hpos >> 1;
    if (mdrt_vdp_width(v) == 320) return (uint8_t)(h <= 0xB6 ? h : h - 0xB7 + 0xE4);
    return (uint8_t)(h <= 0x93 ? h : h - 0x94 + 0xE9);
}

uint16_t mdrt_vdp_hv_at(const mdrt_vdp *v, unsigned line, unsigned hpos)
{
    return (uint16_t)(v_counter(v, line) << 8 | h_counter(v, hpos));
}

uint16_t mdrt_vdp_hv16(mdrt_vdp *v)
{
    if (v->hv) return v->hv(v->hv_user);
    return mdrt_vdp_hv_at(v, v->line, v->hpos);
}

uint8_t mdrt_vdp_hv8(mdrt_vdp *v, int odd)
{
    uint16_t hv = mdrt_vdp_hv16(v);
    return (uint8_t)(odd ? hv : hv >> 8);
}

uint8_t mdrt_vdp_reg(const mdrt_vdp *v, int n) { return n >= 0 && n < 24 ? v->s.reg[n] : 0; }

void mdrt_vdp_get_state(const mdrt_vdp *v, mdrt_vdp_state *out) { *out = v->s; }

void mdrt_vdp_set_state(mdrt_vdp *v, const mdrt_vdp_state *in)
{
    v->s = *in;
    v->pending = 0;
    v->fill = 0;
    v->addr = 0;
    v->addr_hi = 0;
    v->code = 0;
    uint32_t base = sat_base(v);
    for (uint32_t i = 0; i < sizeof v->sat; i++) v->sat[i] = v->s.vram[(base + i) & 0xFFFF];
}

static void save(const mdrt_vdp *v, st_out *o)
{
    st_put(o, v->s.reg, 24);
    st_put(o, v->s.vram, sizeof v->s.vram);
    st_u16s(o, v->s.cram, 64);
    st_u16s(o, v->s.vsram, 40);
    st_put(o, v->sat, sizeof v->sat);
    st_u16(o, v->addr); st_u16(o, v->addr_hi);
    st_u8(o, v->code); st_u8(o, v->pending); st_u8(o, v->fill); st_u8(o, v->fifo_idx);
    st_u16s(o, v->fifo, 4);
    st_u8(o, v->pal); st_u8(o, v->vblank); st_u8(o, v->vint_pending);
    st_u16(o, v->line); st_u16(o, v->hpos);
}

size_t mdrt_vdp_save(const mdrt_vdp *v, uint8_t *out)
{
    st_out o = { out, 0 };
    save(v, &o);
    return o.n;
}

int mdrt_vdp_load(mdrt_vdp *v, const uint8_t *in, size_t size)
{
    if (size != mdrt_vdp_save(v, NULL)) return -1;
    st_in i = { in, 0, size, 0 };
    st_get(&i, v->s.reg, 24);
    st_get(&i, v->s.vram, sizeof v->s.vram);
    st_g16s(&i, v->s.cram, 64);
    st_g16s(&i, v->s.vsram, 40);
    st_get(&i, v->sat, sizeof v->sat);
    v->addr = st_g16(&i); v->addr_hi = st_g16(&i);
    v->code = st_g8(&i); v->pending = st_g8(&i); v->fill = st_g8(&i); v->fifo_idx = st_g8(&i);
    st_g16s(&i, v->fifo, 4);
    v->pal = st_g8(&i); v->vblank = st_g8(&i); v->vint_pending = st_g8(&i);
    v->line = st_g16(&i); v->hpos = st_g16(&i);
    return 0;
}
