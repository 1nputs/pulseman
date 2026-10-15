#include <stdio.h>
#include <stdlib.h>
#include "png.h"

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)type[i];
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(0, h + 4, 4);
    c = crc(c, data, len);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

int png_write(const char *path, const uint32_t *rgb, int width, int height)
{
    crc_init();
    size_t raw_len = (size_t)height * (1 + (size_t)width * 3);
    uint8_t *raw = malloc(raw_len);
    size_t blocks = raw_len / 65535 + 1;
    uint8_t *z = malloc(raw_len + blocks * 5 + 6);
    if (!raw || !z) { free(raw); free(z); return -1; }
    uint8_t *p = raw;
    for (int y = 0; y < height; y++) {
        *p++ = 0;
        for (int x = 0; x < width; x++) {
            uint32_t c = rgb[y * width + x];
            *p++ = (uint8_t)(c >> 16); *p++ = (uint8_t)(c >> 8); *p++ = (uint8_t)c;
        }
    }
    /* zlib: header, stored blocks, adler32 */
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw_len || at == 0; ) {
        size_t n = raw_len - at > 65535 ? 65535 : raw_len - at;
        z[o++] = (uint8_t)(at + n == raw_len);
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        for (size_t i = 0; i < n; i++) z[o++] = raw[at + i];
        at += n;
        if (!n) break;
    }
    be32(z + o, b << 16 | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)width); be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 0;
}
