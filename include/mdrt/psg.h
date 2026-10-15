/* The PSG: the SN76489 inside the Mega Drive's VDP (port $C00011). Three square-wave tone
 * channels and a noise channel, each with a 4-bit attenuation, clocked at the Z80's clock / 16.
 * The model renders mono samples at the YM2612's rate (1008 master clocks a sample), so the mixer
 * adds it to the FM output sample for sample. Each change of the output goes in as a band-limited
 * step (no aliasing of the square waves' harmonics), which delays the output by
 * MDRT_PSG_LATENCY samples. Integer arithmetic only: the same samples on every host.
 * See docs/HARDWARE.md for the sources. */
#ifndef MDRT_PSG_H
#define MDRT_PSG_H
#include <stdint.h>

#define MDRT_PSG_STEP_MCLKS   240     /* an internal clock: master / 15 / 16 */
#define MDRT_PSG_SAMPLE_MCLKS 1008    /* an output sample: the YM2612's, master / 7 / 144 */
#define MDRT_PSG_LATENCY      16      /* samples the output is late by */
#define MDRT_PSG_MAX          6144    /* a channel at full volume swings +-this: four, with the steps'
                                       * overshoot, stay inside the sample range */

typedef struct mdrt_psg {
    uint16_t reg[8];        /* tone 0, volume 0, tone 1, volume 1, tone 2, volume 2, noise, volume 3 */
    uint8_t latch;          /* the register the last latch byte chose */
    uint16_t count[4];      /* the down counters, in internal clocks */
    uint8_t flip[4];        /* the flip-flops (tone output; the noise channel's clocks its register) */
    uint16_t lfsr;          /* the noise shift register */
    uint8_t noise_out;      /* the bit last shifted out of it */
    uint16_t phase;         /* master clocks into the current internal clock */
    int32_t level;          /* the output now */
    uint32_t sample;        /* samples rendered */
    int32_t pending[64];    /* the band-limited steps still to come, by sample (mod 64) */
    int64_t sum;            /* the output so far, in 1/32768ths */
} mdrt_psg;

/* power on: tone and noise registers 0, all channels silent */
void mdrt_psg_init(mdrt_psg *p);
/* a byte written to the PSG port: a latch byte (1 cc t dddd) or a data byte (0 - dddddd) */
void mdrt_psg_write(mdrt_psg *p, uint8_t value);
/* the next n samples (mono) */
void mdrt_psg_render(mdrt_psg *p, int16_t *out, int n);

#endif
