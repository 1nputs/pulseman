/* md-runtime: the composition of the hardware modules and the timing constants.
 * See docs/WORKPLAN.md. */
#ifndef MDRT_MDRT_H
#define MDRT_MDRT_H
#include <stdint.h>

/* NTSC clocks (Sega Genesis Software Manual; the YM2612 divides the 68k clock by 144) */
#define MDRT_MASTER_CLOCK_NTSC 53693175u
#define MDRT_M68K_CLOCK_NTSC   (MDRT_MASTER_CLOCK_NTSC / 7)    /* 7670453 Hz */
#define MDRT_Z80_CLOCK_NTSC    (MDRT_MASTER_CLOCK_NTSC / 15)   /* 3579545 Hz, also the PSG */
#define MDRT_FM_RATE_NTSC      (MDRT_M68K_CLOCK_NTSC / 144)    /* 53267 Hz */
#define MDRT_LINES_NTSC        262
#define MDRT_LINES_PAL         313
#define MDRT_LINE_MCLKS        3420

#endif
