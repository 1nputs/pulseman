/* The YM2612 (FM): its four ports ($4000-$4003 from the Z80, $A04000-$A04003 from the 68k),
 * rendered at its own rate: a stereo sample every 24 of its internal clocks, which is 144 68k
 * clocks or 1008 master clocks (53267 Hz NTSC). The core is behind this interface, chosen when the
 * chip is made from those the build has: Nuked-OPN2 (third_party/nuked-opn2, LGPL-2.1) and ymfm
 * (BSD-3), both by default (`make FM_CORES="nuked ymfm"`; a build with ymfm only has no LGPL
 * code). The model: the YM2612 (the discrete chip of the first consoles, with its DAC's "ladder"
 * distortion) or the YM3438 (the integrated one of later consoles). The timers are not modelled
 * (the client sequences its music on the 68k). */
#ifndef MDRT_FM_H
#define MDRT_FM_H
#include <stddef.h>
#include <stdint.h>

#define MDRT_FM_CYCLE_MCLKS  42       /* an internal clock: 6 of the 68k's */
#define MDRT_FM_SAMPLE_MCLKS 1008     /* a sample: 24 internal clocks */

typedef struct mdrt_fm mdrt_fm;
enum { MDRT_FM_YM2612, MDRT_FM_YM3438 };

/* a chip with the default core (the first the build has: Nuked-OPN2 when it is there), a YM2612 */
mdrt_fm *mdrt_fm_new(void);
/* a chip with the core named (NULL: the default) and the model; NULL when the build has not that core */
mdrt_fm *mdrt_fm_new_core(const char *core, int model);
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
/* the default core's name ("nuked" or "ymfm"); a chip's core and model; the build's cores, from 0
 * (NULL past the last) */
const char *mdrt_fm_core(void);
const char *mdrt_fm_core_of(const mdrt_fm *f);
int mdrt_fm_model(const mdrt_fm *f);
const char *mdrt_fm_cores(int i);
/* the chip and its core as bytes: save returns the size (out NULL: only that); load returns 0, or
 * -1 for bytes of another size, core or model (then nothing changed). A state names its core: it
 * loads only into a chip with the same one. */
size_t mdrt_fm_save(const mdrt_fm *f, uint8_t *out);
int mdrt_fm_load(mdrt_fm *f, const uint8_t *in, size_t size);

#endif
