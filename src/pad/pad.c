/* The controller ports (Sega2.doc "I/O"; the pad protocol as in gen_hw.txt):
 *   data registers ($A10003 / 5 / 7): bit 7 reads back what was written, bit 6 is TH; a bit set
 *   in the control register ($A10009 / B / D) is an output and reads back the written value,
 *   the others read the pad's lines.
 *   3-button pad: TH 1: ?1CBRLDU, TH 0: ?0SA00DU (active low).
 *   6-button pad: counting TH low-to-high transitions, the third TH 0 read gives D3-D0 all 0, the
 *   following TH 1 gives ?1CBMXYZ and the TH 0 after it D3-D0 all 1. */
#include <string.h>
#include "mdrt/pad.h"

void mdrt_pads_init(mdrt_pads *p, int overseas, int pal)
{
    memset(p, 0, sizeof *p);
    p->reg[0] = (uint8_t)((overseas ? 0x80 : 0) | (pal ? 0x40 : 0) | 0x20);   /* no expansion unit */
    p->reg[7] = 0xFF;                                     /* serial transmit buffers */
    p->reg[10] = 0xFF;
    p->reg[13] = 0xFB;
    p->type[0] = p->type[1] = MDRT_PAD_3BUTTON;
}

void mdrt_pad_set_type(mdrt_pads *p, int port, int type) { if (port >= 0 && port < 2) p->type[port] = (uint8_t)type; }
void mdrt_pad_set(mdrt_pads *p, int port, uint16_t buttons) { if (port >= 0 && port < 2) p->buttons[port] = buttons; }
void mdrt_pads_frame(mdrt_pads *p) { p->steps[0] = p->steps[1] = 0; }

/* the TH level: the written value when TH is an output, else pulled high */
static int th(const mdrt_pads *p, int port)
{
    return (p->reg[4 + port] & 0x40) ? (p->reg[1 + port] >> 6) & 1 : 1;
}

/* the lines a pad drives: D6 is TH, D5-D0 the buttons for the TH level (active low) */
static uint8_t pad_lines(const mdrt_pads *p, int port)
{
    int t = th(p, port);
    uint8_t d = (uint8_t)((t << 6) | 0x3F);
    uint16_t b = p->buttons[port];
    if (p->type[port] == MDRT_PAD_NONE) return 0x7F;
    if (p->type[port] == MDRT_PAD_6BUTTON) {
        int s = p->steps[port];
        if (!t && s == 2) return (uint8_t)(d & ~(((b >> 2) & 0x30) | 0x0F));          /* ?0SA0000 */
        if (t && s == 3) return (uint8_t)(d & ~((b & 0x30) | ((b >> 8) & 0x0F)));     /* ?1CBMXYZ */
        if (!t && s == 3) return (uint8_t)(d & ~((b >> 2) & 0x30));                   /* ?0SA1111 */
    }
    if (t) return (uint8_t)(d & ~(b & 0x3F));                                          /* ?1CBRLDU */
    return (uint8_t)(d & ~(((b >> 2) & 0x30) | (b & 0x03) | 0x0C));                    /* ?0SA00DU */
}

uint8_t mdrt_pad_read(mdrt_pads *p, uint32_t addr)
{
    int r = (int)((addr & 0x1F) >> 1);
    if (r >= 1 && r <= 3) {
        uint8_t mask = (uint8_t)(0x80 | p->reg[r + 3]);
        uint8_t lines = r <= 2 ? pad_lines(p, r - 1) : 0x7F;
        return (uint8_t)((p->reg[r] & mask) | (lines & ~mask));
    }
    return p->reg[r & 15];
}

void mdrt_pad_write(mdrt_pads *p, uint32_t addr, uint8_t value)
{
    int r = (int)((addr & 0x1F) >> 1);
    if (r == 0) return;                                   /* the version register is read-only */
    if (r >= 1 && r <= 2) {
        int port = r - 1, before = th(p, port);
        p->reg[r] = value;
        if (!before && th(p, port) && p->steps[port] < 4) p->steps[port]++;           /* TH low to high */
        return;
    }
    if (r >= 4 && r <= 5) {
        int port = r - 4, before = th(p, port);
        p->reg[r] = value;
        if (!before && th(p, port) && p->steps[port] < 4) p->steps[port]++;
        return;
    }
    p->reg[r & 15] = value;
}
