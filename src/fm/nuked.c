/* The FM core: Nuked-OPN2 (third_party/nuked-opn2), in its YM2612 mode (the discrete chip of the
 * first Mega Drives: the DAC's ladder effect, status on port 0 only). */
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "../../third_party/nuked-opn2/ym3438.h"

struct fm_core { ym3438_t chip; };

fm_core *fm_core_new(void)
{
    fm_core *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    OPN2_SetChipType(&c->chip, ym3438_mode_ym2612);
    OPN2_Reset(&c->chip);
    return c;
}

void fm_core_free(fm_core *c) { free(c); }
void fm_core_reset(fm_core *c) { OPN2_Reset(&c->chip); }
void fm_core_write(fm_core *c, int port, uint8_t value) { OPN2_Write(&c->chip, (Bit32u)port, value); }

void fm_core_clock(fm_core *c, int32_t out[2])
{
    Bit16s buf[2];
    OPN2_Clock(&c->chip, buf);
    out[0] = buf[0];
    out[1] = buf[1];
}

const char *fm_core_name(void) { return "nuked"; }

/* the chip's struct as it is: fixed-width fields only (ym3438.h), so the same bytes on every
 * little-endian host with the usual alignment */
size_t fm_core_save(fm_core *c, uint8_t *out)
{
    if (out) memcpy(out, &c->chip, sizeof c->chip);
    return sizeof c->chip;
}

int fm_core_load(fm_core *c, const uint8_t *in, size_t size)
{
    if (size != sizeof c->chip) return -1;
    memcpy(&c->chip, in, size);
    return 0;
}
