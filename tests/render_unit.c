/* WP2: the renderer on synthetic VRAM: scroll modes, the window in both directions, sprite
 * limits and masking, priorities, the left column blank. */
#include <string.h>
#include "check.h"
#include "mdrt/vdp.h"

static mdrt_vdp v;
static uint16_t pic[320 * 240];

#define PA 0xC000u      /* plane A */
#define PB 0xE000u      /* plane B */
#define WIN 0xB000u     /* window */
#define SAT 0xF000u     /* sprites */
#define HS 0xFC00u      /* horizontal scroll */

static void w16(uint32_t a, uint16_t w) { v.s.vram[a & 0xFFFF] = (uint8_t)(w >> 8); v.s.vram[(a + 1) & 0xFFFF] = (uint8_t)w; }
/* tile n filled with colour index c */
static void solid_tile(int n, int c) { memset(v.s.vram + n * 32, (c << 4) | c, 32); }
/* tile n: column x (0-7) colour c, the rest transparent */
static void column_tile(int n, int x, int c)
{
    memset(v.s.vram + n * 32, 0, 32);
    for (int y = 0; y < 8; y++) {
        uint8_t *b = v.s.vram + n * 32 + y * 4 + x / 2;
        *b = (uint8_t)((x & 1) ? (*b & 0xF0) | c : (*b & 0x0F) | (c << 4));
    }
}
static void sprite(int i, int y, int size, int link, uint16_t tile, int x)
{
    uint32_t a = SAT + (uint32_t)i * 8;
    w16(a, (uint16_t)(y + 128)); v.s.vram[a + 2] = (uint8_t)size; v.s.vram[a + 3] = (uint8_t)link;
    w16(a + 4, tile); w16(a + 6, (uint16_t)(x + 128));
}

/* H32, 64x32 planes, display on, everything else zero; colours: index n of palette p = p * 16 + n */
static void setup(void)
{
    mdrt_vdp_init(&v, 0);
    mdrt_vdp_state s;
    memset(&s, 0, sizeof s);
    s.reg[1] = 0x44;                                      /* display, mode 5 */
    s.reg[2] = PA >> 10; s.reg[3] = WIN >> 10; s.reg[4] = PB >> 13; s.reg[5] = SAT >> 9;
    s.reg[13] = HS >> 10; s.reg[15] = 2; s.reg[16] = 0x01;
    for (int i = 0; i < 64; i++) s.cram[i] = (uint16_t)(i * 2);   /* distinct colours */
    s.reg[7] = 0x3F;                                      /* backdrop: colour 63 */
    mdrt_vdp_set_state(&v, &s);
}
static void refresh_sat(void) { mdrt_vdp_state s; mdrt_vdp_get_state(&v, &s); mdrt_vdp_set_state(&v, &s); }
static uint16_t at(int x, int y) { return pic[y * 320 + x]; }
#define COL(p, n) ((uint16_t)(((p) * 16 + (n)) * 2))

static void planes_and_scroll(void)
{
    setup();
    solid_tile(1, 3);
    column_tile(2, 0, 5);
    w16(PB, 0x0001);                                      /* B cell (0,0): tile 1 */
    w16(PA + 2, 0x2002);                                  /* A cell (1,0): tile 2, palette 1 */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(mdrt_vdp_width(&v), 256);
    CHECK_EQ(at(0, 0), COL(0, 3));                        /* B */
    CHECK_EQ(at(8, 0), COL(1, 5));                        /* A's column 0 */
    CHECK_EQ(at(9, 0), 63 * 2);                           /* backdrop */
    /* full-screen scroll: A right by 3 */
    w16(HS, 3);
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(11, 0), COL(1, 5));
    CHECK_EQ(at(8, 0), 63 * 2);
    /* per-line scroll (R11 = 3): line 1 scrolls by 1, line 0 by 3 */
    v.s.reg[11] = 3;
    w16(HS + 4, 1);
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(11, 0), COL(1, 5));
    CHECK_EQ(at(9, 1), COL(1, 5));
    /* per-cell scroll (R11 = 2): lines 0-7 use entry 0 */
    v.s.reg[11] = 2;
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(11, 1), COL(1, 5));
    /* vertical scroll, full screen: A up by 8 shows row 1 at line 0 */
    v.s.reg[11] = 0;
    w16(HS, 0);
    w16(PA + 64 * 2 + 2, 0x2002);                         /* A cell (1,1) */
    w16(PA + 2, 0);
    v.s.vsram[0] = 8;
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(8, 0), COL(1, 5));
    /* per 2-cell column: only columns 0-15 (pixels) scrolled */
    v.s.reg[11] = 4;
    v.s.vsram[0] = 0; v.s.vsram[2] = 8;
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(8, 8), COL(1, 5));                        /* column pair 0 unscrolled: row 1 at line 8 */
    /* priorities: high B over low A */
    v.s.reg[11] = 0; v.s.vsram[0] = 0; v.s.vsram[2] = 0;
    w16(PA, 0x0001 | 0x2000);                             /* A low (0,0), palette 1 */
    w16(PB, 0x8001);                                      /* B high */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(0, 0), COL(0, 3));
    w16(PA, 0xA001);                                      /* A high too: A wins */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(0, 0), COL(1, 3));
    /* display off: backdrop */
    v.s.reg[1] = 0x04;
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(0, 0), 63 * 2);
}

