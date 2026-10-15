/* WP0: the harness itself and the constants. */
#include "check.h"
#include "mdrt/mdrt.h"

int main(void)
{
    CHECK_EQ(MDRT_M68K_CLOCK_NTSC, 7670453);
    CHECK_EQ(MDRT_Z80_CLOCK_NTSC, 3579545);
    CHECK_EQ(MDRT_FM_RATE_NTSC, 53267);
    return check_done("smoke");
}
