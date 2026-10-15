/* The mixer: the FM chip's stereo samples and the PSG's mono ones (both at the YM2612's rate, 1008
 * master clocks a sample) added at the console's balance and level (as the oracle mixes them), the
 * DC taken out (the output capacitor's job on the
 * hardware: the FM chip's DAC sits a step above zero), resampled to the output rate with a
 * 20 kHz low-pass. Integer arithmetic only: the same samples on every host. */
#ifndef MDRT_MIX_H
#define MDRT_MIX_H
#include <stdint.h>

/* the gains of the FM and PSG outputs, in 1/32768ths: Genesis Plus GX's balance (FM preamp 100%,
 * PSG 150%) and loudness, measured from the oracle's sound traces (docs/HARDWARE.md) */
#define MDRT_MIX_FM_GAIN  58026
#define MDRT_MIX_PSG_GAIN 11000

typedef struct mdrt_mix {
    uint32_t rate;              /* output samples a second */
    uint64_t in;                /* input samples taken */
    uint64_t out;               /* output samples made */
    int32_t hist[2][64];        /* the last input samples (after the DC blocker), by index mod 64 */
    int32_t dc_x[2];            /* the DC blocker's last input */
    int64_t dc_y[2];            /* and output, in 1/65536ths */
} mdrt_mix;

void mdrt_mix_init(mdrt_mix *m, uint32_t rate);
/* n input samples: fm (left, right pairs; NULL for silence) and psg (mono; NULL for silence); the
 * output samples they complete (left, right pairs) go to out; returns how many. out has room for
 * n * rate / 53267 + 2 of them. The output is 16 input samples (0.3 ms) late. */
int mdrt_mix_run(mdrt_mix *m, const int16_t *fm, const int16_t *psg, int n, int16_t *out);

#endif
