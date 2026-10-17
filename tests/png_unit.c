/* PNGs and deflate (mdrt/png.h): every kind of PNG read (tests/png_cases.h: made by an encoder of
 * tools/gen_png_cases.py's own and Python's zlib, each with the hashes of what a reader must make
 * of it, worked out from its samples); deflate and inflate both ways on data of every sort; an
 * indexed picture written and read back, palette, transparency and all, the same pixels making
 * the same bytes; and damaged files (every case cut short, and bytes changed at random) failing,
 * never more (the sanitizers watch). */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/png.h"
#include "png_cases.h"

static unsigned long long fnv(const void *p, size_t n)
{
    unsigned long long h = 0xCBF29CE484222325ull;
    for (const unsigned char *s = p; n--; s++) h = (h ^ *s) * 0x100000001B3ull;
    return h;
}

/* the hash of pixels 0xAARRGGBB, a little-endian word each (as the generator hashes them) */
static unsigned long long fnv_words(const uint32_t *w, size_t n)
{
    unsigned long long h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) h = (h ^ (w[i] >> (8 * k) & 0xFF)) * 0x100000001B3ull;
    return h;
}

static void cases(void)
{
    int read = 0, n = (int)(sizeof png_cases / sizeof png_cases[0]);
    for (int i = 0; i < n; i++) {
        const struct png_case *c = &png_cases[i];
        mdrt_png p;
        char why[120];
        int ok = mdrt_png_read(c->png, c->size, &p, why, sizeof why) == 0;
        if (!ok) { printf("  %s: %s\n", c->name, why); CHECK(ok); continue; }
        int same = p.width == c->w && p.height == c->h && fnv_words(p.rgba, (size_t)c->w * (size_t)c->h) == c->rgba
                   && (c->index ? p.index && fnv(p.index, (size_t)c->w * (size_t)c->h) == c->index : !p.index);
        if (!same) printf("  %s: not the pixels it holds\n", c->name);
        CHECK(same);
        read += same;
        mdrt_png_free(&p);
    }
    printf("  png: %d of %d PNGs of every kind read as they are\n", read, n);
}

static uint32_t next(uint32_t *x) { *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5; return *x; }

