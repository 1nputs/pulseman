/* Cartridge save RAM: bytes on the odd addresses of $200000 up (the usual 8-bit SRAM wiring).
 * The platform keeps it (a file, IndexedDB) through mdrt_sram_bytes and the dirty flag. */
#ifndef MDRT_SRAM_H
#define MDRT_SRAM_H
#include <stdint.h>
#include <stddef.h>

#define MDRT_SRAM_MAX 0x10000
typedef struct mdrt_sram {
    uint8_t bytes[MDRT_SRAM_MAX];
    size_t size;                /* bytes of save RAM */
    int dirty;                  /* written since the platform last saved */
} mdrt_sram;

void mdrt_sram_init(mdrt_sram *s, size_t size);
/* a byte access at bus offset off from $200000: odd offsets reach byte off / 2; even offsets read
 * $FF and ignore writes */
uint8_t mdrt_sram_read8(const mdrt_sram *s, uint32_t off);
void mdrt_sram_write8(mdrt_sram *s, uint32_t off, uint8_t v);
/* the save RAM's bytes (size of them): save returns the size (out NULL: only that); load returns
 * 0, or -1 for the wrong size (then nothing changed). A load that changes them marks them dirty. */
size_t mdrt_sram_save(const mdrt_sram *s, uint8_t *out);
int mdrt_sram_load(mdrt_sram *s, const uint8_t *in, size_t size);

#endif
