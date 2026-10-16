#include <string.h>
#include "mdrt/sram.h"

void mdrt_sram_init(mdrt_sram *s, size_t size)
{
    memset(s->bytes, 0xFF, sizeof s->bytes);
    s->size = size > MDRT_SRAM_MAX ? MDRT_SRAM_MAX : size;
    s->dirty = 0;
}

uint8_t mdrt_sram_read8(const mdrt_sram *s, uint32_t off)
{
    if (!(off & 1) || (off >> 1) >= s->size) return 0xFF;
    return s->bytes[off >> 1];
}

void mdrt_sram_write8(mdrt_sram *s, uint32_t off, uint8_t v)
{
    if (!(off & 1) || (off >> 1) >= s->size) return;
    if (s->bytes[off >> 1] != v) s->dirty = 1;
    s->bytes[off >> 1] = v;
}

size_t mdrt_sram_save(const mdrt_sram *s, uint8_t *out)
{
    if (out) memcpy(out, s->bytes, s->size);
    return s->size;
}

int mdrt_sram_load(mdrt_sram *s, const uint8_t *in, size_t size)
{
    if (size != s->size) return -1;
    if (memcmp(s->bytes, in, size)) s->dirty = 1;
    memcpy(s->bytes, in, size);
    return 0;
}
