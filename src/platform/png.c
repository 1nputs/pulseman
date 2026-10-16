/* A PNG in memory (mdrt/png.h): signature, IHDR, one IDAT of stored deflate blocks, IEND. */
#include <string.h>
#include "mdrt/png.h"

static uint32_t crc_step(uint32_t c, const uint8_t *p, size_t n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* a chunk at out: length, type, the data already in place after them, CRC */
static size_t chunk(uint8_t *out, const char *type, size_t len)
{
    be32(out, (uint32_t)len);
    memcpy(out + 4, type, 4);
    be32(out + 8 + len, crc_step(0, out + 4, 4 + len));
    return 12 + len;
}

size_t mdrt_png_encode(const uint32_t *rgb, int width, int height, int pitch, uint8_t *out, size_t size)
{
    if (width <= 0 || height <= 0 || pitch < width) return 0;
    size_t raw = (size_t)height * (1 + (size_t)width * 3);     /* each row: filter 0, then RGB */
    size_t blocks = (raw + 65534) / 65535;
    size_t z = 2 + raw + 5 * blocks + 4;                        /* zlib header, stored blocks, Adler-32 */
    size_t total = 8 + (12 + 13) + (12 + z) + 12;
    if (!out || size < total) return total;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t *p = out;
    memcpy(p, sig, 8); p += 8;
    be32(p + 8, (uint32_t)width); be32(p + 12, (uint32_t)height);
    p[16] = 8; p[17] = 2; p[18] = 0; p[19] = 0; p[20] = 0;     /* 8 bits, RGB, deflate, no filter choice, no interlace */
    p += chunk(p, "IHDR", 13);
    uint8_t *d = p + 8;
    *d++ = 0x78; *d++ = 0x01;
    uint32_t a = 1, b = 0;
    size_t left = raw, in_block = 0;
    for (int y = 0; y < height; y++)
        for (int x = -1; x < width; x++) {
            uint8_t px[3];
            int n = 1;
            if (x < 0) px[0] = 0;                               /* the row's filter byte */
            else { uint32_t c = rgb[(size_t)y * (size_t)pitch + (size_t)x]; px[0] = (uint8_t)(c >> 16); px[1] = (uint8_t)(c >> 8); px[2] = (uint8_t)c; n = 3; }
            for (int i = 0; i < n; i++) {
                if (!in_block) {                                /* a stored block's header */
                    size_t len = left > 65535 ? 65535 : left;
                    *d++ = (uint8_t)(len == left);
                    *d++ = (uint8_t)len; *d++ = (uint8_t)(len >> 8); *d++ = (uint8_t)~len; *d++ = (uint8_t)(~len >> 8);
                    in_block = len;
                }
                *d++ = px[i];
                a = (a + px[i]) % 65521;
                b = (b + a) % 65521;
                in_block--;
                left--;
            }
        }
    be32(d, b << 16 | a);
    p += chunk(p, "IDAT", z);
    p += chunk(p, "IEND", 0);
    return (size_t)(p - out);
}