static void window(void)
{
    setup();
    solid_tile(1, 3);
    solid_tile(2, 7);
    for (int i = 0; i < 32 * 28; i++) w16(PA + (uint32_t)(i / 32 * 64 + i % 32) * 2, 0x0001);
    for (int i = 0; i < 32 * 28; i++) w16(WIN + (uint32_t)i * 2, 0x0002);
    v.s.reg[17] = 0x02;                                   /* left of 2 x 2 cells: x < 32 */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(31, 100), COL(0, 7));
    CHECK_EQ(at(32, 100), COL(0, 3));
    v.s.reg[17] = 0x82;                                   /* right of x 32 */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(31, 100), COL(0, 3));
    CHECK_EQ(at(32, 100), COL(0, 7));
    v.s.reg[17] = 0; v.s.reg[18] = 0x03;                  /* above line 24 */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(100, 23), COL(0, 7));
    CHECK_EQ(at(100, 24), COL(0, 3));
    v.s.reg[18] = 0x83;                                   /* below */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(100, 23), COL(0, 3));
    CHECK_EQ(at(100, 24), COL(0, 7));
    /* left column blank (R0 bit 5) */
    v.s.reg[0] = 0x20;
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(7, 100), 63 * 2);
    CHECK_EQ(at(8, 100), COL(0, 7));
}

static void sprites(void)
{
    setup();
    solid_tile(1, 4);
    solid_tile(2, 6);
    /* a 2x1 sprite (tiles 1 then 2 across: column-major, one row) at 10, 20 */
    sprite(0, 20, 0x04, 1, 0x0001, 10);
    sprite(1, 20, 0x00, 0, 0x0002, 12);                   /* behind sprite 0 where they overlap */
    refresh_sat();
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(10, 20), COL(0, 4));
    CHECK_EQ(at(12, 20), COL(0, 4));                      /* the overlap: the earlier sprite in the list wins */
    CHECK_EQ(at(18, 20), COL(0, 6));                      /* its second column: tile 1 + height (1) = tile 2 */
    /* 17 sprites on a line in H32: the 17th is not drawn */
    setup();
    solid_tile(1, 4);
    for (int i = 0; i < 17; i++) sprite(i, 50, 0, i < 16 ? i + 1 : 0, 0x0001, i * 12);
    refresh_sat();
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(15 * 12, 50), COL(0, 4));
    CHECK_EQ(at(16 * 12, 50), 63 * 2);
    /* masking: a sprite at raw X 0 after another sprite on the line hides the sprites after it */
    setup();
    solid_tile(1, 4);
    sprite(0, 60, 0, 1, 0x0001, 20);
    sprite(1, 60, 0, 2, 0x0001, -128);                    /* raw X 0 */
    sprite(2, 60, 0, 0, 0x0001, 40);
    refresh_sat();
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(20, 60), COL(0, 4));
    CHECK_EQ(at(40, 60), 63 * 2);
    /* ... but not as the first sprite on the line */
    setup();
    solid_tile(1, 4);
    sprite(0, 60, 0, 1, 0x0001, -128);
    sprite(1, 60, 0, 0, 0x0001, 40);
    refresh_sat();
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(40, 60), COL(0, 4));
}

/* the column vertical scroll follows the fine horizontal scroll; the window bug */
static void column_scroll_and_window_bug(void)
{
    setup();
    solid_tile(1, 3);
    solid_tile(2, 5);
    for (int c = 0; c < 64; c++) { w16(PA + (uint32_t)c * 2, 0x0001); w16(PA + 128 + (uint32_t)c * 2, 0x0002); }
    w16(HS, 4);                                           /* plane A fine scroll 4 */
    v.s.reg[11] = 4;
    v.s.vsram[0] = 8;                                     /* column 0 of A: row 1 at line 0 */
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(5, 0), COL(0, 5));                        /* column 0 covers x 4-19 */
    CHECK_EQ(at(2, 0), COL(0, 3));                        /* the partial column: H32, no vertical scroll */
    CHECK_EQ(at(20, 0), COL(0, 3));                       /* column 1: VSRAM word 2 = 0 */
    /* the window bug: window left of x 32, plane A fine scroll 4: x 32-35 show the next column */
    setup();
    solid_tile(1, 3);
    solid_tile(2, 5);
    for (int c = 0; c < 64; c++) w16(PA + (uint32_t)c * 2, c == 5 ? 0x0002 : 0x0001);
    w16(HS, 4);
    v.s.reg[17] = 0x02;
    mdrt_vdp_render(&v, pic, 320);
    CHECK_EQ(at(33, 0), COL(0, 5));                       /* would be cell 3; shows cell 5 */
    CHECK_EQ(at(36, 0), COL(0, 3));                       /* cell 4 */
}

int main(void)
{
    planes_and_scroll();
    column_scroll_and_window_bug();
    window();
    sprites();
    return check_done("render_unit");
}
