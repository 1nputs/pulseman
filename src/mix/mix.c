/* The mixer (mdrt/mix.h): sum, DC blocker, polyphase resampler (kernel.h, 256 positions between
 * two input samples, 32 taps). */
#include <string.h>
#include "mdrt/mix.h"
#include "mdrt/mdrt.h"
#include "kernel.h"

#define IN_MCLKS 1008u                                    /* master clocks an input sample */
/* the DC blocker: y = x - x' + a y', a = 1 - 2 pi 10 Hz / 53267 Hz in 1/65536ths (a 10 Hz high-pass) */
#define DC_A 65459

void mdrt_mix_init(mdrt_mix *m, uint32_t rate)
{
    memset(m, 0, sizeof *m);
    m->rate = rate;
    m->clock = MDRT_MASTER_CLOCK_NTSC;
    m->fm_gain = MDRT_MIX_FM_GAIN;
    m->psg_gain = MDRT_MIX_PSG_GAIN;
}

void mdrt_mix_set_clock(mdrt_mix *m, uint32_t master_clock) { m->clock = master_clock; }
void mdrt_mix_set_gains(mdrt_mix *m, int32_t fm_gain, int32_t psg_gain) { m->fm_gain = fm_gain; m->psg_gain = psg_gain; }

int mdrt_mix_run(mdrt_mix *m, const int16_t *fm, const int16_t *psg, int n, int16_t *out)
{
    int made = 0;
    const uint64_t span = (uint64_t)IN_MCLKS * m->rate;   /* an input sample, in master clocks x rate */
    for (int i = 0; i < n; i++) {
        int64_t p = psg ? (int64_t)psg[i] * m->psg_gain : 0;
        for (int c = 0; c < 2; c++) {
            int32_t x = (int32_t)(((fm ? (int64_t)fm[2 * i + c] * m->fm_gain : 0) + p) / 32768);
            m->dc_y[c] = (int64_t)(x - m->dc_x[c]) * 65536 + ((m->dc_y[c] * DC_A) >> 16);
            m->dc_x[c] = x;
            m->hist[c][m->in & 63] = (int32_t)(m->dc_y[c] >> 16);
        }
        m->in++;
        for (;;) {                                        /* the outputs the inputs now reach */
            uint64_t t = m->out * (uint64_t)m->clock;
            uint64_t j0 = t / span;
            if (j0 + 16 >= m->in) break;
            const int16_t *k = mix_kernel[(t % span) * 256 / span];
            for (int c = 0; c < 2; c++) {
                int64_t acc = 0;
                for (int j = 0; j < 32; j++) acc += (int64_t)m->hist[c][(j0 - 15 + (uint64_t)j) & 63] * k[j];
                int64_t v = acc >> 15;
                out[2 * made + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            m->out++;
            made++;
        }
    }
    return made;
}
