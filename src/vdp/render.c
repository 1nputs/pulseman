/* The picture, a line at a time, from the VDP state as it stands when the line is drawn: the
 * composition draws each line as its clock passes the line's start (mdrt_vdp_line_start), so
 * writes made during the display change the lines after them; a client that writes only in the
 * vertical blank gets the whole frame drawn at the handshake. Layers from back to front: the
 * backdrop (R7), then per pixel the highest of B low, A low, sprites low, B high, A high, sprites
 * high that is not transparent (genvdp.txt "priority"). Plane A is replaced by the window inside
 * its area. No shadow / highlight or interlace yet. */
#include <string.h>
#include "mdrt/vdp.h"

#define MDRT_LINE_MCLKS_VDP 3420                         /* master clocks a line */

static uint16_t vword(const mdrt_vdp *v, uint32_t a)
{
    a &= 0xFFFE;
    return (uint16_t)(v->s.vram[a] << 8 | v->s.vram[a + 1]);
}

/* the colour index (0-15) of pixel x, y of the tile named by name table entry e (4 bits per
 * pixel, the left pixel in the high nibble; bits 11 / 12 flip) */
static int tile_pixel(const mdrt_vdp *v, uint16_t e, int x, int y)
{
    if (e & 0x0800) x = 7 - x;
    if (e & 0x1000) y = 7 - y;
    uint8_t b = v->s.vram[(uint32_t)((e & 0x7FF) << 5) + (uint32_t)(y << 2) + (uint32_t)(x >> 1)];
    return (x & 1) ? (b & 0xF) : (b >> 4);
}

/* a layer's line: colour (palette << 4 | index, 0 = transparent) and priority per pixel */
struct layer { uint8_t col[320], pri[320]; };

/* bug_x: plane A's left edge next to a window on the left (0 when none). The partly shown plane A
 * column there shows the tiles of the column after it (the "window bug": the vdptest vector,
 * docs/HARDWARE.md). */
static void scroll_plane(const mdrt_vdp *v, int line, uint32_t table, int hscroll, int plane, int width, int bug_x, struct layer *out)
{
    static const int cells[4] = { 32, 64, 32, 128 };      /* R16: 00 32, 01 64, 11 128 */
    int w = cells[v->s.reg[16] & 3], h = cells[(v->s.reg[16] >> 4) & 3];
    /* R11 bit 2: vertical scroll per 2-cell column. The columns are the plane's, moved by its fine
     * horizontal scroll: column c starts at x = (hscroll & 15) + 16 c and uses VSRAM word 2c (A) or
     * 2c + 1 (B). A partly shown column at the left takes, in H40, the AND of VSRAM words 38 and
     * 39 for both planes, and in H32 no vertical scroll (the vdptest vector; docs/HARDWARE.md). */
    int fine = hscroll & 15;
    int left = (width == 320) ? (v->s.vsram[38] & v->s.vsram[39]) : 0;
    for (int x = 0; x < width; x++) {
        int vs = v->s.vsram[plane];
        if (v->s.reg[11] & 4) vs = x < fine ? left : v->s.vsram[(((x - fine) >> 4) * 2 + plane) % 40];
        int py = (line + vs) & (h * 8 - 1);
        int px = (x - hscroll + (bug_x && x >= bug_x && x < bug_x + fine ? 16 : 0)) & (w * 8 - 1);
        uint16_t e = vword(v, table + (uint32_t)(((py >> 3) * w + (px >> 3)) << 1));
        int c = tile_pixel(v, e, px & 7, py & 7);
        out->col[x] = (uint8_t)(c ? (((e >> 13) & 3) << 4) | c : 0);
        out->pri[x] = (uint8_t)(e >> 15);
    }
}

/* the window's columns on this line (R17: right / left of a 2-cell column, R18: below / above a
 * cell row; a line inside the vertical area is all window) */
static void window_span(const mdrt_vdp *v, int line, int width, int *x0, int *x1)
{
    int wy = (v->s.reg[18] & 0x1F) * 8;
    if ((v->s.reg[18] & 0x80) ? line >= wy : line < wy) { *x0 = 0; *x1 = width; return; }
    int wx = (v->s.reg[17] & 0x1F) * 16;
    if (wx > width) wx = width;
    if (v->s.reg[17] & 0x80) { *x0 = wx; *x1 = width; } else { *x0 = 0; *x1 = wx; }
}

/* the sprites on a line, in link order from entry 0; the first opaque pixel wins. Y, size and
 * link come from the sprite cache, tile and X from VRAM. Per line at most 20 sprites and 320
 * pixels in H40 (16 and 256 in H32); a sprite at X 0 (raw) after another sprite on the line hides
 * the rest of the line's sprites. Sprite tiles go down the columns first. */
