/* The platform's pure helpers: the settings store (a hand-edited file read, changed and written
 * back with its comments, order and unknown keys kept), where the picture goes in a window for
 * each scaling mode and aspect, and the PNG encoder (decoded back here: chunks, CRCs, the stored
 * deflate blocks, Adler-32, every pixel). */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/png.h"
#include "mdrt/settings.h"
#include "mdrt/view.h"

static void settings(void)
{
    const char *file = "# my settings\r\nvideo.scale = integer\n\n  audio.volume=40  \nmystery.key = kept\nnot a setting\n";
    mdrt_settings *s = mdrt_settings_new();
    CHECK_EQ(mdrt_settings_parse(s, file, strlen(file)), 3);
    CHECK(!strcmp(mdrt_settings_get(s, "video.scale"), "integer"));
    CHECK_EQ(mdrt_settings_get_int(s, "audio.volume", 100), 40);
    CHECK_EQ(mdrt_settings_get_int(s, "video.scale", 7), 7);          /* (not a number) */
    CHECK(mdrt_settings_get(s, "audio.mute") == NULL);
    CHECK(!mdrt_settings_dirty(s));
    mdrt_settings_set_int(s, "audio.volume", 40);                     /* the same: not dirty */
    CHECK(!mdrt_settings_dirty(s));
    mdrt_settings_default(s, "video.scale", "sharp");                 /* present: kept */
    mdrt_settings_default(s, "audio.mute", "0");                      /* absent: added */
    mdrt_settings_default(s, "audio.device", "");                     /* empty: "key =" */
    mdrt_settings_set_int(s, "audio.volume", 70);
    CHECK(mdrt_settings_dirty(s));
    size_t n = mdrt_settings_format(s, NULL, 0);
    char *out = malloc(n);
    CHECK_EQ(mdrt_settings_format(s, out, n), n);
    const char *want = "# my settings\nvideo.scale = integer\n\naudio.volume = 70\nmystery.key = kept\nnot a setting\naudio.mute = 0\naudio.device =\n";
    CHECK(!strcmp(out, want));
    mdrt_settings *t = mdrt_settings_new();                           /* and read back: the same */
    mdrt_settings_parse(t, out, n - 1);
    char *again = malloc(n);
    mdrt_settings_format(t, again, n);
    CHECK(!strcmp(again, want));
    mdrt_settings_clean(s);
    CHECK(!mdrt_settings_dirty(s));
    const char *keys = "key.volume_up = =\nkey.slow = ,\nkey.none =\n";      /* keys that are signs, and none */
    CHECK_EQ(mdrt_settings_parse(t, keys, strlen(keys)), 3);
    const char *up = mdrt_settings_get(t, "key.volume_up"), *slow = mdrt_settings_get(t, "key.slow"), *none = mdrt_settings_get(t, "key.none");
    CHECK(up && !strcmp(up, "=") && slow && !strcmp(slow, ",") && none && !*none);
    free(out); free(again);
    mdrt_settings_free(s); mdrt_settings_free(t);
}

static void views(void)
{
    /* 320 x 224 in a window of 1000 x 700 */
    mdrt_view v = mdrt_view_place(1000, 700, 320, 224, MDRT_SCALE_FIT, MDRT_ASPECT_SQUARE);
    CHECK(v.w == 1000 && v.h == 700 && v.x == 0 && v.y == 0 && v.prescale == 1);
    v = mdrt_view_place(1000, 700, 320, 224, MDRT_SCALE_INTEGER, MDRT_ASPECT_SQUARE);
    CHECK(v.w == 960 && v.h == 672 && v.x == 20 && v.y == 14 && v.prescale == 1);
    v = mdrt_view_place(1000, 700, 320, 224, MDRT_SCALE_SHARP, MDRT_ASPECT_SQUARE);
    CHECK(v.w == 1000 && v.h == 700 && v.prescale == 3);
    /* 4:3: 224 lines shown 298.67 wide at 1x */
    v = mdrt_view_place(1000, 700, 320, 224, MDRT_SCALE_FIT, MDRT_ASPECT_4_3);
    CHECK(v.h == 700 && v.w == 933 && v.x == 33);
    v = mdrt_view_place(1000, 700, 256, 224, MDRT_SCALE_INTEGER, MDRT_ASPECT_4_3);   /* H32: the same shape */
    CHECK(v.h == 672 && v.w == 896);
    /* smaller than the picture: no whole multiple, so it fits */
    v = mdrt_view_place(200, 150, 320, 224, MDRT_SCALE_INTEGER, MDRT_ASPECT_SQUARE);
    CHECK(v.w == 200 && v.h == 140 && v.prescale == 1);
    v = mdrt_view_place(0, 150, 320, 224, MDRT_SCALE_SHARP, MDRT_ASPECT_SQUARE);
    CHECK(v.w == 0 && v.h == 0);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
    return ~c;
}

