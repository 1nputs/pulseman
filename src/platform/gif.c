/* GIF (mdrt/gif.h). */
#include <string.h>
#include "mdrt/gif.h"

/* ---- the palette ----------------------------------------------------------------------------- */

static int key(uint16_t c) { return (c >> 1 & 7) | (c >> 5 & 7) << 3 | (c >> 9 & 7) << 6; }   /* (9 bits: B G R) */

int mdrt_gif_palette(const uint16_t *px, int w, int h, int pitch, uint8_t *idx, uint16_t pal[256])
{
    int16_t at[512];                                        /* each colour's index, -1 not yet */
    memset(at, 0xFF, sizeof at);
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t c = px[y * pitch + x];
            int k = key(c);
            if (at[k] < 0) {
                if (n < 256) { at[k] = (int16_t)n; pal[n++] = (uint16_t)(c & 0x0EEE); }
                else {                                      /* (a 257th colour: the nearest there is) */
                    int best = 0, bd = 1 << 30;
                    for (int i = 0; i < 256; i++) {
                        int p = key(pal[i]), dr = (p & 7) - (k & 7), dg = (p >> 3 & 7) - (k >> 3 & 7), db = (p >> 6) - (k >> 6);
                        int d = dr * dr + dg * dg + db * db;
                        if (d < bd) { bd = d; best = i; }
                    }
                    at[k] = (int16_t)best;
                }
            }
            *idx++ = (uint8_t)at[k];
        }
    return n;
}

int mdrt_gif_bits(int colours)
{
    int b = 2;
    while (b < 8 && (1 << b) < colours) b++;
    return b;
}

/* ---- LZW: codes of 3 to 12 bits, least significant bit first --------------------------------- */

typedef struct bitout { uint8_t *out; size_t cap, n; uint32_t acc; int bits, full; } bitout;

static void put_code(bitout *o, unsigned code, int size)
{
    o->acc |= (uint32_t)code << o->bits;
    o->bits += size;
    while (o->bits >= 8) {
        if (o->n < o->cap) o->out[o->n++] = (uint8_t)o->acc;
        else o->full = 1;
        o->acc >>= 8;
        o->bits -= 8;
    }
}

size_t mdrt_gif_lzw_bound(size_t n) { return (n + 2) * 12 / 8 + 16; }   /* (a code a pixel at most, and clears) */

/* the dictionary: string (prefix code, next index) -> code, hashed; codes from clear + 2 up to 4095,
 * then a clear code and a fresh start. The code size grows when the next code to be made needs
 * another bit, as the reader's does a code later (GIF has no "early change"). */
size_t mdrt_gif_lzw(const uint8_t *idx, size_t n, int bits, uint8_t *out, size_t cap)
{
    enum { SLOTS = 8192 };
    static _Thread_local uint32_t keys[SLOTS];              /* prefix << 8 | index, + 1 (0 empty) */
    static _Thread_local uint16_t codes[SLOTS];
    if (bits < 2) bits = 2;
    if (bits > 8) bits = 8;
    unsigned clear = 1u << bits, next = clear + 2;
    int size = bits + 1;
    bitout o = { out, cap, 0, 0, 0, 0 };
    memset(keys, 0, sizeof keys);
    put_code(&o, clear, size);
    if (n) {
        unsigned w = idx[0] & (clear - 1);
        for (size_t i = 1; i < n; i++) {
            unsigned k = idx[i] & (clear - 1);
            uint32_t want = (w << 8 | k) + 1, h = (want * 2654435761u) >> 19;   /* (13 bits) */
            while (keys[h] && keys[h] != want) h = (h + 1) & (SLOTS - 1);
            if (keys[h]) { w = codes[h]; continue; }
            put_code(&o, w, size);
            keys[h] = want;
            codes[h] = (uint16_t)next++;
            if (next - 1 == 1u << size && size < 12) size++;
            if (next == 4096) {                             /* the dictionary full: a fresh start */
                put_code(&o, clear, size);
                memset(keys, 0, sizeof keys);
                next = clear + 2;
                size = bits + 1;
            }
            w = k;
        }
        put_code(&o, w, size);
        if (next == 1u << size && size < 12) size++;        /* (the reader makes its last code here) */
    }
    put_code(&o, clear + 1, size);                          /* the end */
    if (o.bits) put_code(&o, 0, 8 - o.bits);
    return o.full ? 0 : o.n;
}

