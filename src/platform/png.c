/* PNGs in memory (mdrt/png.h): a screenshot's (signature, IHDR, one IDAT of stored deflate blocks,
 * IEND), an indexed picture's (deflated), and any PNG read back, with deflate both ways written
 * here from RFC 1950, 1951 and the PNG specification. Everything read is checked: lengths, codes,
 * distances, sizes, CRCs and Adler-32s, so that a bad or hostile file only fails. */
#include <stdio.h>
#include <stdlib.h>
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

uint32_t mdrt_crc32(uint32_t crc, const void *p, size_t n) { return crc_step(crc, p, n); }

static uint32_t be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint32_t adler(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;                   /* (the most before the sums can overflow) */
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* ---- inflate ------------------------------------------------------------------------------------ */

struct inflate {
    const uint8_t *in;
    size_t n, at;                       /* (in bytes) */
    uint32_t bits;
    int count;                          /* bits held */
    uint8_t *out;
    size_t got, max;
};

/* need bits from the stream, the first in the lowest; -1 past its end */
static int take(struct inflate *z, int need)
{
    while (z->count < need) {
        if (z->at >= z->n) return -1;
        z->bits |= (uint32_t)z->in[z->at++] << z->count;
        z->count += 8;
    }
    int v = (int)(z->bits & ((1u << need) - 1));
    z->bits >>= need;
    z->count -= need;
    return v;
}

/* a canonical Huffman code: how many codes of each length, and the symbols in code order */
struct huffman { uint16_t count[16]; uint16_t symbol[320]; };

/* the code of n symbols' lengths: 0, or -1 when they are more than the codes allow (a code with
 * room left over is allowed: a stream with one distance code has one) */
static int code_of(struct huffman *h, const uint8_t *length, int n)
{
    uint16_t offset[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0) return -1;
    }
    offset[1] = 0;
    for (int len = 1; len < 15; len++) offset[len + 1] = (uint16_t)(offset[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offset[length[i]]++] = (uint16_t)i;
    return 0;
}

/* the next symbol: codes are read a bit at a time, the first bit the code's highest; -1 for none */
static int symbol(struct inflate *z, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = take(z, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                       131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                                        2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* a block's symbols to its end code: 0, -1 */
static int codes(struct inflate *z, const struct huffman *lit, const struct huffman *dist)
{
    for (;;) {
        int s = symbol(z, lit);
        if (s < 0) return -1;
        if (s < 256) {
            if (z->got >= z->max) return -1;
            z->out[z->got++] = (uint8_t)s;
            continue;
        }
        if (s == 256) return 0;
        s -= 257;
        if (s >= 29) return -1;
        int e = take(z, len_extra[s]);
        if (e < 0) return -1;
        size_t len = (size_t)len_base[s] + (size_t)e;
        int d = symbol(z, dist);
        if (d < 0 || d >= 30 || (e = take(z, dist_extra[d])) < 0) return -1;
        size_t back = (size_t)dist_base[d] + (size_t)e;
        if (back > z->got || len > z->max - z->got) return -1;
        for (size_t i = 0; i < len; i++, z->got++) z->out[z->got] = z->out[z->got - back];   /* (it may overlap itself) */
    }
}

static int fixed(struct inflate *z)
{
    static struct huffman lit, dist;
    static int made;
    if (!made) {
        uint8_t l[288];
        for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        code_of(&lit, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        code_of(&dist, l, 30);
        made = 1;
    }
    return codes(z, &lit, &dist);
}

static int dynamic(struct inflate *z)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    struct huffman lens, lit, dist;
    uint8_t length[320];
    int nlit = take(z, 5), ndist = take(z, 5), ncode = take(z, 4);
    if (nlit < 0 || ndist < 0 || ncode < 0) return -1;
    nlit += 257;
    ndist += 1;
    ncode += 4;
    if (nlit > 286 || ndist > 30) return -1;
    memset(length, 0, 19);
    for (int i = 0; i < ncode; i++) {
        int v = take(z, 3);
        if (v < 0) return -1;
        length[order[i]] = (uint8_t)v;
    }
    if (code_of(&lens, length, 19) < 0) return -1;
    for (int i = 0; i < nlit + ndist;) {
        int s = symbol(z, &lens), rep, v;
        if (s < 0) return -1;
        if (s < 16) { length[i++] = (uint8_t)s; continue; }
        if (s == 16) { if (!i || (rep = take(z, 2)) < 0) return -1; v = length[i - 1]; rep += 3; }
        else if (s == 17) { if ((rep = take(z, 3)) < 0) return -1; v = 0; rep += 3; }
        else { if ((rep = take(z, 7)) < 0) return -1; v = 0; rep += 11; }
        if (i + rep > nlit + ndist) return -1;
        while (rep--) length[i++] = (uint8_t)v;
    }
    if (!length[256]) return -1;                          /* (no end code) */
    if (code_of(&lit, length, nlit) < 0 || code_of(&dist, length + nlit, ndist) < 0) return -1;
    return codes(z, &lit, &dist);
}

long mdrt_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t max, size_t *used)
{
    struct inflate z = { in, n, 0, 0, 0, out, 0, max };
    int last;
    do {
        last = take(&z, 1);
        int type = take(&z, 2);
        if (last < 0 || type < 0 || type == 3) return -1;
        if (type == 0) {
            z.bits = 0;                                   /* (to the byte) */
            z.count = 0;
            if (z.at + 4 > z.n) return -1;
            size_t len = (size_t)(in[z.at] | in[z.at + 1] << 8);
            if ((len ^ 0xFFFF) != (size_t)(in[z.at + 2] | in[z.at + 3] << 8)) return -1;
            z.at += 4;
            if (len > z.n - z.at || len > z.max - z.got) return -1;
            memcpy(z.out + z.got, in + z.at, len);
            z.at += len;
            z.got += len;
        } else if ((type == 1 ? fixed(&z) : dynamic(&z)) < 0) return -1;
    } while (!last);
    if (used) *used = z.at;
    return (long)z.got;
}

/* ---- deflate: LZ77 on a hash of three bytes, the fixed codes --------------------------------------- */

struct deflate { uint8_t *b; size_t n, cap; uint32_t bits; int count; int bad; };

static void put_bits(struct deflate *d, uint32_t v, int n)
{
    d->bits |= v << d->count;
    d->count += n;
    while (d->count >= 8) {
        if (d->n == d->cap) {
            size_t cap = d->cap ? d->cap * 2 : 4096;
            uint8_t *p = realloc(d->b, cap);
            if (!p) { d->bad = 1; d->n = 0; }
            else { d->b = p; d->cap = cap; }
        }
        if (!d->bad) d->b[d->n++] = (uint8_t)d->bits;
        d->bits >>= 8;
        d->count -= 8;
    }
}

/* a Huffman code: its bits from the highest, as the stream wants them */
static void put_code(struct deflate *d, uint32_t code, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= (code >> i & 1) << (n - 1 - i);
    put_bits(d, r, n);
}

static void put_literal(struct deflate *d, int s)
{
    if (s < 144) put_code(d, 0x30 + (uint32_t)s, 8);
    else if (s < 256) put_code(d, 0x190 + (uint32_t)(s - 144), 9);
    else if (s < 280) put_code(d, (uint32_t)(s - 256), 7);
    else put_code(d, 0xC0 + (uint32_t)(s - 280), 8);
}

static void put_match(struct deflate *d, int len, int back)
{
    int s = 28;
    while (len_base[s] > len) s--;
    put_literal(d, 257 + s);
    put_bits(d, (uint32_t)(len - len_base[s]), len_extra[s]);
    int c = 29;
    while (dist_base[c] > back) c--;
    put_code(d, (uint32_t)c, 5);
    put_bits(d, (uint32_t)(back - dist_base[c]), dist_extra[c]);
}

#define WINDOW 32768
#define HASH_BITS 15
#define CHAIN 128                       /* (places tried for each match) */

static uint32_t hash3(const uint8_t *p) { return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & ((1u << HASH_BITS) - 1); }

/* the longest match for in[at] among the places before it with the same three bytes: its length
 * (0: none of 3 at least) and how far back */
static int longest(const uint8_t *in, size_t n, size_t at, const int32_t *head, const int32_t *prev, int *back)
{
    int best = 0, tries = CHAIN;
    size_t most = n - at < 258 ? n - at : 258;
    if (most < 3) return 0;
    for (int32_t p = head[hash3(in + at)]; p >= 0 && (size_t)p < at && at - (size_t)p < WINDOW && tries--; p = prev[p & (WINDOW - 1)]) {
        size_t k = 0;
        while (k < most && in[(size_t)p + k] == in[at + k]) k++;
        if ((int)k > best) {
            best = (int)k;
            *back = (int)(at - (size_t)p);
            if (k == most) break;
        }
    }
    return best >= 3 ? best : 0;
}

static void insert(const uint8_t *in, size_t n, size_t at, int32_t *head, int32_t *prev)
{
    if (at + 3 > n) return;
    uint32_t h = hash3(in + at);
    prev[at & (WINDOW - 1)] = head[h];
    head[h] = (int32_t)at;
}

size_t mdrt_deflate(const uint8_t *in, size_t n, uint8_t **out)
{
    struct deflate d = { 0 };
    int32_t *head = malloc(sizeof *head << HASH_BITS), *prev = malloc(sizeof *prev * WINDOW);
    *out = NULL;
    if (!head || !prev) { free(head); free(prev); return 0; }
    for (int i = 0; i < 1 << HASH_BITS; i++) head[i] = -1;
    put_bits(&d, 1, 1);                                   /* one block, the last: the fixed codes */
    put_bits(&d, 1, 2);
    for (size_t at = 0; at < n;) {
        int back = 0, len = longest(in, n, at, head, prev, &back), back2 = 0;
        insert(in, n, at, head, prev);
        if (len && at + 1 < n && longest(in, n, at + 1, head, prev, &back2) > len) len = 0;   /* (a longer one a byte on: the byte first) */
        if (!len) {
            put_literal(&d, in[at++]);
            continue;
        }
        put_match(&d, len, back);
        for (size_t k = 1; k < (size_t)len; k++) insert(in, n, at + k, head, prev);
        at += (size_t)len;
    }
    put_literal(&d, 256);
    put_bits(&d, 0, 7);                                   /* (to the byte) */
    free(head);
    free(prev);
    if (d.bad) { free(d.b); return 0; }
    *out = d.b;
    return d.n;
}

/* ---- reading ------------------------------------------------------------------------------------ */

static int no(char *why, size_t size, const char *say)
{
    if (why && size) snprintf(why, size, "%s", say);
    return -1;
}

void mdrt_png_free(mdrt_png *p)
{
    free(p->rgba);
    free(p->index);
    p->rgba = NULL;
    p->index = NULL;
}

/* a row's filter undone, in place: bpp bytes a pixel (1 at least), prior the row above (NULL for
 * none) */
static int unfilter(uint8_t *row, const uint8_t *prior, size_t len, size_t bpp, int type)
{
    for (size_t i = 0; i < len; i++) {
        int a = i >= bpp ? row[i - bpp] : 0, b = prior ? prior[i] : 0, c = prior && i >= bpp ? prior[i - bpp] : 0;
        switch (type) {
        case 0: break;
        case 1: row[i] = (uint8_t)(row[i] + a); break;
        case 2: row[i] = (uint8_t)(row[i] + b); break;
        case 3: row[i] = (uint8_t)(row[i] + ((a + b) >> 1)); break;
        case 4: {                                         /* (Paeth: the nearest of left, above, above left) */
            int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            row[i] = (uint8_t)(row[i] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
            break;
        }
        default: return -1;
        }
    }
    return 0;
}

struct header { uint32_t w, h; int depth, type, interlace, channels; uint32_t trns[3]; int has_trns; };

/* sample k of a row of samples depth bits each */
static unsigned sample(const uint8_t *row, size_t k, int depth)
{
    if (depth == 8) return row[k];
    if (depth == 16) return row[2 * k];                   /* (the high byte) */
    size_t bit = k * (size_t)depth;
    return (unsigned)(row[bit >> 3] >> (8 - depth - (int)(bit & 7)) & ((1u << depth) - 1));
}

/* the pixel at sample k (its first channel) of a row, into out */
static int pixel(const struct header *H, const uint8_t *row, size_t k, mdrt_png *out, size_t at)
{
    unsigned v[4];
    for (int c = 0; c < H->channels; c++) v[c] = sample(row, k + (size_t)c, H->depth);
    unsigned max = H->depth >= 8 ? 255 : (1u << H->depth) - 1;
    uint32_t rgba;
    if (H->type == 3) {
        if ((int)v[0] >= out->colours) return -1;
        out->index[at] = (uint8_t)v[0];
        rgba = out->palette[v[0]];
    } else if (H->type == 0 || H->type == 4) {
        unsigned raw = H->depth == 16 ? (unsigned)(row[2 * k] << 8 | row[2 * k + 1]) : v[0];
        unsigned g = v[0] * 255 / max, a = H->type == 4 ? v[1] : H->has_trns && raw == H->trns[0] ? 0 : 255;
        rgba = (uint32_t)a << 24 | g * 0x010101u;
    } else {
        unsigned a = H->type == 6 ? v[3] : 255;
        if (H->type == 2 && H->has_trns) {
            int same = 1;
            for (int c = 0; c < 3; c++)
                same &= (H->depth == 16 ? (unsigned)(row[2 * (k + (size_t)c)] << 8 | row[2 * (k + (size_t)c) + 1]) : v[c]) == H->trns[c];
            if (same) a = 0;
        }
        rgba = (uint32_t)a << 24 | v[0] << 16 | v[1] << 8 | v[2];
    }
    out->rgba[at] = rgba;
    return 0;
}

int mdrt_png_read(const uint8_t *png, size_t size, mdrt_png *out, char *why, size_t why_size)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    memset(out, 0, sizeof *out);
    if (size < 8 || memcmp(png, sig, 8)) return no(why, why_size, "not a PNG");
    struct header H = { 0 };
    uint8_t *z = NULL, *raw = NULL;
    size_t zn = 0, zcap = 0;
    int got_header = 0, got_end = 0, r = -1;
    const char *say = "a PNG cut short";
    for (size_t o = 8; o + 12 <= size;) {
        uint32_t len = be(png + o);
        const uint8_t *type = png + o + 4, *data = png + o + 8;
        if (len > size - o - 12) goto done;
        if (crc_step(0, type, 4 + (size_t)len) != be(data + len)) { say = "a PNG damaged (a chunk's CRC is wrong)"; goto done; }
        o += 12 + (size_t)len;
        if (!got_header && memcmp(type, "IHDR", 4)) { say = "not a PNG (no header first)"; goto done; }
        if (!memcmp(type, "IHDR", 4)) {
            if (got_header || len != 13) { say = "a PNG with a bad header"; goto done; }
            got_header = 1;
            H.w = be(data);
            H.h = be(data + 4);
            H.depth = data[8];
            H.type = data[9];
            H.interlace = data[12];
            static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
            H.channels = H.type <= 6 ? channels[H.type] : 0;
            int d = H.depth, ok = H.channels && (H.type == 0 ? d == 1 || d == 2 || d == 4 || d == 8 || d == 16
                                                : H.type == 3 ? d == 1 || d == 2 || d == 4 || d == 8 : d == 8 || d == 16);
            if (!ok || data[10] || data[11] || H.interlace > 1) { say = "a PNG of a kind there is none of (its colour type, bit depth or method)"; goto done; }
            if (!H.w || !H.h || H.w > MDRT_PNG_MAX_SIDE || H.h > MDRT_PNG_MAX_SIDE || (uint64_t)H.w * H.h > MDRT_PNG_MAX_PIXELS) {
                say = "a PNG too big (8192 pixels a side, 16 million in all, at most)";
                goto done;
            }
        } else if (!memcmp(type, "PLTE", 4)) {
            if (len % 3 || len / 3 > 256 || !len) { say = "a PNG with a bad palette"; goto done; }
            out->colours = (int)(len / 3);
            for (int i = 0; i < out->colours; i++) out->palette[i] = 0xFF000000u | (uint32_t)data[3 * i] << 16 | (uint32_t)data[3 * i + 1] << 8 | data[3 * i + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            if (H.type == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++) out->palette[i] = (out->palette[i] & 0xFFFFFF) | (uint32_t)data[i] << 24;
            } else if ((H.type == 0 && len == 2) || (H.type == 2 && len == 6)) {
                for (uint32_t c = 0; c < len / 2; c++) H.trns[c] = (uint32_t)(data[2 * c] << 8 | data[2 * c + 1]);
                if (H.depth == 8 && H.type == 2)          /* (8-bit samples: the value's low byte) */
                    for (int c = 0; c < 3; c++) H.trns[c] &= 0xFF;
                H.has_trns = 1;
            }
        } else if (!memcmp(type, "IDAT", 4) && len) {
            if (zn + len > zcap) {
                size_t cap = zcap ? zcap : 65536;
                while (cap < zn + len) cap *= 2;
                uint8_t *p = realloc(z, cap);
                if (!p) { say = "out of memory"; goto done; }
                z = p;
                zcap = cap;
            }
            memcpy(z + zn, data, len);
            zn += len;
        } else if (!memcmp(type, "IEND", 4)) {
            got_end = 1;
            break;
        } else if (!(type[0] & 0x20)) { say = "a PNG with a part this reader does not know (a critical chunk)"; goto done; }
    }
    if (!got_header || !got_end) goto done;
    if (H.type == 3 && !out->colours) { say = "an indexed PNG with no palette"; goto done; }
    if (H.type != 3) out->colours = 0;
    /* the passes (Adam7's seven, or the one picture) and the bytes their rows take */
    static const int px0[7] = { 0, 4, 0, 2, 0, 1, 0 }, py0[7] = { 0, 0, 4, 0, 2, 0, 1 }, dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = H.interlace ? 7 : 1;
    size_t bits = (size_t)H.channels * (size_t)H.depth, bpp = (bits + 7) / 8, total = 0;
    for (int p = 0; p < passes; p++) {
        size_t pw = H.interlace ? (H.w - (size_t)px0[p] + (size_t)dx[p] - 1) / (size_t)dx[p] : H.w;
        size_t ph = H.interlace ? (H.h - (size_t)py0[p] + (size_t)dy[p] - 1) / (size_t)dy[p] : H.h;
        if (H.interlace && (H.w <= (uint32_t)px0[p] || H.h <= (uint32_t)py0[p])) pw = ph = 0;
        if (pw && ph) total += ph * (1 + (pw * bits + 7) / 8);
    }
    if (zn < 6 || (z[0] & 0x0F) != 8 || (z[0] >> 4) > 7 || ((z[0] << 8) | z[1]) % 31 || (z[1] & 0x20)) { say = "a PNG whose picture is not a zlib stream"; goto done; }
    if (!(raw = malloc(total ? total : 1))) { say = "out of memory"; goto done; }
    size_t used;
    long n = mdrt_inflate(z + 2, zn - 2, raw, total, &used);
    if (n != (long)total || 2 + used + 4 > zn || adler(raw, total) != be(z + 2 + used)) { say = "a PNG damaged (its picture's data)"; goto done; }
    out->width = (int)H.w;
    out->height = (int)H.h;
    out->rgba = malloc((size_t)H.w * H.h * sizeof *out->rgba);
    if (H.type == 3) out->index = malloc((size_t)H.w * H.h);
    if (!out->rgba || (H.type == 3 && !out->index)) { say = "out of memory"; goto done; }
    uint8_t *at = raw;
    for (int p = 0; p < passes; p++) {
        size_t pw = H.interlace ? (H.w - (size_t)px0[p] + (size_t)dx[p] - 1) / (size_t)dx[p] : H.w;
        size_t ph = H.interlace ? (H.h - (size_t)py0[p] + (size_t)dy[p] - 1) / (size_t)dy[p] : H.h;
        if (H.interlace && (H.w <= (uint32_t)px0[p] || H.h <= (uint32_t)py0[p])) continue;
        size_t row = (pw * bits + 7) / 8;
        const uint8_t *prior = NULL;
        for (size_t y = 0; y < ph; y++, at += 1 + row) {
            if (unfilter(at + 1, prior, row, bpp, at[0]) < 0) { say = "a PNG damaged (a row's filter)"; goto done; }
            prior = at + 1;
            for (size_t x = 0; x < pw; x++) {
                size_t ox = H.interlace ? (size_t)px0[p] + x * (size_t)dx[p] : x, oy = H.interlace ? (size_t)py0[p] + y * (size_t)dy[p] : y;
                if (pixel(&H, at + 1, x * (size_t)H.channels, out, oy * H.w + ox) < 0) { say = "an indexed PNG with a pixel past its palette"; goto done; }
            }
        }
    }
    r = 0;
done:
    free(z);
    free(raw);
    if (r < 0) {
        mdrt_png_free(out);
        no(why, why_size, say);
    }
    return r;
}

int mdrt_png_decode(const uint8_t *png, size_t size, uint32_t *out, int max_w, int max_h, int *w, int *h)
{
    mdrt_png p;
    if (mdrt_png_read(png, size, &p, NULL, 0) < 0) return -1;
    int r = -1;
    if (p.width <= max_w && p.height <= max_h) {
        for (int y = 0; y < p.height; y++)
            for (int x = 0; x < p.width; x++) out[(size_t)y * (size_t)max_w + (size_t)x] = 0xFF000000u | p.rgba[(size_t)y * (size_t)p.width + (size_t)x];
        *w = p.width;
        *h = p.height;
        r = 0;
    }
    mdrt_png_free(&p);
    return r;
}

/* ---- writing an indexed picture ----------------------------------------------------------------- */

size_t mdrt_png_write_indexed(const uint8_t *index, int width, int height, int pitch, const uint32_t *palette, int colours,
                              int transparent0, uint8_t **out)
{
    *out = NULL;
    if (width <= 0 || height <= 0 || pitch < width || colours < 1 || colours > 256 || width > MDRT_PNG_MAX_SIDE || height > MDRT_PNG_MAX_SIDE) return 0;
    size_t row = 1 + (size_t)width, raw_n = row * (size_t)height;
    uint8_t *raw = malloc(raw_n), *z = NULL;
    if (!raw) return 0;
    for (int y = 0; y < height; y++) {                    /* (filter 0, as indexed pictures want) */
        raw[(size_t)y * row] = 0;
        memcpy(raw + (size_t)y * row + 1, index + (size_t)y * (size_t)pitch, (size_t)width);
    }
    size_t zn = mdrt_deflate(raw, raw_n, &z);
    uint32_t sum = adler(raw, raw_n);
    free(raw);
    if (!zn) return 0;
    size_t total = 8 + (12 + 13) + (12 + 3 * (size_t)colours) + (transparent0 ? 12 + 1 : 0) + (12 + 2 + zn + 4) + 12;
    uint8_t *b = malloc(total), *p = b;
    if (!b) { free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    memcpy(p, sig, 8); p += 8;
    be32(p + 8, (uint32_t)width); be32(p + 12, (uint32_t)height);
    p[16] = 8; p[17] = 3; p[18] = 0; p[19] = 0; p[20] = 0;     /* 8 bits, indexed */
    p += chunk(p, "IHDR", 13);
    for (int i = 0; i < colours; i++) { p[8 + 3 * i] = (uint8_t)(palette[i] >> 16); p[9 + 3 * i] = (uint8_t)(palette[i] >> 8); p[10 + 3 * i] = (uint8_t)palette[i]; }
    p += chunk(p, "PLTE", 3 * (size_t)colours);
    if (transparent0) { p[8] = 0; p += chunk(p, "tRNS", 1); }
    p[8] = 0x78; p[9] = 0x01;                             /* (zlib: deflate, a 32 KB window, the fastest's mark) */
    memcpy(p + 10, z, zn);
    be32(p + 10 + zn, sum);
    p += chunk(p, "IDAT", 2 + zn + 4);
    p += chunk(p, "IEND", 0);
    free(z);
    *out = b;
    return (size_t)(p - b);
}
