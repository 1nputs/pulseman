/* The YM2612 behind mdrt/fm.h. The core runs an internal clock at a time (core.h); a sample is the
 * sum of the core's output over 24 clocks: the chip time-multiplexes its six channels through one
 * DAC, so the sum is the mix. */
#include <stdlib.h>
#include <string.h>
#include "mdrt/fm.h"
#include "core.h"
#include "../state.h"

struct mdrt_fm {
    fm_core *core;
    uint32_t mclk;              /* master clocks toward the next internal clock */
    uint32_t owed;              /* master clocks of internal clocks run between writes, ahead of time */
    int cycle;                  /* internal clocks into the sample */
    int32_t left, right;        /* the sample's sum so far */
    int written;                /* a write the core has not had a clock for yet */
    int16_t spare[2];           /* a sample a clock between two writes completed, for the next run */
    int spare_full;
    uint8_t addr[2];
    uint8_t regs[2][256];
};

/* output scale: a sample sums six channel slots of +-768 (9 bits x 3) and the DAC ladder of the
 * other 18 clocks; x 6 keeps the loudest inside 16 bits */
#define SCALE 6

mdrt_fm *mdrt_fm_new(void)
{
    mdrt_fm *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->core = fm_core_new();
    if (!f->core) { free(f); return NULL; }
    return f;
}

void mdrt_fm_free(mdrt_fm *f)
{
    if (!f) return;
    fm_core_free(f->core);
    free(f);
}

void mdrt_fm_reset(mdrt_fm *f)
{
    fm_core_reset(f->core);
    f->cycle = 0;
    f->left = f->right = 0;
    f->written = 0;
    f->spare_full = 0;
    memset(f->addr, 0, sizeof f->addr);
    memset(f->regs, 0, sizeof f->regs);
}

/* one internal clock, into the sample being summed; 1 when that completes it */
static int clock_once(mdrt_fm *f, int16_t *out)
{
    int32_t lr[2];
    fm_core_clock(f->core, lr);
    f->left += lr[0];
    f->right += lr[1];
    f->written = 0;
    if (++f->cycle < 24) return 0;
    int32_t l = f->left * SCALE, r = f->right * SCALE;
    out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
    out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    f->cycle = 0;
    f->left = f->right = 0;
    return 1;
}

void mdrt_fm_write(mdrt_fm *f, int port, uint8_t value)
{
    port &= 3;
    if (f->written) {                                     /* the last write needs a clock first */
        if (clock_once(f, f->spare)) f->spare_full = 1;
        f->owed += MDRT_FM_CYCLE_MCLKS;
    }
    fm_core_write(f->core, port, value);
    f->written = 1;
    if (port & 1) f->regs[port >> 1][f->addr[port >> 1]] = value;
    else f->addr[port >> 1] = value;
}

int mdrt_fm_run(mdrt_fm *f, uint32_t mclk, int16_t *out, int max)
{
    int n = 0;
    if (f->spare_full && n < max) { out[0] = f->spare[0]; out[1] = f->spare[1]; n++; f->spare_full = 0; }
    if (f->owed >= mclk) { f->owed -= mclk; return n; }
    mclk -= f->owed;
    f->owed = 0;
    f->mclk += mclk;
    while (f->mclk >= MDRT_FM_CYCLE_MCLKS) {
        f->mclk -= MDRT_FM_CYCLE_MCLKS;
        int16_t s[2];
        if (clock_once(f, s) && n < max) { out[2 * n] = s[0]; out[2 * n + 1] = s[1]; n++; }
    }
    return n;
}

uint8_t mdrt_fm_reg(const mdrt_fm *f, int part, uint8_t reg) { return f->regs[part & 1][reg]; }

const char *mdrt_fm_core(void) { return fm_core_name(); }

/* the wrapper's fields, then the core's name (8 bytes) and its own state */
static void save_wrapper(const mdrt_fm *f, st_out *o)
{
    st_u32(o, f->mclk); st_u32(o, f->owed);
    st_u32(o, (uint32_t)f->cycle);
    st_u32(o, (uint32_t)f->left); st_u32(o, (uint32_t)f->right);
    st_u8(o, (uint8_t)f->written);
    st_u16(o, (uint16_t)f->spare[0]); st_u16(o, (uint16_t)f->spare[1]);
    st_u8(o, (uint8_t)f->spare_full);
    st_put(o, f->addr, 2);
    st_put(o, f->regs, sizeof f->regs);
    char name[8] = { 0 };
    strncpy(name, fm_core_name(), sizeof name - 1);
    st_put(o, name, 8);
}

size_t mdrt_fm_save(const mdrt_fm *f, uint8_t *out)
{
    st_out o = { out, 0 };
    save_wrapper(f, &o);
    return o.n + fm_core_save(f->core, out ? out + o.n : NULL);
}

int mdrt_fm_load(mdrt_fm *f, const uint8_t *in, size_t size)
{
    st_out o = { NULL, 0 };
    save_wrapper(f, &o);
    char name[8] = { 0 };
    strncpy(name, fm_core_name(), sizeof name - 1);
    if (size < o.n || memcmp(in + o.n - 8, name, 8) || fm_core_load(f->core, in + o.n, size - o.n)) return -1;
    st_in i = { in, 0, o.n, 0 };
    f->mclk = st_g32(&i); f->owed = st_g32(&i);
    f->cycle = (int)st_g32(&i);
    f->left = (int32_t)st_g32(&i); f->right = (int32_t)st_g32(&i);
    f->written = st_g8(&i);
    f->spare[0] = (int16_t)st_g16(&i); f->spare[1] = (int16_t)st_g16(&i);
    f->spare_full = st_g8(&i);
    st_get(&i, f->addr, 2);
    st_get(&i, f->regs, sizeof f->regs);
    return 0;
}