static void png(void)
{
    enum { W = 300, H = 230, PITCH = 320 };                           /* (several stored blocks) */
    static uint32_t px[PITCH * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < PITCH; x++) px[y * PITCH + x] = (uint32_t)(x * 7 + y * 131) * 0x010203u & 0xFFFFFF;
    size_t n = mdrt_png_encode(px, W, H, PITCH, NULL, 0);
    uint8_t *b = malloc(n);
    CHECK_EQ(mdrt_png_encode(px, W, H, PITCH, b, n), n);
    CHECK_EQ(mdrt_png_encode(px, 0, H, PITCH, b, n), 0);
    CHECK(!memcmp(b, "\x89PNG\r\n\x1a\n", 8));
    /* the chunks and their CRCs */
    size_t o = 8, idat = 0, idat_len = 0;
    int chunks = 0, crcs = 0;
    while (o + 12 <= n) {
        uint32_t len = rd32(b + o);
        crcs += crc(b + o + 4, 4 + len) == rd32(b + o + 8 + len);
        if (!memcmp(b + o + 4, "IHDR", 4)) CHECK(rd32(b + o + 8) == W && rd32(b + o + 12) == H && b[o + 16] == 8 && b[o + 17] == 2);
        if (!memcmp(b + o + 4, "IDAT", 4)) { idat = o + 8; idat_len = len; }
        o += 12 + len;
        chunks++;
    }
    CHECK_EQ(o, n);
    CHECK_EQ(chunks, 3);
    CHECK_EQ(crcs, 3);
    /* the zlib stream: stored blocks, then the Adler-32 of what they hold */
    const uint8_t *z = b + idat;
    CHECK(z[0] == 0x78 && ((z[0] << 8) | z[1]) % 31 == 0);
    size_t at = 2, got = 0, raw_len = (size_t)H * (1 + W * 3);
    uint8_t *raw = malloc(raw_len);
    int last = 0;
    while (!last && at + 5 <= idat_len) {
        last = z[at] & 1;
        CHECK_EQ(z[at] >> 1, 0);                                       /* stored */
        size_t len = (size_t)(z[at + 1] | z[at + 2] << 8);
        CHECK_EQ(len ^ 0xFFFF, (size_t)(z[at + 3] | z[at + 4] << 8));
        if (got + len > raw_len) break;
        memcpy(raw + got, z + at + 5, len);
        got += len;
        at += 5 + len;
    }
    CHECK(last && got == raw_len && at + 4 == idat_len);
    uint32_t a = 1, s2 = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; s2 = (s2 + a) % 65521; }
    CHECK_EQ(rd32(z + at), s2 << 16 | a);
    int same = 0;
    for (int y = 0; y < H; y++) {
        const uint8_t *r = raw + (size_t)y * (1 + W * 3);
        CHECK_EQ(r[0], 0);
        for (int x = 0; x < W; x++)
            same += (uint32_t)(r[1 + 3 * x] << 16 | r[2 + 3 * x] << 8 | r[3 + 3 * x]) == px[y * PITCH + x];
    }
    printf("  png: %zu bytes, %d of %d pixels decoded back\n", n, same, W * H);
    CHECK_EQ(same, W * H);
    free(raw); free(b);
}

int main(void)
{
    settings();
    views();
    png();
    return check_done("platform_unit");
}
