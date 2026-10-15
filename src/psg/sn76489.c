/* The SN76489 variant in the Mega Drive's VDP (SMS Power, "SN76489"; Sega's manual):
 * - registers: a latch byte 1 cc t dddd chooses channel cc and its tone / noise (t = 0) or
 *   volume (t = 1) register and writes dddd to its low bits; a data byte 0 - dddddd writes the
 *   latched register: the high 6 bits of a 10-bit tone period, or the low bits of a volume or the
 *   noise register. The latch is never cleared by a data byte. Writing the noise register (either
 *   way) resets the shift register.
 * - an internal clock is the input clock (master / 15) / 16. A tone counter counts down from its
 *   period; at the end it reloads and its output flips: a square wave of period 2 x period
 *   internal clocks. Periods 0 and 1 give a constant +1 (games play samples through the volume).
 * - the noise counter reloads with $10, $20, $40 (rate 0-2) or tone 2's period (rate 3); its
 *   flip-flop's rising edge shifts the 16-bit register right, the bit shifted out is the output,
 *   and the bit shifted in is bit 0 (periodic noise, register bit 2 = 0) or bit 0 XOR bit 3
 *   (white noise; Sega's taps). The reset value is $8000.
 * - attenuation: 2 dB a step, 15 off.
 * Outputs are bipolar (+-level): the hardware's single-ended output goes through a capacitor, so
 * only the swing reaches the speaker. Changes of the output go into the samples as band-limited
 * steps (kernel.h): a low-pass at 21 kHz, so the square waves' harmonics above the output's
 * Nyquist frequency do not fold back. */
#include <string.h>
#include "mdrt/psg.h"
#include "kernel.h"

/* MDRT_PSG_MAX x 10^(-2n / 20), n = 0..14; 15 is off */
static const int16_t level[16] = {
    6144, 4880, 3877, 3079, 2446, 1943, 1543, 1226, 974, 773, 614, 488, 388, 308, 245, 0
};

/* the output changes by delta at master clock t of sample s (t < MDRT_PSG_SAMPLE_MCLKS): the step
 * spread over samples s - 15 .. s + 16, which come out MDRT_PSG_LATENCY samples late */
static void add_step(mdrt_psg *p, int32_t delta, uint32_t s, int t)
{
    const int16_t *k = psg_kernel[t / (MDRT_PSG_SAMPLE_MCLKS / 21)];
    for (int j = 0; j < 32; j++) p->pending[(s + (uint32_t)j + 1) & 63] += delta * k[j];
}

void mdrt_psg_init(mdrt_psg *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 1; i < 8; i += 2) p->reg[i] = 15;
    p->lfsr = 0x8000;
}

static int32_t output(const mdrt_psg *p);

void mdrt_psg_write(mdrt_psg *p, uint8_t v)
{
    int r;
    if (v & 0x80) {                                       /* latch: register and its low 4 bits */
        r = p->latch = (uint8_t)((v >> 4) & 7);
        if (r == 0 || r == 2 || r == 4) p->reg[r] = (uint16_t)((p->reg[r] & 0x3F0) | (v & 0x0F));
        else p->reg[r] = v & (r == 6 ? 0x07 : 0x0F);
    } else {                                              /* data: the latched register */
        r = p->latch;
        if (r == 0 || r == 2 || r == 4) p->reg[r] = (uint16_t)((p->reg[r] & 0x00F) | (v & 0x3F) << 4);
        else p->reg[r] = v & (r == 6 ? 0x07 : 0x0F);
    }
    if (r == 6) p->lfsr = 0x8000;
    int32_t now = output(p);                              /* a volume changed: from the next sample */
    if (now != p->level) { add_step(p, now - p->level, p->sample, 0); p->level = now; }
}

/* the counter of channel i reloads: its period in internal clocks */
static uint16_t period(const mdrt_psg *p, int i)
{
    if (i < 3) return p->reg[i * 2] & 0x3FF;
    int rate = p->reg[6] & 3;
    return rate == 3 ? (p->reg[4] & 0x3FF) : (uint16_t)(0x10 << rate);
}

/* one internal clock */
static void step(mdrt_psg *p)
{
    for (int i = 0; i < 4; i++) {
        uint16_t n = period(p, i);
        if (i < 3 && n <= 1) { p->flip[i] = 1; p->count[i] = n; continue; }   /* constant +1 */
        if (p->count[i] > 1) { p->count[i]--; continue; }
        p->count[i] = n ? n : 1;
        p->flip[i] ^= 1;
        if (i == 3 && p->flip[3]) {                       /* the noise flip-flop's rising edge */
            unsigned in = (p->reg[6] & 4) ? ((p->lfsr ^ p->lfsr >> 3) & 1) : (p->lfsr & 1);
            p->noise_out = p->lfsr & 1;
            p->lfsr = (uint16_t)(p->lfsr >> 1 | in << 15);
        }
    }
}

/* the output now */
static int32_t output(const mdrt_psg *p)
{
    int32_t s = 0;
    for (int i = 0; i < 3; i++) s += p->flip[i] ? level[p->reg[i * 2 + 1]] : -level[p->reg[i * 2 + 1]];
    s += p->noise_out ? level[p->reg[7]] : -level[p->reg[7]];
    return s;
}

void mdrt_psg_render(mdrt_psg *p, int16_t *out, int n)
{
    for (int k = 0; k < n; k++) {
        int t = 0;                                        /* master clocks into this sample */
        for (;;) {
            int take = MDRT_PSG_STEP_MCLKS - p->phase;
            if (t + take >= MDRT_PSG_SAMPLE_MCLKS) {      /* the next internal clock is in a later sample */
                p->phase = (uint16_t)(p->phase + MDRT_PSG_SAMPLE_MCLKS - t);
                if (p->phase == MDRT_PSG_STEP_MCLKS) p->phase = 0, t = -1;   /* (at the next sample's start) */
                break;
            }
            t += take;
            p->phase = 0;
            step(p);
            int32_t now = output(p);
            if (now != p->level) { add_step(p, now - p->level, p->sample, t); p->level = now; }
        }
        uint32_t s = p->sample & 63;                      /* this sample's steps, all in by now */
        p->sum += p->pending[s];
        p->pending[s] = 0;
        int32_t v = (int32_t)(p->sum >> 15);
        out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        p->sample++;
        if (t < 0) {                                      /* the internal clock that ends with this sample */
            step(p);
            int32_t now = output(p);
            if (now != p->level) { add_step(p, now - p->level, p->sample, 0); p->level = now; }
        }
    }
}
