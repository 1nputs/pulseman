/* The 68k's bus by address (mdrt_bus_*): the client's ROM and RAM through read68k, the VDP's ports
 * by width (a move.b to the data port lands on both halves, a move.l is one access), the PSG, the
 * I/O chip, save RAM, and a client's region (a bank-switching mapper: a register and a window) that
 * DMA from the 68k bus reads through. */
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"

static uint8_t ram[0x10000];
static uint8_t read68k(void *user, uint32_t a)
{
    (void)user;
    if (a >= 0xFF0000) return ram[a & 0xFFFF];
    return a < 0x400000 ? (uint8_t)(a ^ (a >> 8)) : 0xFF;   /* the ROM: a pattern */
}

/* the mapper: $A130F3 picks a bank of 512 KB of a 4 MB ROM shown at $080000-$0FFFFF */
static uint8_t bank;
static uint8_t window_read(void *user, uint32_t a) { (void)user; return (uint8_t)(bank * 16 + (a & 15)); }
static uint8_t reg_read(void *user, uint32_t a) { (void)user; (void)a; return bank; }
static void reg_write(void *user, uint32_t a, uint8_t v) { (void)user; if (a == 0xA130F3) bank = v; }

static long sound;
static void audio(void *user, const int16_t *s, int n)
{
    (void)user;
    for (int i = 0; i < 2 * n; i++) sound += s[i] < 0 ? -s[i] : s[i];
}

int main(void)
{
    mdrt_config c = { 0 };
    c.read68k = read68k;
    c.sram_size = 0x100;
    c.platform = (mdrt_platform){ .audio = audio };
    mdrt *m = mdrt_new(&c);
    mdrt_vdp *v = mdrt_get_vdp(m);

    /* the client's memory */
    ram[0x1234] = 0xAB; ram[0x1235] = 0xCD;
    CHECK_EQ(mdrt_bus_read8(m, 0x000102), 0x02 ^ 0x01);
    CHECK_EQ(mdrt_bus_read16(m, 0xFF1234), 0xABCD);
    CHECK_EQ(mdrt_bus_read32(m, 0xFFFF1234), 0xABCD0000 | (uint32_t)ram[0x1236] << 8 | ram[0x1237]);   /* (24-bit addresses) */

    /* the VDP by width */
    mdrt_bus_write16(m, 0xC00004, 0x8F02);                /* a register */
    CHECK_EQ(mdrt_vdp_reg(v, 15), 2);
    mdrt_bus_write16(m, 0xC00004, 0x8144);                /* display on, mode 5 */
    mdrt_bus_write32(m, 0xC00004, 0x40000000);            /* VRAM $0000, one move.l */
    mdrt_bus_write16(m, 0xC00000, 0x1234);
    mdrt_bus_write8(m, 0xC00001, 0x56);                   /* a byte: on both halves */
    mdrt_vdp_state s;
    mdrt_vdp_get_state(v, &s);
    CHECK_EQ(s.vram[0] << 8 | s.vram[1], 0x1234);
    CHECK_EQ(s.vram[2] << 8 | s.vram[3], 0x5656);
    CHECK_EQ(mdrt_bus_read16(m, 0xC00004) & 0x0208, 0x0200);   /* the status word: FIFO empty, not in the blank */
    CHECK_EQ(mdrt_bus_read8(m, 0xC00008), mdrt_bus_read16(m, 0xC00008) >> 8 & 0xFF);
    /* the PSG: a tone at full volume */
    mdrt_bus_write8(m, 0xC00011, 0x80 | 0x0F);
    mdrt_bus_write8(m, 0xC00011, 0x08);
    mdrt_bus_write8(m, 0xC00011, 0x90);
    for (int f = 0; f < 3; f++) { mdrt_wait_vblank(m); mdrt_vblank_end(m); }
    CHECK(sound > 1000000);

    /* the I/O chip: pad 1's TH as an output, high: ?1CBRLDU */
    mdrt_pad_set(mdrt_get_pads(m), 0, MDRT_PAD_C | MDRT_PAD_UP);
    mdrt_bus_write8(m, 0xA10009, 0x40);
    mdrt_bus_write8(m, 0xA10003, 0x40);
    CHECK_EQ(mdrt_bus_read8(m, 0xA10003) & 0x3F, 0x3F & ~(0x20 | 0x01));
    CHECK_EQ(mdrt_bus_read8(m, 0xA10001) & 0x80, 0x80);   /* the version register: overseas */
    /* save RAM on the odd bytes */
    mdrt_bus_write8(m, 0x200011, 0x77);
    mdrt_bus_write16(m, 0x200012, 0x0088);
    CHECK_EQ(mdrt_bus_read8(m, 0x200011), 0x77);
    CHECK_EQ(mdrt_get_sram(m)->bytes[9], 0x88);
    CHECK_EQ(mdrt_bus_read8(m, 0x200010), 0xFF);

    /* a client's region: the mapper's register and its window, which DMA reads through */
    CHECK_EQ(mdrt_bus_map(m, 0xA130F0, 0x10, reg_read, reg_write, NULL), 0);
    CHECK_EQ(mdrt_bus_map(m, 0x080000, 0x80000, window_read, NULL, NULL), 0);
    mdrt_bus_write8(m, 0xA130F3, 5);
    CHECK_EQ(mdrt_bus_read8(m, 0xA130F3), 5);
    CHECK_EQ(mdrt_bus_read8(m, 0x080003), 5 * 16 + 3);
    CHECK_EQ(mdrt_bus_read8(m, 0x07FFFF), read68k(NULL, 0x07FFFF));   /* (just below: the ROM) */
    mdrt_bus_write16(m, 0xC00004, 0x8154);                /* DMA on */
    mdrt_bus_write32(m, 0xC00004, 0x93089400);            /* 8 words */
    mdrt_bus_write32(m, 0xC00004, 0x95009600);            /* from $080000: (R21-R23) >> 1 */
    mdrt_bus_write16(m, 0xC00004, 0x9704);
    mdrt_bus_write32(m, 0xC00004, 0x40000080 | (0x100 << 16));   /* VRAM $0100, DMA */
    mdrt_vdp_get_state(v, &s);
    int good = 0;
    for (int i = 0; i < 16; i++) good += s.vram[0x100 + i] == (uint8_t)(5 * 16 + i);
    CHECK_EQ(good, 16);
    printf("  ROM, RAM, the VDP by width, the PSG, the pads, save RAM; a mapper's window read by DMA: %d of 16 bytes\n", good);
    mdrt_free(m);
    return check_done("bus_test");
}
