// The FM core: ymfm's YM2612 or YM3438 (third_party/ymfm, BSD-3). ymfm makes a whole sample at a
// time, so the clock that ends a sample brings all of it and the other 23 bring nothing; its level
// is brought to Nuked-OPN2's (fm.c sums Nuked's 24 clocks).
#include <cstring>
#include <new>
#include <vector>
#include "mdrt/fm.h"
#include "core.h"
#include "ymfm_opn.h"

namespace {
const int SCALE_NUM = 1, SCALE_DEN = 7;     // ymfm's 16-bit output to the sum of Nuked's clocks (equal loudness)
}

struct fm_core {
    ymfm::ymfm_interface intf;
    ymfm::ym3438 chip;                      // (a ym2612 that generates as the YM3438 does: used when model is set)
    int model;
    int cycle;
    fm_core(int m) : chip(intf), model(m), cycle(0) {}
};

static fm_core *create(int model)
{
    fm_core *c = new (std::nothrow) fm_core(model == MDRT_FM_YM3438);
    if (c) c->chip.reset();
    return c;
}

static void destroy(fm_core *c) { delete c; }
static void reset(fm_core *c) { c->chip.reset(); c->cycle = 0; }
static void write(fm_core *c, int port, uint8_t value) { c->chip.write((uint32_t)port, value); }

static void clock(fm_core *c, int32_t out[2])
{
    out[0] = out[1] = 0;
    if (++c->cycle < 24) return;
    c->cycle = 0;
    ymfm::ym2612::output_data o;
    if (c->model) c->chip.generate(&o, 1);
    else static_cast<ymfm::ym2612 &>(c->chip).generate(&o, 1);
    out[0] = o.data[0] * SCALE_NUM / SCALE_DEN;
    out[1] = o.data[1] * SCALE_NUM / SCALE_DEN;
}

// ymfm's own save format (ymfm_saved_state: the chip's fields one after another), after the
// clock count into the sample and the model
static size_t save(fm_core *c, uint8_t *out)
{
    std::vector<uint8_t> buf;
    ymfm::ymfm_saved_state st(buf, true);
    c->chip.save_restore(st);
    if (out) {
        for (int i = 0; i < 4; i++) out[i] = uint8_t(uint32_t(c->cycle) >> (8 * i));
        out[4] = uint8_t(c->model);
        std::memcpy(out + 5, buf.data(), buf.size());
    }
    return buf.size() + 5;
}

static int load(fm_core *c, const uint8_t *in, size_t size)
{
    if (size != save(c, nullptr) || in[4] != c->model) return -1;
    std::vector<uint8_t> buf(in + 5, in + size);
    ymfm::ymfm_saved_state st(buf, false);
    c->chip.save_restore(st);
    c->cycle = int(uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24);
    return 0;
}

extern "C" const fm_core_ops fm_core_ymfm = { "ymfm", create, destroy, reset, write, clock, save, load };