static void deflate(void)
{
    enum { N = 200000 };
    uint8_t *in = malloc(N), *back = malloc(N + 1);
    uint32_t x = 7;
    int kinds = 0, ok = 0;
    for (int kind = 0; kind < 7; kind++) {
        size_t n = kind == 0 ? 0 : kind == 1 ? 1 : N;
        for (size_t i = 0; i < n; i++)
            switch (kind) {
            case 1: case 2: in[i] = (uint8_t)next(&x); break;                       /* noise */
            case 3: in[i] = 'a'; break;                                              /* one run */
            case 4: in[i] = (uint8_t)("the quick brown fox "[i % 20] ^ (i % 997 ? 0 : 1)); break;
            case 5: in[i] = (uint8_t)(i < 40000 ? next(&x) : in[i - 40000 + (i % 3 ? 0 : 1) * 0]); break;   /* past the window */
            default: in[i] = (uint8_t)((i / 37) * 3 ^ (next(&x) % 4 ? 0 : next(&x))); break;
            }
        uint8_t *z;
        size_t zn = mdrt_deflate(in, n, &z), used = 0;
        long got = zn ? mdrt_inflate(z, zn, back, N + 1, &used) : -1;
        int same = got == (long)n && used == zn && !memcmp(in, back, n);
        uint8_t *z2;
        size_t zn2 = mdrt_deflate(in, n, &z2);              /* (the same bytes again) */
        same &= zn2 == zn && !memcmp(z, z2, zn);
        if (kind == 3 || kind == 4) same &= zn < n / 20;     /* (it compresses) */
        CHECK(same);
        ok += same;
        kinds++;
        CHECK_EQ(mdrt_inflate(z, zn, back, n ? n - 1 : 0, NULL), n ? -1 : 0);   /* (not room for it) */
        free(z);
        free(z2);
    }
    /* a stored block, then fixed codes, in one stream (Python's zlib's "aab" after the "abc") */
    static const uint8_t two[] = { 0x00, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c', 0x4B, 0x4C, 0x4C, 0x02, 0x00 };
    CHECK_EQ(mdrt_inflate(two, sizeof two, back, 16, NULL), 6);
    CHECK(!memcmp(back, "abcaab", 6));
    printf("  deflate: %d of %d kinds of data deflated and back as they were\n", ok, kinds);
    free(in);
    free(back);
}

static void indexed(void)
{
    enum { W = 77, H = 45, PITCH = 80 };
    static uint8_t idx[PITCH * H];
    uint32_t pal[16], x = 99;
    for (int i = 0; i < 16; i++) pal[i] = next(&x) & 0xFFFFFF;
    for (int y = 0; y < H; y++) for (int i = 0; i < PITCH; i++) idx[y * PITCH + i] = (uint8_t)((i / 8 + y / 8) % 16 ^ (next(&x) % 9 ? 0 : 1));
    uint8_t *png, *png2;
    size_t n = mdrt_png_write_indexed(idx, W, H, PITCH, pal, 16, 1, &png), n2 = mdrt_png_write_indexed(idx, W, H, PITCH, pal, 16, 1, &png2);
    CHECK(n > 0 && n == n2 && !memcmp(png, png2, n));
    mdrt_png p;
    CHECK_EQ(mdrt_png_read(png, n, &p, NULL, 0), 0);
    int same = p.width == W && p.height == H && p.colours == 16 && p.index;
    for (int y = 0; same && y < H; y++)
        for (int i = 0; i < W; i++) {
            uint8_t k = idx[y * PITCH + i];
            same &= p.index[y * W + i] == k && p.rgba[y * W + i] == ((k ? 0xFF000000u : 0) | pal[k]);
        }
    CHECK(same);
    printf("  png: an indexed picture %dx%d, %zu bytes, read back %s\n", W, H, n, same ? "as it was" : "WRONG");
    mdrt_png_free(&p);
    free(png2);
    CHECK_EQ(mdrt_png_write_indexed(idx, 0, H, PITCH, pal, 16, 0, &png2), 0);
    CHECK_EQ(mdrt_png_write_indexed(idx, W, H, PITCH, pal, 0, 0, &png2), 0);
    free(png);
}

/* every case cut at every few bytes, and with bytes changed (its CRCs then put right, so the
 * damage reaches the picture): each read fails or succeeds, nothing worse */
static void damaged(void)
{
    int n = (int)(sizeof png_cases / sizeof png_cases[0]), cut = 0, changed = 0;
    uint32_t x = 12345;
    for (int i = 0; i < n; i++) {
        const struct png_case *c = &png_cases[i];
        uint8_t *b = malloc(c->size);
        for (unsigned len = 0; len < c->size; len += 1 + len / 16) {
            memcpy(b, c->png, len);
            mdrt_png p;
            cut += mdrt_png_read(b, len, &p, NULL, 0) < 0;
            mdrt_png_free(&p);
        }
        for (int t = 0; t < 300; t++) {
            memcpy(b, c->png, c->size);
            for (int k = 1 + (int)(next(&x) % 4); k--;) b[8 + next(&x) % (c->size - 8)] ^= (uint8_t)(1u << (next(&x) % 8));
            for (unsigned o = 8; o + 12 <= c->size;) {    /* (the CRCs right again) */
                unsigned len = (unsigned)b[o] << 24 | (unsigned)b[o + 1] << 16 | (unsigned)b[o + 2] << 8 | b[o + 3];
                if (len > c->size - o - 12) break;
                uint32_t crc = mdrt_crc32(0, b + o + 4, 4 + (size_t)len);
                b[o + 8 + len] = (uint8_t)(crc >> 24); b[o + 9 + len] = (uint8_t)(crc >> 16); b[o + 10 + len] = (uint8_t)(crc >> 8); b[o + 11 + len] = (uint8_t)crc;
                o += 12 + len;
            }
            mdrt_png p;
            changed += mdrt_png_read(b, c->size, &p, NULL, 0) < 0;
            mdrt_png_free(&p);
        }
        free(b);
    }
    printf("  png: damaged files: %d cut short refused; %d of %d changed at random refused, the rest read\n", cut, changed, 300 * n);
    CHECK(cut > 0 && changed > 0);
}

int main(void)
{
    cases();
    deflate();
    indexed();
    damaged();
    return check_done("png_unit");
}