static void sprites(const mdrt_vdp *v, int line, int width, struct layer *out)
{
    int h40 = width == 320;
    uint32_t base = h40 ? (uint32_t)(v->s.reg[5] & 0x7E) << 9 : (uint32_t)(v->s.reg[5] & 0x7F) << 9;
    int total = h40 ? 80 : 64, per_line = h40 ? 20 : 16, dots = h40 ? 320 : 256;
    memset(out->col, 0, sizeof out->col);
    int link = 0, seen = 0, shown = 0, masked = 0;
    while (seen++ < total) {
        const uint8_t *c = v->sat + link * 8;
        int y = ((c[0] << 8 | c[1]) & 0x3FF) - 128;
        int sh = (c[2] & 3) + 1, sw = ((c[2] >> 2) & 3) + 1;
        int next = c[3] & 0x7F;
        if (line >= y && line < y + sh * 8) {
            if (++shown > per_line) break;
            uint32_t s = base + (uint32_t)(link << 3);
            uint16_t e = vword(v, s + 4);
            int rawx = vword(v, s + 6) & 0x1FF, x = rawx - 128;
            if (rawx == 0 && shown > 1) masked = 1;
            int row = line - y;
            if (e & 0x1000) row = sh * 8 - 1 - row;
            for (int cx = 0; cx < sw * 8; cx++) {
                if (dots-- <= 0) break;
                int sx = x + cx;
                if (masked || sx < 0 || sx >= width || out->col[sx]) continue;
                int tx = (e & 0x0800) ? sw * 8 - 1 - cx : cx;
                uint16_t t = (uint16_t)((e & 0x7FF) + (tx >> 3) * sh + (row >> 3));
                int p = tile_pixel(v, (uint16_t)(t & 0x7FF), tx & 7, row & 7);
                if (!p) continue;
                out->col[sx] = (uint8_t)((((e >> 13) & 3) << 4) | p);
                out->pri[sx] = (uint8_t)(e >> 15);
            }
            if (masked || dots <= 0) break;
        }
        if (!next || next >= total) break;
        link = next;
    }
}

/* one line of the active area into o; each pixel's colour (palette << 4 | index, 0 the backdrop)
 * into idx too when it is given */
static void draw_line(const mdrt_vdp *v, int line, uint16_t *o, uint8_t *idx)
{
    int width = mdrt_vdp_width(v);
    uint16_t back = v->s.cram[v->s.reg[7] & 0x3F];
    if (!(v->s.reg[1] & 0x40)) {                          /* R1 DISP off: the backdrop */
        for (int x = 0; x < width; x++) o[x] = back;
        if (idx) memset(idx, 0, (size_t)width);
        return;
    }
    uint32_t ta = (uint32_t)(v->s.reg[2] & 0x38) << 10, tb = (uint32_t)(v->s.reg[4] & 7) << 13;
    uint32_t tw = width == 320 ? (uint32_t)(v->s.reg[3] & 0x3C) << 10 : (uint32_t)(v->s.reg[3] & 0x3E) << 10;
    uint32_t hs = (uint32_t)(v->s.reg[13] & 0x3F) << 10;
    int wcells = width == 320 ? 64 : 32;
    struct layer a, b, s;
    int hl = 0;                                           /* R11 bits 1-0: per cell (2) or line (3) */
    if ((v->s.reg[11] & 3) == 2) hl = line & ~7;
    else if ((v->s.reg[11] & 3) == 3) hl = line;
    int hsa = vword(v, hs + (uint32_t)(hl << 2)) & 0x3FF, hsb = vword(v, hs + (uint32_t)(hl << 2) + 2) & 0x3FF;
    int w0, w1;
    window_span(v, line, width, &w0, &w1);
    scroll_plane(v, line, tb, hsb, 1, width, 0, &b);
    scroll_plane(v, line, ta, hsa, 0, width, (w0 == 0 && w1 > 0 && w1 < width) ? w1 : 0, &a);
    for (int x = w0; x < w1; x++) {
        uint16_t e = vword(v, tw + (uint32_t)(((line >> 3) * wcells + (x >> 3)) << 1));
        int c = tile_pixel(v, e, x & 7, line & 7);
        a.col[x] = (uint8_t)(c ? (((e >> 13) & 3) << 4) | c : 0);
        a.pri[x] = (uint8_t)(e >> 15);
    }
    sprites(v, line, width, &s);
    for (int x = 0; x < width; x++) {
        uint8_t c = 0;
        if (s.col[x] && s.pri[x]) c = s.col[x];
        else if (a.col[x] && a.pri[x]) c = a.col[x];
        else if (b.col[x] && b.pri[x]) c = b.col[x];
        else if (s.col[x]) c = s.col[x];
        else if (a.col[x]) c = a.col[x];
        else if (b.col[x]) c = b.col[x];
        if (x < 8 && (v->s.reg[0] & 0x20)) c = 0;         /* R0 LCB: the left column blanked */
        o[x] = c ? v->s.cram[c] : back;
        if (idx) idx[x] = c;
    }
}

