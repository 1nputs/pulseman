/* WP1: the port rules, each with a hand-written case. */
#include <string.h>
#include "check.h"
#include "mdrt/vdp.h"

static mdrt_vdp v;
static uint8_t mem[0x40000];
static uint8_t read8(void *user, uint32_t addr) { (void)user; return mem[addr % sizeof mem]; }

static void setup(void)
{
    mdrt_vdp_init(&v, 0);
    mdrt_vdp_set_memory(&v, read8, NULL);
    mdrt_vdp_ctrl16(&v, 0x8114);                          /* mode 5, DMA enabled, display off */
    mdrt_vdp_ctrl16(&v, 0x8F02);                          /* auto-increment 2 */
}

static void registers_and_writes(void)
{
    setup();
    CHECK_EQ(v.s.reg[1], 0x14);
    CHECK_EQ(v.s.reg[15], 2);
    mdrt_vdp_ctrl16(&v, 0x9F12);                          /* register 31 does not exist */
    CHECK_EQ(v.pending, 0);
    /* VRAM write at 0, then at an odd address: the bytes swap */
    mdrt_vdp_ctrl32(&v, 0x40000000);
    mdrt_vdp_data16(&v, 0x1234);
    CHECK_EQ(v.s.vram[0], 0x12);
    CHECK_EQ(v.s.vram[1], 0x34);
    CHECK_EQ(v.addr, 2);
    mdrt_vdp_ctrl32(&v, 0x40050000);                      /* VRAM write at 5 */
    mdrt_vdp_data16(&v, 0xABCD);
    CHECK_EQ(v.s.vram[4], 0xCD);
    CHECK_EQ(v.s.vram[5], 0xAB);
    /* address bits 15-14 come from the second word */
    mdrt_vdp_ctrl32(&v, 0x40020003);                      /* $C002 */
    mdrt_vdp_data16(&v, 0x5566);
    CHECK_EQ(v.s.vram[0xC002], 0x55);
    /* VRAM read */
    mdrt_vdp_ctrl32(&v, 0x00000000);
    CHECK_EQ(mdrt_vdp_read16(&v), 0x1234);
    CHECK_EQ(mdrt_vdp_read16(&v), 0x0000);
    /* CRAM: 9 bits */
    mdrt_vdp_ctrl32(&v, 0xC0020000);
    mdrt_vdp_data16(&v, 0xFFFF);
    CHECK_EQ(v.s.cram[1], 0x0EEE);
    /* VSRAM: 11 bits, 40 words; writes past them are dropped */
    mdrt_vdp_ctrl32(&v, 0x40000010);
    mdrt_vdp_data16(&v, 0xFFFF);
    CHECK_EQ(v.s.vsram[0], 0x07FF);
    mdrt_vdp_ctrl32(&v, 0x40500010);
    mdrt_vdp_data16(&v, 0x0123);
    for (int i = 1; i < 40; i++) CHECK_EQ(v.s.vsram[i], 0);
    /* CRAM read: the bits CRAM does not drive come from the next FIFO entry */
    mdrt_vdp_ctrl32(&v, 0x00020020);
    uint16_t w = mdrt_vdp_read16(&v);
    CHECK_EQ(w & 0x0EEE, 0x0EEE);
}

static void pending_rules(void)
{
    setup();
    mdrt_vdp_ctrl16(&v, 0x4000);
    CHECK_EQ(v.pending, 1);
    mdrt_vdp_ctrl16(&v, 0x8F04);                          /* the second word, not a register write */
    CHECK_EQ(v.pending, 0);
    CHECK_EQ(v.s.reg[15], 2);
    mdrt_vdp_ctrl16(&v, 0x4000);
    (void)mdrt_vdp_status(&v);                            /* a status read ends the command */
    CHECK_EQ(v.pending, 0);
    mdrt_vdp_ctrl16(&v, 0x4000);
    mdrt_vdp_data16(&v, 0);                               /* so does a data port access */
    CHECK_EQ(v.pending, 0);
    mdrt_vdp_ctrl16(&v, 0x8100);                          /* mode 4 (no M5): no pending state */
    mdrt_vdp_ctrl16(&v, 0x4000);
    CHECK_EQ(v.pending, 0);
}

static void dma_68k(void)
{
    setup();
    for (int i = 0; i < 8; i++) mem[0x1000 + i] = (uint8_t)(0xA0 + i);
    mdrt_vdp_ctrl16(&v, 0x9302); mdrt_vdp_ctrl16(&v, 0x9400);     /* 2 words */
    mdrt_vdp_ctrl16(&v, 0x9500 | ((0x1000 >> 1) & 0xFF)); mdrt_vdp_ctrl16(&v, 0x9600 | ((0x1000 >> 9) & 0xFF));
    mdrt_vdp_ctrl16(&v, 0x9700);
    mdrt_vdp_ctrl32(&v, 0x41000080);                      /* VRAM $0100, CD5 */
    CHECK_EQ(v.s.vram[0x100], 0xA0);
    CHECK_EQ(v.s.vram[0x103], 0xA3);
    CHECK_EQ(v.s.vram[0x104], 0);
    CHECK_EQ(v.s.reg[19], 0); CHECK_EQ(v.s.reg[20], 0);
    CHECK_EQ(v.s.reg[21], 0x02);                          /* the source advanced by 2 words */
    /* the source counts within its 128 KB bank: from $1FFFE it wraps to $00000 */
    mem[0x1FFFE] = 0x11; mem[0x1FFFF] = 0x22; mem[0] = 0x33; mem[1] = 0x44;
    mdrt_vdp_ctrl16(&v, 0x9302); mdrt_vdp_ctrl16(&v, 0x9400);
    mdrt_vdp_ctrl16(&v, 0x95FF); mdrt_vdp_ctrl16(&v, 0x96FF); mdrt_vdp_ctrl16(&v, 0x9700);
    mdrt_vdp_ctrl32(&v, 0x42000080);
    CHECK_EQ(v.s.vram[0x200], 0x11);
    CHECK_EQ(v.s.vram[0x202], 0x33);
    /* to CRAM */
    mem[0x2000] = 0x0E; mem[0x2001] = 0xEE;
    mdrt_vdp_ctrl16(&v, 0x9301); mdrt_vdp_ctrl16(&v, 0x9400);
    mdrt_vdp_ctrl16(&v, 0x9500); mdrt_vdp_ctrl16(&v, 0x9610); mdrt_vdp_ctrl16(&v, 0x9700);
    mdrt_vdp_ctrl32(&v, 0xC0000080);
    CHECK_EQ(v.s.cram[0], 0x0EEE);
    /* DMA disabled (R1 M1 clear): a CD5 command does nothing */
    mdrt_vdp_ctrl16(&v, 0x8104);
    mdrt_vdp_ctrl16(&v, 0x9301);
    mdrt_vdp_ctrl32(&v, 0x43000080);
    CHECK_EQ(v.s.vram[0x300], 0);
}

