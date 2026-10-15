// The FM core: ymfm's YM2612 (third_party/ymfm, BSD-3), for `make FM_CORE=ymfm`. ymfm makes a
// whole sample at a time, so the clock that ends a sample brings all of it and the other 23 bring
// nothing; its level is brought to Nuked-OPN2's (fm.c sums Nuked's 24 clocks).
#include <new>
#include "core.h"
#include "ymfm_opn.h"

namespace {
const int SCALE_NUM = 1, SCALE_DEN = 7;     // ymfm's 16-bit output to the sum of Nuked's clocks (equal loudness)
}

struct fm_core {
    ymfm::ymfm_interface intf;
    ymfm::ym2612 chip;
    int cycle;
    fm_core() : chip(intf), cycle(0) {}
};

extern "C" {

fm_core *fm_core_new(void)
{
    fm_core *c = new (std::nothrow) fm_core();
    if (c) c->chip.reset();
    return c;
}

void fm_core_free(fm_core *c) { delete c; }
void fm_core_reset(fm_core *c) { c->chip.reset(); c->cycle = 0; }
void fm_core_write(fm_core *c, int port, uint8_t value) { c->chip.write((uint32_t)port, value); }

void fm_core_clock(fm_core *c, int32_t out[2])
{
    out[0] = out[1] = 0;
    if (++c->cycle < 24) return;
    c->cycle = 0;
    ymfm::ym2612::output_data o;
    c->chip.generate(&o, 1);
    out[0] = o.data[0] * SCALE_NUM / SCALE_DEN;
    out[1] = o.data[1] * SCALE_NUM / SCALE_DEN;
}

const char *fm_core_name(void) { return "ymfm"; }

}
