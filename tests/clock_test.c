/* The composition's clock: the picture drawn a line at a time as the client's position passes each
 * line, the H-interrupt handler called on the lines register 10 picks, mdrt_wait_line,
 * mdrt_charge, the HV counter at the position, and a state taken in the middle of the display.
 * The timing rules are the oracle's (the vdptest vector's phases 8 and 9 check them against it);
 * here they are checked through the client's hooks. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"

static uint16_t shown[320 * 240];
static int shown_h;
static void present(void *user, const uint16_t *px, int w, int h, int pitch)
{
    (void)user;
    for (int y = 0; y < h; y++) memcpy(shown + y * 320, px + y * pitch, (size_t)w * 2);
    shown_h = h;
}

/* a console showing the backdrop only (planes of transparent tiles), colour n a different one for
 * each n: red n & 7, green n >> 3 */
static mdrt *console(void)
{
    mdrt_config c = { 0 };
    c.platform = (mdrt_platform){ .present = present };
    mdrt *m = mdrt_new(&c);
    mdrt_vdp *v = mdrt_get_vdp(m);
    static const uint8_t regs[] = { 0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
    for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
    mdrt_vdp_ctrl32(v, 0xC0000000);
    for (int i = 0; i < 64; i++) mdrt_vdp_data16(v, (uint16_t)((i & 7) << 1 | (i >> 3) << 5));
    return m;
}
static int backdrop(int line) { uint16_t c = shown[line * 320 + 100]; return (c >> 1 & 7) | (c >> 5 & 7) << 3; }   /* the colour a line showed */

static int bands;
static void hint(void *user)
{
    mdrt *m = user;
    mdrt_vdp_ctrl16(mdrt_get_vdp(m), (uint16_t)(0x8700 | ++bands));
}

/* H-interrupts every 8 lines, each setting the backdrop to the next colour: with the clock moved
 * by the client or not */
static void hints(int move)
{
    mdrt *m = console();
    mdrt_vdp *v = mdrt_get_vdp(m);
    mdrt_set_hint(m, hint, m);
    mdrt_vdp_ctrl16(v, 0x8A07);                           /* every 8 lines */
    mdrt_vdp_ctrl16(v, 0x8014);                           /* the H-interrupt on */
    mdrt_vdp_ctrl16(v, 0x8700);
    mdrt_wait_vblank(m);
    mdrt_vblank_end(m);
    mdrt_vdp_ctrl16(v, 0x8700);
    bands = 0;
    if (move) for (int i = 0; i < 100; i++) mdrt_charge(m, 1000);
    mdrt_wait_vblank(m);
    /* the counter: reloaded on line 261, so it runs out on lines 7, 15, 23 ...; the handler's
     * write comes at the start of that line, in its horizontal blank, so the line itself changes */
    int good = 0;
    for (int y = 0; y < 224; y++) good += backdrop(y) == (y + 1) / 8;
    printf("  H-interrupts%s: %d bands, %d of 224 lines as expected\n", move ? " (the clock moved)" : "", bands, good);
    CHECK_EQ(good, 224);
    CHECK_EQ(bands, 28);
    mdrt_free(m);
}

int main(void)
{
    hints(0);
    hints(1);

    /* the clock, mdrt_wait_line, mdrt_charge and the HV counter */
    mdrt *m = console();
    mdrt_vdp *v = mdrt_get_vdp(m);
    mdrt_wait_vblank(m);
    mdrt_vblank_end(m);
    CHECK_EQ(mdrt_now(m), 0);
    CHECK_EQ(mdrt_charge(m, 100), 0);
    CHECK_EQ(mdrt_now(m), 700);
    CHECK_EQ(mdrt_wait_line(m, 230), 0);                  /* a line of the vertical blank */
    CHECK_EQ(mdrt_now(m), 6 * 3420);
    CHECK_EQ(mdrt_wait_line(m, 100), 0);
    CHECK_EQ(mdrt_now(m), (38 + 100) * 3420);
    CHECK_EQ(mdrt_vdp_hv8(v, 0), 100);
    mdrt_vdp_ctrl16(v, 0x8705);                           /* in line 100's horizontal blank: from line 100 */
    mdrt_run_until(m, (38 + 150) * 3420 + 1500);
    mdrt_vdp_ctrl16(v, 0x8706);                           /* after it: from line 151 */
    CHECK_EQ(mdrt_wait_line(m, 120), 1);                  /* gone by */
    CHECK_EQ((mdrt_vdp_hv16(v) >> 8), 150);
    /* a state here, in the middle of the display, then the rest of the frame; and again from it */
    size_t n = mdrt_get_state(m, NULL, 0);
    uint8_t *st = malloc(n);
    mdrt_get_state(m, st, n);
    mdrt_wait_line(m, 200);
    mdrt_vdp_ctrl16(v, 0x8707);
    mdrt_wait_vblank(m);
    static uint16_t first[320 * 240];
    memcpy(first, shown, sizeof first);
    int good = 0;
    for (int y = 0; y < 224; y++) good += backdrop(y) == (y < 100 ? 0 : y < 151 ? 5 : y < 200 ? 6 : 7);
    printf("  mdrt_wait_line, mdrt_run_until: %d of 224 lines as expected\n", good);
    CHECK_EQ(good, 224);
    CHECK_EQ(mdrt_set_state(m, st, n), 0);
    CHECK_EQ(mdrt_now(m), (38 + 150) * 3420 + 1500);
    mdrt_wait_line(m, 200);
    mdrt_vdp_ctrl16(v, 0x8707);
    mdrt_wait_vblank(m);
    CHECK(!memcmp(first, shown, sizeof first));
    /* charged to the frame's end: the vertical blank is due */
    mdrt_vblank_end(m);
    CHECK_EQ(mdrt_charge(m, 896040 / 7 - 1), 0);
    CHECK_EQ(mdrt_charge(m, 10), 1);
    CHECK_EQ(mdrt_now(m), 896040);
    free(st);
    mdrt_free(m);
    return check_done("clock_test");
}
