/* A PNG in memory (mdrt/png.h): signature, IHDR, one IDAT of stored deflate blocks, IEND; and the
 * same read back (only PNGs made so: a screenshot or a save slot's picture). */
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

static uint32_t be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int mdrt_png_decode(const uint8_t *png, size_t size, uint32_t *out, int max_w, int max_h, int *w, int *h)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (size < 8 + 25 || memcmp(png, sig, 8) || memcmp(png + 12, "IHDR", 4)) return -1;
    uint32_t pw = be(png + 16), ph = be(png + 20);
    if (!pw || !ph || pw > (uint32_t)max_w || ph > (uint32_t)max_h || png[24] != 8 || png[25] != 2 || png[28] != 0) return -1;
    size_t o = 33;
    while (o + 12 <= size && memcmp(png + o + 4, "IDAT", 4)) o += 12 + be(png + o);   /* (to the IDAT) */
    if (o + 12 > size) return -1;
    size_t len = be(png + o), at = o + 8 + 2, end = o + 8 + len;
    if (end > size || len < 2) return -1;
    size_t row = 1 + (size_t)pw * 3, x = 0, y = 0, k = 0;
    uint8_t px[3];
    int last = 0;
    while (!last && at + 5 <= end) {                      /* stored blocks only */
        if (png[at] >> 1) return -1;
        last = png[at] & 1;
        size_t n = (size_t)(png[at + 1] | png[at + 2] << 8);
        at += 5;
        if (at + n > end) return -1;
        for (size_t i = 0; i < n; i++, k++) {
            size_t c = k % row;
            uint8_t b = png[at + i];
            if (c == 0) { if (b) return -1; continue; }   /* the row's filter: none */
            px[(c - 1) % 3] = b;
            if ((c - 1) % 3 == 2) {
                y = k / row;
                x = (c - 1) / 3;
                if (y < ph) out[y * (size_t)max_w + x] = 0xFF000000u | (uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2];
            }
        }
        at += n;
    }
    if (k != row * ph) return -1;
    *w = (int)pw;
    *h = (int)ph;
    return 0;
}