void mdrt_vdp_render(const mdrt_vdp *v, uint16_t *out, int pitch) { mdrt_vdp_render_lines(v, out, pitch, 0, mdrt_vdp_height(v)); }

void mdrt_vdp_render_lines(const mdrt_vdp *v, uint16_t *out, int pitch, int from, int to)
{
    int height = mdrt_vdp_height(v);
    if (from < 0) from = 0;
    if (to > height) to = height;
    for (int line = from; line < to; line++) draw_line(v, line, out + line * pitch, NULL);
}

/* ---- the beam: a frame drawn a line at a time ------------------------------------------------ */

#define HBLANK_MCLKS 860                                  /* a line's horizontal blank, from its start */

static void recolour(mdrt_vdp_beam *b, int line)
{
    const mdrt_vdp *v = b->v;
    uint16_t *o = b->out + line * b->pitch, back = v->s.cram[v->s.reg[7] & 0x3F];
    for (int x = 0; x < mdrt_vdp_width(v); x++) o[x] = b->idx[x] ? v->s.cram[b->idx[x]] : back;
}

static void blank(mdrt_vdp_beam *b, int line, int from, int to)
{
    const mdrt_vdp *v = b->v;
    uint16_t *o = b->out + line * b->pitch, back = v->s.cram[v->s.reg[7] & 0x3F];
    for (int x = from; x < to; x++) { o[x] = back; b->idx[x] = 0; }
}

/* a write in the line last drawn, at b->now: the oracle's rules (docs/HARDWARE.md, 2026-10-16) */
static void beam_hook(void *user, int what, unsigned changed)
{
    mdrt_vdp_beam *b = user;
    const mdrt_vdp *v = b->v;
    int line = b->drawn - 1;
    if (line < 0 || !changed) return;
    uint32_t start = mdrt_vdp_line_start(v, line);
    if (b->now < start || b->now - start >= MDRT_LINE_MCLKS_VDP) return;
    uint32_t offset = b->now - start;
    int disp = v->s.reg[1] & 0x40, width = mdrt_vdp_width(v);
    if (what >= 64) {                                     /* CRAM: recoloured */
        if (offset <= HBLANK_MCLKS && (disp || what - 64 == (v->s.reg[7] & 0x3F))) recolour(b, line);
    } else if (what == 7) {                               /* the backdrop: recoloured */
        if ((changed & 0x3F) && offset <= HBLANK_MCLKS) recolour(b, line);
    } else if (what >= 2 && what <= 4) {                  /* a name table: drawn again */
        if (disp && offset <= HBLANK_MCLKS) draw_line(v, line, b->out + line * b->pitch, b->idx);
    } else if (what == 1 && (changed & 0x40)) {           /* the display on or off */
        if (offset <= HBLANK_MCLKS) { draw_line(v, line, b->out + line * b->pitch, b->idx); return; }
        int px = (int)(offset - HBLANK_MCLKS) / ((v->s.reg[12] & 1) ? 8 : 10) + 16;
        if (px >= width) return;
        if (disp) { draw_line(v, line, b->out + line * b->pitch, b->idx); blank(b, line, 0, px); }
        else blank(b, line, px, width);
    }
}

void mdrt_vdp_beam_start(mdrt_vdp_beam *b, mdrt_vdp *v, uint16_t *out, int pitch)
{
    b->v = v;
    b->out = out;
    b->pitch = pitch;
    b->drawn = 0;
    b->now = 0;
    memset(b->idx, 0, sizeof b->idx);
    mdrt_vdp_set_write_hook(v, beam_hook, b);
}

void mdrt_vdp_beam_line(mdrt_vdp_beam *b)
{
    if (b->drawn >= mdrt_vdp_height(b->v)) return;
    draw_line(b->v, b->drawn, b->out + b->drawn * b->pitch, b->idx);
    b->drawn++;
}

void mdrt_vdp_beam_to(mdrt_vdp_beam *b, uint32_t t)
{
    int height = mdrt_vdp_height(b->v);
    while (b->drawn < height && mdrt_vdp_line_start(b->v, b->drawn) <= t) mdrt_vdp_beam_line(b);
    b->now = t;
}

uint32_t mdrt_vdp_rgb(uint16_t colour, int curve)
{
    static const uint8_t linear[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };
    static const uint8_t gpgx[8] = { 0, 34, 68, 102, 136, 170, 204, 238 };
    static const uint8_t console[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
    const uint8_t *t = curve == MDRT_RGB_CONSOLE ? console : curve == MDRT_RGB_GPGX ? gpgx : linear;
    return (uint32_t)t[(colour >> 1) & 7] << 16 | (uint32_t)t[(colour >> 5) & 7] << 8 | t[(colour >> 9) & 7];
}
