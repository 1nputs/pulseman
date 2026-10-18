/* The harness itself and the constants. */
#include "check.h"
#include "mdrt/mdrt.h"

int main(void)
{
    CHECK_EQ(MDRT_M68K_CLOCK_NTSC, 7670453);
    CHECK_EQ(MDRT_Z80_CLOCK_NTSC, 3579545);
    CHECK_EQ(MDRT_FM_RATE_NTSC, 53267);
    mdrt_config c = { 0 };
    mdrt *m = mdrt_new(&c);
    CHECK_EQ(mdrt_master_clock(m), 53693175);
    CHECK_EQ(mdrt_frame_mclks(m), 896040);
    mdrt_free(m);
    c.pal = 1;
    m = mdrt_new(&c);
    CHECK_EQ(mdrt_master_clock(m), 53203424);
    CHECK_EQ(mdrt_frame_mclks(m), 1070460);
    mdrt_free(m);
    return check_done("smoke");
}
