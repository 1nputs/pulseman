/* The FM core: Nuked-OPN2 (third_party/nuked-opn2). The YM2612 model is its YM2612 mode (the
 * discrete chip of the first Mega Drives: the DAC's ladder effect, status on port 0 only); the
 * YM3438 model its YM3438 mode (the later integrated chip: no ladder, status on every port). */
#include <stdlib.h>
#include <string.h>
#include "mdrt/fm.h"
#include "core.h"
#include "../../third_party/nuked-opn2/ym3438.h"

struct fm_core { ym3438_t chip; };

static fm_core *create(int model)
{
    fm_core *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    OPN2_SetChipType(&c->chip, model == MDRT_FM_YM3438 ? ym3438_mode_readmode : ym3438_mode_ym2612);
    OPN2_Reset(&c->chip);
    return c;
}

static void destroy(fm_core *c) { free(c); }
static void reset(fm_core *c) { OPN2_Reset(&c->chip); }
static void write(fm_core *c, int port, uint8_t value) { OPN2_Write(&c->chip, (Bit32u)port, value); }

static void clock(fm_core *c, int32_t out[2])
{
    Bit16s buf[2];
    OPN2_Clock(&c->chip, buf);
    out[0] = buf[0];
    out[1] = buf[1];
}

/* the chip's struct as it is: fixed-width fields only (ym3438.h), so the same bytes on every
 * little-endian host with the usual alignment; the model is in it (chip_type) */
static size_t save(fm_core *c, uint8_t *out)
{
    if (out) memcpy(out, &c->chip, sizeof c->chip);
    return sizeof c->chip;
}

static int load(fm_core *c, const uint8_t *in, size_t size)
{
    ym3438_t chip;
    if (size != sizeof chip) return -1;
    memcpy(&chip, in, size);
    if (chip.chip_type != c->chip.chip_type) return -1;   /* (another model) */
    c->chip = chip;
    return 0;
}

const fm_core_ops fm_core_nuked = { "nuked", create, destroy, reset, write, clock, save, load };
