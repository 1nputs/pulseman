/* The VDP (Yamaha YM7101): registers, the control and data ports, VRAM / CRAM / VSRAM, DMA, the
 * status word and the HV counter. Mode 5 (Mega Drive) only. Timing is not modelled: DMA completes
 * at once and the FIFO is always empty, because clients wait for the vertical blank
 * cooperatively. Everything a game can read that depends on time (HV counter, status timing bits)
 * can be supplied by the client, so recorded runs replay exactly.
 *
 * The struct is public so it can be embedded; its fields are not API and may change. Read the
 * state through the functions (mdrt_vdp_reg, mdrt_vdp_get_state). */
#ifndef MDRT_VDP_H
#define MDRT_VDP_H
#include <stdint.h>

/* the state a game can observe, and a snapshot carries */
typedef struct mdrt_vdp_state {
    uint8_t reg[24];
    uint8_t vram[0x10000];      /* VDP byte order: the word at an even address is vram[a] << 8 | vram[a + 1] */
    uint16_t cram[64];          /* bus format 0000BBB0GGG0RRR0 */
    uint16_t vsram[40];         /* 11 bits */
} mdrt_vdp_state;

/* a byte of the 68k address space (DMA sources): the client's ROM and work RAM */
typedef uint8_t (*mdrt_read8_fn)(void *user, uint32_t addr);
/* the HV counter word (V counter high byte, H counter low byte) at the moment of the read */
typedef uint16_t (*mdrt_hv_fn)(void *user);

typedef struct mdrt_vdp {
    mdrt_vdp_state s;
    uint8_t sat[0x280];         /* sprite attribute cache: bytes 0-3 of each entry (80 x 8) */
    uint16_t addr;              /* access address */
    uint16_t addr_hi;           /* A15-A14, from the second command word */
    uint8_t code;               /* CD5-CD0 */
    uint8_t pending;            /* the first command word was written */
    uint8_t fill;               /* a DMA fill waits for the next data port write */
    uint8_t fifo_idx;
    uint16_t fifo[4];           /* the last four words written (unused bits of CRAM / VSRAM reads) */
    uint8_t pal;                /* status bit 0 */
    uint8_t vblank;             /* status bit 3 (the frame layer sets it) */
    uint8_t vint_pending;       /* status bit 7 */
    uint16_t line, hpos;        /* the beam position the default HV counter reports */
    mdrt_read8_fn read8;
    void *read8_user;
    mdrt_hv_fn hv;
    void *hv_user;
} mdrt_vdp;

void mdrt_vdp_init(mdrt_vdp *v, int pal);
void mdrt_vdp_set_memory(mdrt_vdp *v, mdrt_read8_fn fn, void *user);
/* replace the HV counter model (lockstep: the recorded values); NULL restores the model */
void mdrt_vdp_set_hv_source(mdrt_vdp *v, mdrt_hv_fn fn, void *user);
/* the beam position the HV counter model reports: line 0-261 (312 PAL), hpos 0-341 (H32) or
 * 0-419 (H40) in pixel clocks */
void mdrt_vdp_set_position(mdrt_vdp *v, uint16_t line, uint16_t hpos);
/* the HV counter word the hardware shows at that beam position (the V counter's jump back in the
 * vertical blank, the H counter's in the horizontal blank) */
uint16_t mdrt_vdp_hv_at(const mdrt_vdp *v, unsigned line, unsigned hpos);

/* 68k accesses: move.w / move.l to the control port ($C00004) and the data port ($C00000) */
void mdrt_vdp_ctrl16(mdrt_vdp *v, uint16_t w);
void mdrt_vdp_ctrl32(mdrt_vdp *v, uint32_t l);
void mdrt_vdp_data16(mdrt_vdp *v, uint16_t w);
void mdrt_vdp_data32(mdrt_vdp *v, uint32_t l);
uint16_t mdrt_vdp_read16(mdrt_vdp *v);                    /* move.w from the data port */
uint16_t mdrt_vdp_status(mdrt_vdp *v);                    /* move.w from the control port */
uint16_t mdrt_vdp_hv16(mdrt_vdp *v);                      /* move.w from $C00008 */
uint8_t mdrt_vdp_hv8(mdrt_vdp *v, int odd);               /* move.b from $C00008 (V) or $C00009 (H) */

/* register n (0-23) as last written */
uint8_t mdrt_vdp_reg(const mdrt_vdp *v, int n);

/* snapshots: get copies the observable state out; set loads it and resets the port state */
void mdrt_vdp_get_state(const mdrt_vdp *v, mdrt_vdp_state *out);
void mdrt_vdp_set_state(mdrt_vdp *v, const mdrt_vdp_state *in);

/* the frame layer: the vertical blank flag in the status word */
void mdrt_vdp_set_vblank(mdrt_vdp *v, int on);

/* the active area: 256 or 320 wide (H32 / H40), 224 or 240 high (V28 / V30) */
int mdrt_vdp_width(const mdrt_vdp *v);
int mdrt_vdp_height(const mdrt_vdp *v);

/* draw the active area into out (9-bit colours in bus format, pitch in pixels) */
void mdrt_vdp_render(const mdrt_vdp *v, uint16_t *out, int pitch);
/* a bus format colour to 0x00RRGGBB, 3-bit levels expanded by the curve (0 linear: level x 255 / 7,
 * 1 the levels x 34 Genesis Plus GX uses for normal colours) */
uint32_t mdrt_vdp_rgb(uint16_t colour, int curve);

#endif