static void dma_fill_and_copy(void)
{
    setup();
    mdrt_vdp_ctrl16(&v, 0x8F01);                          /* auto-increment 1 */
    mdrt_vdp_ctrl16(&v, 0x9304); mdrt_vdp_ctrl16(&v, 0x9400); mdrt_vdp_ctrl16(&v, 0x9780);   /* fill, 4 */
    mdrt_vdp_ctrl32(&v, 0x40000080);
    CHECK_EQ(v.fill, 1);
    mdrt_vdp_data16(&v, 0xAB12);                          /* the first write is a normal word write */
    CHECK_EQ(v.s.vram[0], 0xAB);                          /* then the high byte at the adjacent byte of 1, 2, 3, 4 */
    CHECK_EQ(v.s.vram[3], 0xAB);
    CHECK_EQ(v.s.vram[2], 0xAB);
    CHECK_EQ(v.s.vram[5], 0xAB);
    CHECK_EQ(v.s.vram[1], 0x12);
    CHECK_EQ(v.fill, 0);
    /* VRAM copy: 3 bytes from $0010 to $0100, odd / even swapped on both sides */
    for (int i = 0; i < 4; i++) v.s.vram[0x10 + i] = (uint8_t)(0x40 + i);
    mdrt_vdp_ctrl16(&v, 0x9303); mdrt_vdp_ctrl16(&v, 0x9400);
    mdrt_vdp_ctrl16(&v, 0x9510); mdrt_vdp_ctrl16(&v, 0x9600); mdrt_vdp_ctrl16(&v, 0x97C0);
    mdrt_vdp_ctrl32(&v, 0x010000C0);
    CHECK_EQ(v.s.vram[0x101], 0x41);
    CHECK_EQ(v.s.vram[0x100], 0x40);
    CHECK_EQ(v.s.vram[0x103], 0x43);
}

static void sprite_cache(void)
{
    setup();
    mdrt_vdp_ctrl16(&v, 0x8578);                          /* SAT at $F000 (H32) */
    mdrt_vdp_ctrl32(&v, 0x70000003);                      /* VRAM $F000 */
    mdrt_vdp_data32(&v, 0x00800501);                      /* Y, size, link: cached */
    mdrt_vdp_data32(&v, 0x2001008A);                      /* tile, X: not cached */
    CHECK_EQ(v.sat[0], 0x00); CHECK_EQ(v.sat[1], 0x80); CHECK_EQ(v.sat[2], 0x05); CHECK_EQ(v.sat[3], 0x01);
    CHECK_EQ(v.sat[4], 0);
    /* the cache is not reloaded when R5 moves */
    mdrt_vdp_ctrl16(&v, 0x8570);
    CHECK_EQ(v.sat[1], 0x80);
}

static void hv_counter(void)
{
    setup();
    mdrt_vdp_set_position(&v, 0xEA, 0);
    CHECK_EQ(mdrt_vdp_hv8(&v, 0), 0xEA);
    mdrt_vdp_set_position(&v, 0xEB, 0);                   /* NTSC: jumps back to $E5 */
    CHECK_EQ(mdrt_vdp_hv8(&v, 0), 0xE5);
    mdrt_vdp_set_position(&v, 261, 0);
    CHECK_EQ(mdrt_vdp_hv8(&v, 0), 0xFF);
    mdrt_vdp_set_position(&v, 0, 2 * 0x94);               /* H32: after $93 comes $E9 */
    CHECK_EQ(mdrt_vdp_hv8(&v, 1), 0xE9);
    /* any beam position, as the composition's beam asks */
    CHECK_EQ(mdrt_vdp_hv_at(&v, 235, 0), 0xE500);
    CHECK_EQ(mdrt_vdp_hv_at(&v, 100, 2 * 0x93), 0x6493);
    mdrt_vdp_ctrl16(&v, 0x8C81);                          /* H40: after $B6 comes $E4 */
    CHECK_EQ(mdrt_vdp_hv_at(&v, 0, 2 * 0xB6), 0x00B6);
    CHECK_EQ(mdrt_vdp_hv_at(&v, 0, 2 * 0xB7), 0x00E4);
    CHECK_EQ(mdrt_vdp_reg(&v, 12), 0x81);
    CHECK_EQ(mdrt_vdp_reg(&v, 24), 0);
}

int main(void)
{
    registers_and_writes();
    pending_rules();
    dma_68k();
    dma_fill_and_copy();
    sprite_cache();
    hv_counter();
    return check_done("vdp_unit");
}
