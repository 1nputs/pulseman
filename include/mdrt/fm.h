/* The YM2612 (FM): its four ports ($4000-$4003 from the Z80, $A04000-$A04003 from the 68k),
 * rendered at its own rate: a stereo sample every 24 of its internal clocks, which is 144 68k
 * clocks or 1008 master clocks (53267 Hz NTSC). The core is behind this interface: Nuked-OPN2
 * (third_party/nuked-opn2, LGPL-2.1) by default, ymfm (BSD-3) with `make FM_CORE=ymfm`. The
 * timers are not modelled (the client sequences its music on the 68k). */
#ifndef MDRT_FM_H
#define MDRT_FM_H
#include <stddef.h>
#include <stdint.h>

#define MDRT_FM_CYCLE_MCLKS  42       /* an internal clock: 6 of the 68k's */
#define MDRT_FM_SAMPLE_MCLKS 1008     /* a sample: 24 internal clocks */

typedef struct mdrt_fm mdrt_fm;

mdrt_fm *mdrt_fm_new(void);
void mdrt_fm_free(mdrt_fm *f);
/* the chip's reset line (the Mega Drive ties it to the Z80's) */
void mdrt_fm_reset(mdrt_fm *f);
/* a write to port 0-3 (0 / 1 part I's address / data, 2 / 3 part II's), at the chip's time now.
 * The chip takes a write over its next clocks: the caller runs it between writes as the
 * hardware's time passes (two writes with no time between get one internal clock apart). */
void mdrt_fm_write(mdrt_fm *f, int port, uint8_t value);
/* run for mclk master clocks: the samples that complete (left, right pairs) go to out, at most
 * max of them; returns how many. A part of an internal clock left over is kept for the next run. */
int mdrt_fm_run(mdrt_fm *f, uint32_t mclk, int16_t *out, int max);
/* the last value written to register reg of part 0 or 1 (for snapshots and tests) */
uint8_t mdrt_fm_reg(const mdrt_fm *f, int part, uint8_t reg);
/* the core's name ("nuked" or "ymfm") */
const char *mdrt_fm_core(void);
/* the chip and its core as bytes: save returns the size (out NULL: only that); load returns 0, or
 * -1 for bytes of another size or another core (then nothing changed). A state names its core:
 * it loads only into a build with the same one. */
size_t mdrt_fm_save(const mdrt_fm *f, uint8_t *out);
int mdrt_fm_load(mdrt_fm *f, const uint8_t *in, size_t size);

#endif