int mdrt_gif_unlzw(const uint8_t *in, size_t len, int bits, uint8_t *idx, size_t n)
{
    static _Thread_local uint16_t prefix[4096];
    static _Thread_local uint8_t suffix[4096], first[4096], stack[4096];
    if (bits < 2 || bits > 8) return 0;
    unsigned clear = 1u << bits, next = clear + 2, prev = 4096;
    int size = bits + 1;
    for (unsigned c = 0; c < clear; c++) { suffix[c] = first[c] = (uint8_t)c; prefix[c] = 4096; }
    size_t got = 0, pos = 0;
    uint32_t acc = 0;
    int have = 0;
    for (;;) {
        while (have < size) {
            if (pos >= len) return 0;
            acc |= (uint32_t)in[pos++] << have;
            have += 8;
        }
        unsigned code = acc & ((1u << size) - 1);
        acc >>= size;
        have -= size;
        if (code == clear) { next = clear + 2; size = bits + 1; prev = 4096; continue; }
        if (code == clear + 1) return got == n;
        unsigned c = code, sp = 0;
        if (code > next || (code == next && prev == 4096)) return 0;
        if (code == next) { stack[sp++] = first[prev]; c = prev; }   /* (the string just being made) */
        while (c >= clear) { stack[sp++] = suffix[c]; c = prefix[c]; if (sp >= 4096) return 0; }
        stack[sp++] = (uint8_t)c;
        if (prev != 4096 && next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = (uint8_t)c;
            first[next] = first[prev];
            next++;
            if (next == 1u << size && size < 12) size++;
        }
        while (sp) { if (got >= n) return 0; idx[got++] = stack[--sp]; }
        prev = code;
    }
}

/* ---- the file -------------------------------------------------------------------------------- */

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void mdrt_gif_begin(mdrt_gif_put put, void *user, int w, int h)
{
    uint8_t b[32] = "GIF89a";
    put16(b + 6, w);
    put16(b + 8, h);
    b[10] = 0x00;                                           /* (no global colour table) */
    b[11] = 0;
    b[12] = 0;
    put(user, b, 13);
    static const uint8_t loop[19] = { 0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0 };
    put(user, loop, sizeof loop);                           /* (Netscape's: loop for ever) */
}

void mdrt_gif_frame(mdrt_gif_put put, void *user, int x, int y, int w, int h, int delay,
                    const uint32_t *rgb, int colours, int bits, const uint8_t *code, size_t len)
{
    uint8_t b[20] = { 0x21, 0xF9, 4, 0x04, 0, 0, 0, 0 };    /* graphic control: kept under the next */
    put16(b + 4, delay);
    put(user, b, 8);
    b[0] = 0x2C;                                            /* the image, with a colour table of its own */
    put16(b + 1, x);
    put16(b + 3, y);
    put16(b + 5, w);
    put16(b + 7, h);
    int tb = 1;
    while (tb < 8 && (1 << tb) < colours) tb++;
    b[9] = (uint8_t)(0x80 | (tb - 1));
    put(user, b, 10);
    uint8_t table[768] = { 0 };
    for (int i = 0; i < colours && i < 256; i++) {
        table[3 * i] = (uint8_t)(rgb[i] >> 16);
        table[3 * i + 1] = (uint8_t)(rgb[i] >> 8);
        table[3 * i + 2] = (uint8_t)rgb[i];
    }
    put(user, table, (size_t)3 << tb);
    b[0] = (uint8_t)bits;
    put(user, b, 1);
    while (len) {                                           /* the code stream in blocks of 255 */
        uint8_t n = (uint8_t)(len > 255 ? 255 : len);
        put(user, &n, 1);
        put(user, code, n);
        code += n;
        len -= n;
    }
    b[0] = 0;
    put(user, b, 1);
}

void mdrt_gif_end(mdrt_gif_put put, void *user)
{
    static const uint8_t end = 0x3B;
    put(user, &end, 1);
}
