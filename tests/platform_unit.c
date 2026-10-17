/* The platform's pure helpers: the settings store (a hand-edited file read, changed and written
 * back with its comments, order and unknown keys kept), where the picture goes in a window for
 * each scaling mode and aspect, and the PNG encoder (decoded back here: chunks, CRCs, the stored
 * deflate blocks, Adler-32, every pixel); and the menu: its moves and values, a long list's pages,
 * its drawing (MDRT_MENU_PNG=FILE writes a page as a PNG to look at); and the upscalers: hand-worked
 * cases, their outputs on generated pixel art against hashes of the MMPX authors' reference code's
 * (MDRT_UPSCALE_DUMP=DIR writes the inputs for that code), Scale3x against a second formulation,
 * xBR against the hashes of its shader's sums done exactly;
 * and the looks: the console's colour levels, dithering blended where it is and nowhere else, the
 * soft blend, the scanlines' rows at whole and uneven sizes, the masks' stripes, slots and dots, and
 * the light they take made up on average by the lift; and captures: GIF's palettes and code sizes,
 * its LZW back and forth (noise at every code size, through the dictionary's fresh starts; long
 * runs; one pixel; none), the last frames kept and taken in order, a recording's file read back, a
 * clip as a GIF (its frames and their times), and a setting taken out. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include <stdio.h>
#include "mdrt/capture.h"
#include "mdrt/gif.h"
#include "mdrt/look.h"
#include "mdrt/menu.h"
#include "mdrt/png.h"
#include "mdrt/settings.h"
#include "mdrt/upscale.h"
#include "mdrt/vdp.h"
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
    mdrt_settings_remove(s, "mystery.key");                           /* a key taken out: the rest as they were */
    mdrt_settings_remove(s, "no.such.key");
    CHECK(mdrt_settings_dirty(s) && !mdrt_settings_get(s, "mystery.key"));
    mdrt_settings_format(s, out, n);
    CHECK(!strcmp(out, "# my settings\nvideo.scale = integer\n\naudio.volume = 70\nnot a setting\naudio.mute = 0\naudio.device =\n"));
    mdrt_settings_clean(s);
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
    static uint32_t back[320 * 240];                                  /* mdrt_png_decode: the same pixels */
    int bw = 0, bh = 0, again = 0;
    CHECK_EQ(mdrt_png_decode(b, n, back, 320, 240, &bw, &bh), 0);
    CHECK(bw == W && bh == H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) again += back[y * 320 + x] == (0xFF000000u | px[y * PITCH + x]);
    CHECK_EQ(again, W * H);
    CHECK_EQ(mdrt_png_decode(b, n, back, 200, 240, &bw, &bh), -1);   /* too wide for the room */
    CHECK_EQ(mdrt_png_decode(b, n - 30, back, 320, 240, &bw, &bh), -1);
    free(raw); free(b);
}

static void menu(void)
{
    static const char *const names[3] = { "sharp", "integer", "fit" };
    mdrt_menu m;
    mdrt_menu_init(&m);
    mdrt_menu_page(&m, "Video");
    mdrt_menu_add(&m, 1, MDRT_ITEM_ACTION, "Resume");
    snprintf(mdrt_menu_add(&m, 2, MDRT_ITEM_TEXT, "Shown, not chosen")->detail, 96, "F1");
    mdrt_menu_add_choice(&m, 3, "Scaling", names, 3, 0);
    mdrt_menu_add_number(&m, 4, "Volume", 50, 0, 100, 10, "%");
    snprintf(mdrt_menu_add(&m, 5, MDRT_ITEM_ACTION, "Game ROM")->detail, 96, "/a/very/long/path/to/the/roms/NHL 94 (UE) [!].gen");
    mdrt_menu_select(&m, 2);                                          /* (text: the first selectable) */
    CHECK_EQ(m.sel, 0);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_DOWN), MDRT_MENU_NOTHING);
    CHECK_EQ(m.sel, 2);                                               /* past the text */
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_OK), 3);
    CHECK_EQ(m.items[2].value, 1);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_RIGHT), 3);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_RIGHT), 3);
    CHECK_EQ(m.items[2].value, 0);                                    /* around the end */
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_LEFT), 3);
    CHECK_EQ(m.items[2].value, 2);
    mdrt_menu_input(&m, MDRT_MENU_DOWN);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_RIGHT), 4);
    CHECK_EQ(m.items[3].value, 60);
    for (int i = 0; i < 4; i++) mdrt_menu_input(&m, MDRT_MENU_RIGHT);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_RIGHT), MDRT_MENU_NOTHING);  /* at its most */
    CHECK_EQ(m.items[3].value, 100);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_OK), MDRT_MENU_NOTHING);
    mdrt_menu_input(&m, MDRT_MENU_DOWN);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_OK), 5);
    mdrt_menu_input(&m, MDRT_MENU_DOWN);
    CHECK_EQ(m.sel, 0);                                               /* around the end */
    mdrt_menu_input(&m, MDRT_MENU_UP);
    CHECK_EQ(m.sel, 4);
    CHECK_EQ(mdrt_menu_input(&m, MDRT_MENU_BACK), MDRT_MENU_CLOSE);
    CHECK(mdrt_menu_find(&m, 4) == &m.items[3] && mdrt_menu_find(&m, 9) == NULL);
    /* drawn: the picture darkened, the selected row's bar, the text in the font */
    static uint32_t px[320 * 224];
    for (int i = 0; i < 320 * 224; i++) px[i] = 0xC0C0C0;
    mdrt_menu_select(&m, 1);
    snprintf(m.subtitle, sizeof m.subtitle, "the line under the title");
    snprintf(m.message, sizeof m.message, "A message: what went wrong");
    mdrt_menu_draw(&m, px, 320, 224, 320);
    CHECK_EQ(px[25 * 320 + 2], 0x303030);
    CHECK_EQ(px[(2 + 3 * 10) * 320 + 300], 0x2A4C9C);                 /* the bar on row 3, the first item's */
    CHECK_EQ(m.rows, 16);
    const char *file = getenv("MDRT_MENU_PNG");
    if (file) {
        for (int y = 0; y < 224; y++) for (int x = 0; x < 320; x++) px[y * 320 + x] = (uint32_t)(x * 255 / 320) << 16 | (uint32_t)(y * 255 / 224) << 8 | 0x60;
        mdrt_menu_draw(&m, px, 320, 224, 320);
        size_t n = mdrt_png_encode(px, 320, 224, 320, NULL, 0);
        uint8_t *b = malloc(n);
        mdrt_png_encode(px, 320, 224, 320, b, n);
        FILE *f = fopen(file, "wb");
        if (f) { fwrite(b, 1, n, f); fclose(f); }
        free(b);
    }
    static uint32_t pic[40 * 30];                                     /* a page's picture: at half size, right of the list */
    for (int i = 0; i < 40 * 30; i++) pic[i] = 0xFF00C000;
    m.image = pic; m.image_w = 40; m.image_h = 30; m.image_pitch = 40;
    mdrt_menu_draw(&m, px, 320, 224, 320);
    int right = 1 + 52 * 6 - 20;                                      /* x0 (1) + (cols - 1) * 6 - 20: its left edge */
    CHECK_EQ(px[(2 + 3 * 10 + 5) * 320 + right + 5], 0x00C000);
    CHECK_EQ(px[(2 + 3 * 10 + 5) * 320 + right - 1], 0x98A0B4);       /* its frame */
    m.image = NULL;
    for (int i = 0; i < 320 * 8; i++) px[i] = 0;
    CHECK_EQ(mdrt_menu_text(px, 320, 224, 320, 0, 0, "A", 0xFFFFFF), 6);
    CHECK(px[0] == 0 && px[1] == 0xFFFFFF && px[3] == 0xFFFFFF && px[4] == 0 && px[320] == 0xFFFFFF && px[320 + 4] == 0xFFFFFF);
    /* a long list: LEFT / RIGHT move a page, the drawing keeps the selected one in sight */
    mdrt_menu_page(&m, "Files");
    for (int i = 0; i < 40; i++) mdrt_menu_add(&m, 100 + i, MDRT_ITEM_ACTION, "a file");
    mdrt_menu_draw(&m, px, 320, 224, 320);
    mdrt_menu_input(&m, MDRT_MENU_RIGHT);
    CHECK_EQ(m.sel, 15);
    mdrt_menu_input(&m, MDRT_MENU_RIGHT);
    mdrt_menu_input(&m, MDRT_MENU_RIGHT);
    CHECK_EQ(m.sel, 39);
    mdrt_menu_draw(&m, px, 320, 224, 320);
    CHECK_EQ(m.top, 24);
    mdrt_menu_input(&m, MDRT_MENU_DOWN);
    mdrt_menu_draw(&m, px, 320, 224, 320);
    CHECK(m.sel == 0 && m.top == 0);
    mdrt_menu_free(&m);
}

/* ---- the upscalers ----------------------------------------------------------------------------- */

static uint64_t fnv(const uint32_t *px, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) h = (h ^ (px[i] >> (8 * k) & 0xFF)) * 0x100000001B3ull;
    return h;
}

static uint32_t lcg_state;
static uint32_t lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state >> 8; }

/* the generated pictures: noise in 3 colours (one half transparent), blocks and lines at 1:1, 2:1
 * and 1:2 in 6, text in the menu's font, and a dithered gradient */
enum { IMAGES = 5, IW = 96, IH = 64 };
static void image(int n, uint32_t *px)
{
    static const uint32_t pal[6] = { 0xFF102030, 0xFFF0E0D0, 0x80C04020, 0xFF2060C0, 0xFF000000, 0xFFFFFFFF };
    lcg_state = 94u + (uint32_t)n;
    for (int i = 0; i < IW * IH; i++) px[i] = pal[n == 0 ? lcg() % 3 : 1];
    if (n == 1)
        for (int r = 0; r < 40; r++) {                    /* rectangles, then lines */
            int x = (int)(lcg() % IW), y = (int)(lcg() % IH), w = 1 + (int)(lcg() % 12), h = 1 + (int)(lcg() % 9);
            uint32_t c = pal[lcg() % 6];
            for (int yy = y; yy < y + h && yy < IH; yy++) for (int xx = x; xx < x + w && xx < IW; xx++) px[yy * IW + xx] = c;
        }
    if (n == 1 || n == 2)
        for (int l = 0; l < 30; l++) {
            int x = (int)(lcg() % IW), y = (int)(lcg() % IH), dx = (int)(lcg() % 3), dy = (int)(lcg() % 3), len = 4 + (int)(lcg() % 20);
            uint32_t c = pal[lcg() % 6];
            for (int k = 0; k < len; k++) {
                int xx = x + k * (dx + 1) / 2, yy = y + k * (dy + 1) / 2;
                if (xx < IW && yy < IH) px[yy * IW + xx] = c;
            }
        }
    if (n == 3) {
        static const char *const lines[] = { "NHL '94 Wings 3", "Penalty: hooking", "0:42 2nd period", "Shots 12 - 9", "@#%&*{}[]<>?" };
        for (int i = 0; i < 5; i++) mdrt_menu_text(px, IW, IH, IW, 1, 2 + 12 * i, lines[i], pal[3 + i % 3]);
    }
    if (n == 4)
        for (int y = 0; y < IH; y++) for (int x = 0; x < IW; x++) px[y * IW + x] = ((x + y) & 1) && x * 3 > y * 4 ? pal[0] : x > 70 ? pal[3] : pal[1];
}

static void upscalers(void)
{
    static uint32_t in[IW * IH], out[16 * IW * IH], again[16 * IW * IH], scratch[4 * IW * IH];
    /* hand-worked: a staircase */
    static const char stairs[4][5] = { "#...", "##..", ".##.", "..##" };
    uint32_t s[16];
    for (int i = 0; i < 16; i++) s[i] = stairs[i / 4][i % 4] == '#' ? 0xFF000000 : 0xFFFFFFFF;
    mdrt_upscale(MDRT_UPSCALE_SCALE2X, s, 4, 4, 4, out, 8, NULL);
    CHECK(out[1 * 8 + 2] == 0xFF000000);                  /* the inner corner of a step filled */
    CHECK(out[2 * 8 + 3] == 0xFFFFFFFF);                  /* the outer one cut */
    CHECK(out[0 * 8 + 2] == 0xFFFFFFFF && out[3 * 8 + 0] == 0xFF000000);   /* (the rest as they were) */
    CHECK_EQ(mdrt_upscale_factor(MDRT_UPSCALE_SCALE3X), 3);
    CHECK_EQ(mdrt_upscale_factor(MDRT_UPSCALE_MMPX4X), 4);
    CHECK(!strcmp(mdrt_upscale_name(MDRT_UPSCALE_MMPX2X), "mmpx2x") && !strcmp(mdrt_upscale_name(99), "off"));
    /* a flat picture stays flat, every method */
    for (int i = 0; i < IW * IH; i++) in[i] = 0xFF336699;
    for (int m = 1; m < MDRT_UPSCALE_COUNT; m++) {
        int f = mdrt_upscale_factor(m), same = 1;
        mdrt_upscale(m, in, IW, IH, IW, out, IW * f, scratch);
        for (int i = 0; i < IW * IH * f * f; i++) same &= out[i] == 0xFF336699;
        CHECK(same);
    }
    /* the generated pictures: no colour that was not there; the hashes of the authors' code */
    static const uint64_t want[IMAGES][3] = {                /* scale2x (their EPX), mmpx2x, mmpx4x */
        { 0x2b13fcac46704f58ull, 0x2aa42d9dfd9fe295ull, 0x9319d8ebf2fdaaf8ull },
        { 0x040cd8b577c5fb29ull, 0x251865b5524215a8ull, 0xf7c14985e6db3dacull },
        { 0x510f56d233e4340dull, 0x24e328c09d89f7d0ull, 0x46b2903ab5090684ull },
        { 0x727826909a67f425ull, 0xa54766a81bbe16fdull, 0x10d1dfd99f3ab775ull },
        { 0x49b0dfe7688a56a5ull, 0x6be0f93a0f595725ull, 0x1e18832213921ee5ull },
    };
    const char *dump = getenv("MDRT_UPSCALE_DUMP");
    for (int n = 0; n < IMAGES; n++) {
        image(n, in);
        if (dump) {
            char path[512];
            snprintf(path, sizeof path, "%s/image%d.raw", dump, n);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(in, 4, IW * IH, f); fclose(f); }
        }
        static const int ms[3] = { MDRT_UPSCALE_SCALE2X, MDRT_UPSCALE_MMPX2X, MDRT_UPSCALE_MMPX4X };
        for (int k = 0; k < 3; k++) {
            int f = mdrt_upscale_factor(ms[k]), foreign = 0;
            mdrt_upscale(ms[k], in, IW, IH, IW, out, IW * f, scratch);
            for (int i = 0; i < IW * IH * f * f; i++) {
                int found = 0;
                for (int j = 0; j < IW * IH && !found; j++) found = out[i] == in[j];
                foreign += !found;
            }
            CHECK_EQ(foreign, 0);
            uint64_t h = fnv(out, (size_t)IW * IH * f * f);
            if (h != want[n][k]) printf("  upscale: image %d, %s: %016llx, the authors' code %016llx\n", n, mdrt_upscale_name(ms[k]),
                                        (unsigned long long)h, (unsigned long long)want[n][k]);
            CHECK(h == want[n][k]);
        }
        /* Scale3x against its other formulation (scale2x.it's "E0 = D == B ? D : E ..." when B != H and D != F) */
        mdrt_upscale(MDRT_UPSCALE_SCALE3X, in, IW, IH, IW, out, IW * 3, NULL);
        for (int y = 0; y < IH; y++)
            for (int x = 0; x < IW; x++) {
                #define P(dx, dy) in[(y + (dy) < 0 ? 0 : y + (dy) >= IH ? IH - 1 : y + (dy)) * IW + (x + (dx) < 0 ? 0 : x + (dx) >= IW ? IW - 1 : x + (dx))]
                uint32_t A = P(-1, -1), B = P(0, -1), C = P(1, -1), D = P(-1, 0), E = P(0, 0), F = P(1, 0), G = P(-1, 1), H = P(0, 1), I = P(1, 1);
                #undef P
                uint32_t e[9] = { E, E, E, E, E, E, E, E, E };
                if (B != H && D != F) {
                    e[0] = D == B ? D : E;
                    e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                    e[2] = B == F ? F : E;
                    e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                    e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                    e[6] = D == H ? D : E;
                    e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                    e[8] = H == F ? F : E;
                }
                for (int k = 0; k < 9; k++) again[(3 * y + k / 3) * IW * 3 + 3 * x + k % 3] = e[k];
            }
        CHECK(!memcmp(out, again, (size_t)IW * IH * 9 * 4));
        /* xBR at 2x, 3x, 4x against the hashes of the shader's sums done exactly (a line-by-line
         * transcription of Hyllian's GLSL in exact fractions; Mesa's GPU running the shader itself
         * agrees but for a level here and there and its own way with exact ties) */
        static const uint64_t want_xbr[IMAGES][3] = {
            { 0xe044f3c61bf9899bull, 0x87e5e1ccb5f406e1ull, 0x3adc96aae2fce11dull },
            { 0xbf646f8f78e7371dull, 0x008f218641afc7c1ull, 0x933bf23895f87734ull },
            { 0x2b776de19ea928f4ull, 0x8175deb83587cef8ull, 0x370e809426c8aafcull },
            { 0x6dfe47fff8b3a0b8ull, 0x170da207be6af631ull, 0xe10308cbefab2b5cull },
            { 0x9ae5b5e32b95e115ull, 0xb09517c312e22f85ull, 0xd022e481ad269cc5ull },
        };
        for (int k = 0; k < 3; k++) {
            int m = MDRT_UPSCALE_XBR2X + k, f = mdrt_upscale_factor(m);
            mdrt_upscale(m, in, IW, IH, IW, out, IW * f, k == 1 ? NULL : scratch);   /* (its own room, and the caller's) */
            uint64_t h = fnv(out, (size_t)IW * IH * f * f);
            if (h != want_xbr[n][k]) printf("  upscale: image %d, %s: %016llx, the exact sums %016llx\n", n, mdrt_upscale_name(m),
                                            (unsigned long long)h, (unsigned long long)want_xbr[n][k]);
            CHECK(h == want_xbr[n][k]);
        }
    }
    /* xBR blends: the staircase's steps at 2x get colours between black and white */
    mdrt_upscale(MDRT_UPSCALE_XBR2X, s, 4, 4, 4, out, 8, NULL);
    int between = 0;
    for (int i = 0; i < 64; i++) between += out[i] != 0xFF000000 && out[i] != 0xFFFFFFFF && (out[i] & 0xFF) == (out[i] >> 8 & 0xFF);
    CHECK(between > 0);
    CHECK_EQ(mdrt_upscale_factor(MDRT_UPSCALE_XBR3X), 3);
    CHECK(!strcmp(mdrt_upscale_name(MDRT_UPSCALE_XBR4X), "xbr4x") && !strcmp(mdrt_upscale_label(MDRT_UPSCALE_XBR2X), "xBR 2x"));
}

/* ---- the looks ------------------------------------------------------------------------------- */

/* a channel (16 red, 8 green, 0 blue) of the overlay at x, y */
static int ch(const uint32_t *o, int pitch, int x, int y, int sh) { return (int)(o[y * pitch + x] >> sh & 0xFF); }

static void looks(void)
{
    /* the colours' levels: level 1 and 7 of each curve */
    CHECK_EQ(mdrt_vdp_rgb(0x0002, MDRT_RGB_CONSOLE), 52u << 16);
    CHECK_EQ(mdrt_vdp_rgb(0x0E00, MDRT_RGB_CONSOLE), 255);
    CHECK_EQ(mdrt_vdp_rgb(0x00A0, MDRT_RGB_CONSOLE), 172u << 8);
    CHECK_EQ(mdrt_vdp_rgb(0x0EEE, MDRT_RGB_GPGX), 0xEEEEEE);
    CHECK_EQ(mdrt_vdp_rgb(0x0EEE, MDRT_RGB_LINEAR), 0xFFFFFF);

    /* dithering: columns A B A B A B mixed; a lone column (A A B A A), a flat run, and two columns
     * of B (B B A A B B) left; a short dither (A B A B between C's) mixed inside, its ends kept */
    enum { A = 0xFF000000, B = 0xFFFFFFFF, C = 0xFF0000FF, M = 0xFF808080 };
    static const uint32_t rows[4][8] = {
        { A, B, A, B, A, B, A, B }, { A, A, A, B, A, A, A, A }, { B, B, A, A, B, B, A, A }, { C, A, B, A, B, C, C, C },
    };
    static const uint32_t want[4][8] = {
        { A, M, M, M, M, M, M, B }, { A, A, A, B, A, A, A, A }, { B, B, A, A, B, B, A, A }, { C, A, M, M, B, C, C, C },
    };
    uint32_t out[64];
    mdrt_look_blend(MDRT_BLEND_DITHER, &rows[0][0], 8, 4, 8, out, 8);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            if (out[y * 8 + x] != want[y][x]) { printf("  blend: row %d x %d: %08X, not %08X\n", y, x, out[y * 8 + x], want[y][x]); CHECK(0); }
    mdrt_look_blend(MDRT_BLEND_OFF, &rows[0][0], 8, 4, 8, out, 8);
    CHECK(!memcmp(out, rows, sizeof rows));
    mdrt_look_blend(MDRT_BLEND_SOFT, &rows[1][0], 8, 1, 8, out, 8);   /* a quarter each side: 0 0 40 80 40 0 0 0 */
    CHECK_EQ(out[2], 0xFF404040);
    CHECK_EQ(out[3], M);
    CHECK_EQ(out[4], 0xFF404040);
    CHECK_EQ(out[0], A);

    /* nothing on: no overlay, no lift */
    CHECK(!mdrt_look_tube(MDRT_SCANLINES_OFF, MDRT_MASK_OFF));
    CHECK(mdrt_look_tube(MDRT_SCANLINES_LIGHT, MDRT_MASK_OFF) && mdrt_look_tube(MDRT_SCANLINES_OFF, MDRT_MASK_DOT));
    CHECK(!mdrt_look_tube(99, -1));
    CHECK_EQ(mdrt_look_lift(MDRT_SCANLINES_OFF, MDRT_MASK_OFF), 0);

    /* scanlines at 3x (a 10 x 10 picture in 30 x 30): every line's three rows alike, the last
     * darkest, the first the brightest; and on average, with the lift, as bright as without */
    static uint32_t ov[160 * 160];
    for (int sl = MDRT_SCANLINES_LIGHT; sl < MDRT_SCANLINES_COUNT; sl++) {
        mdrt_look_overlay(sl, MDRT_MASK_OFF, 30, 30, 10, 10, ov, 30);
        int lift = mdrt_look_lift(sl, MDRT_MASK_OFF), same = 1;
        for (int y = 0; y < 30; y++)
            for (int x = 0; x < 30; x++) same &= ov[y * 30 + x] == ov[(y % 3) * 30] && ch(ov, 30, x, y, 16) == ch(ov, 30, x, y, 0);
        CHECK(same);
        CHECK(ch(ov, 30, 0, 0, 8) > ch(ov, 30, 0, 2, 8) && ch(ov, 30, 0, 0, 8) >= ch(ov, 30, 0, 1, 8));
        double mean = (ch(ov, 30, 0, 0, 8) + ch(ov, 30, 0, 1, 8) + ch(ov, 30, 0, 2, 8)) / 3.0 + lift;
        if (mean < 245 || mean > 265) { printf("  scanlines %d: on average %.1f / 255\n", sl, mean); CHECK(0); }
        CHECK(lift > 0);
    }
    /* at 2x one row in two darker; at an uneven size (224 lines in 500 rows) every line darker at
     * its end than its start, and the rows' average the same as at 3x within a little */
    mdrt_look_overlay(MDRT_SCANLINES_MEDIUM, MDRT_MASK_OFF, 20, 20, 10, 10, ov, 20);
    CHECK(ch(ov, 20, 0, 0, 8) > ch(ov, 20, 0, 1, 8) + 40 && ov[0] == ov[2 * 20] && ov[20] == ov[3 * 20]);
    {
        static uint32_t tall[500];
        mdrt_look_overlay(MDRT_SCANLINES_MEDIUM, MDRT_MASK_OFF, 1, 500, 1, 224, tall, 1);
        int ok = 1;
        double sum = 0;
        for (int y = 0; y < 500; y++) sum += ch(tall, 1, 0, y, 8);
        for (int line = 0; line < 223; line++) {
            int first = (line * 500 + 223) / 224, last = ((line + 1) * 500) / 224 - 1;   /* (rows wholly in the line) */
            if (last > first && ch(tall, 1, 0, first, 8) <= ch(tall, 1, 0, last, 8)) ok = 0;
        }
        CHECK(ok);
        mdrt_look_overlay(MDRT_SCANLINES_MEDIUM, MDRT_MASK_OFF, 1, 30, 1, 10, ov, 1);
        double sum3 = 0;
        for (int y = 0; y < 30; y++) sum3 += ch(ov, 1, 0, y, 8);
        CHECK(sum / 500 > sum3 / 30 - 4 && sum / 500 < sum3 / 30 + 4);
    }

    /* the aperture grille at 4x: red, green, blue stripes a pixel each, every row alike; at 2x
     * magenta and green; the slot mask's breaks every fourth row, staggered between triads; the
     * shadow mask's triads offset on odd rows; each with the lift, as bright as without on average */
    mdrt_look_overlay(MDRT_SCANLINES_OFF, MDRT_MASK_APERTURE, 40, 8, 10, 2, ov, 40);
    CHECK(ch(ov, 40, 0, 0, 16) > ch(ov, 40, 0, 0, 8) && ch(ov, 40, 0, 0, 8) == ch(ov, 40, 0, 0, 0));
    CHECK(ch(ov, 40, 1, 0, 8) > ch(ov, 40, 1, 0, 16) && ch(ov, 40, 2, 0, 0) > ch(ov, 40, 2, 0, 8));
    CHECK(ov[3] == ov[0] && ov[5 * 40 + 7] == ov[7]);
    mdrt_look_overlay(MDRT_SCANLINES_OFF, MDRT_MASK_APERTURE, 20, 4, 10, 2, ov, 20);
    CHECK(ch(ov, 20, 0, 0, 16) == ch(ov, 20, 0, 0, 0) && ch(ov, 20, 0, 0, 16) > ch(ov, 20, 0, 0, 8));
    CHECK(ch(ov, 20, 1, 0, 8) > ch(ov, 20, 1, 0, 16) && ov[2] == ov[0]);
    mdrt_look_overlay(MDRT_SCANLINES_OFF, MDRT_MASK_SLOT, 40, 8, 10, 2, ov, 40);
    CHECK(ch(ov, 40, 0, 3, 16) < ch(ov, 40, 0, 0, 16) && ch(ov, 40, 3, 1, 16) < ch(ov, 40, 3, 0, 16) && ov[3] == ov[0]);
    mdrt_look_overlay(MDRT_SCANLINES_OFF, MDRT_MASK_DOT, 40, 8, 10, 2, ov, 40);
    CHECK(ch(ov, 40, 0, 0, 16) > ch(ov, 40, 0, 0, 8) && ch(ov, 40, 1, 1, 16) > ch(ov, 40, 1, 1, 8) && ov[2 * 40] == ov[0]);
    for (int mk = MDRT_MASK_APERTURE; mk < MDRT_MASK_COUNT; mk++) {
        mdrt_look_overlay(MDRT_SCANLINES_LIGHT, mk, 120, 120, 30, 30, ov, 120);
        double sum = 0;
        for (int i = 0; i < 120 * 120; i++) sum += ch(ov, 120, i % 120, i / 120, 8 * (i % 3));
        double mean = sum / (120 * 120) + mdrt_look_lift(MDRT_SCANLINES_LIGHT, mk);
        if (mean < 240 || mean > 270) { printf("  mask %d: on average %.1f / 255\n", mk, mean); CHECK(0); }
    }
}

/* ---- captures ----------------------------------------------------------------------------------- */

static void put_mem(void *user, const void *b, size_t n)
{
    struct mem { uint8_t *p; size_t n, cap; } *m = user;
    if (m->n + n > m->cap) { m->cap = (m->n + n) * 2; m->p = realloc(m->p, m->cap); }
    memcpy(m->p + m->n, b, n);
    m->n += n;
}

static void captures(void)
{
    CHECK_EQ(mdrt_gif_bits(1), 2);
    CHECK_EQ(mdrt_gif_bits(4), 2);
    CHECK_EQ(mdrt_gif_bits(5), 3);
    CHECK_EQ(mdrt_gif_bits(64), 6);
    CHECK_EQ(mdrt_gif_bits(256), 8);
    /* the palette: colours in the order first seen, the low bits of each ignored */
    static const uint16_t px[6] = { 0x0E00, 0x000E, 0x0E01, 0x00E0, 0x000E, 0x0E00 };
    uint8_t idx[6];
    uint16_t pal[256];
    CHECK_EQ(mdrt_gif_palette(px, 3, 2, 3, idx, pal), 3);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 0 && idx[3] == 2 && idx[4] == 1 && idx[5] == 0 && pal[2] == 0x00E0);

    /* LZW there and back */
    enum { N = 320 * 240 };
    static uint8_t in[N], back[N], code[N * 2];
    unsigned seed = 7;
    int all = 1;
    for (int bits = 2; bits <= 8; bits++)
        for (int kind = 0; kind < 4; kind++) {
            size_t n = kind == 2 ? 1 : kind == 3 ? 0 : N;
            for (size_t i = 0; i < n; i++) {
                seed = seed * 1103515245u + 12345u;
                in[i] = (uint8_t)(kind == 1 ? 3 & ((1 << bits) - 1) : (seed >> 16) & ((1u << bits) - 1));
            }
            size_t len = mdrt_gif_lzw(in, n, bits, code, sizeof code);
            int ok = len > 0 && len <= mdrt_gif_lzw_bound(n) && mdrt_gif_unlzw(code, len, bits, back, n) && !memcmp(in, back, n);
            if (!ok) printf("  lzw: %d bits, case %d: not the same back\n", bits, kind);
            all &= ok;
            if (kind == 1 && bits == 8) CHECK(len < n / 50);   /* (a run: a few hundred codes) */
        }
    CHECK(all);
    CHECK_EQ(mdrt_gif_lzw(in, N, 8, code, 100), 0);            /* (no room) */
    CHECK(!mdrt_gif_unlzw(code, 3, 8, back, N));               /* (cut short) */

    /* a capture keeping 5 frames, of 8: frame k all red k % 7 + 1 at 4 x 2 */
    mdrt_capture *c = mdrt_capture_new(5);
    FILE *rec = tmpfile();
    CHECK(c && rec);
    if (!c || !rec) return;
    mdrt_capture_record(c, rec);
    for (int k = 0; k < 8; k++) {
        uint16_t f[8];
        for (int i = 0; i < 8; i++) f[i] = (uint16_t)((k % 7 + 1) << 1);
        mdrt_capture_picture(c, f, 4, 2, 4);
    }
    CHECK_EQ(mdrt_capture_kept(c), 5);
    CHECK_EQ(mdrt_capture_recorded(c), 8);
    mdrt_capture_record(c, NULL);
    mdrt_capture_keep(c, 7);                                    /* (more room: the frames stay) */
    CHECK_EQ(mdrt_capture_kept(c), 5);
    mdrt_capture_keep(c, 4);                                    /* (less: the oldest go) */
    CHECK_EQ(mdrt_capture_kept(c), 4);
    mdrt_clip *clip = mdrt_capture_take(c);
    CHECK(clip && !mdrt_capture_kept(c) && !mdrt_capture_take(c));
    int w = 0, h = 0, order = 1;
    CHECK_EQ(mdrt_clip_frames(clip, &w, &h), 4);
    CHECK(w == 4 && h == 2);
    mdrt_capture_frame f;
    for (int k = 4; mdrt_clip_next(clip, &f); k++)             /* frames 4-7, the oldest first */
        order &= f.colours == 1 && f.palette[0] == (uint16_t)((k % 7 + 1) << 1) && f.w == 4 && f.h == 2;
    CHECK(order);
    /* as a GIF at 60 frames a second: frames 0 and 2 kept, 3 and 4 hundredths each; twice the size,
     * its first pixel red 5 of 7 */
    struct mem { uint8_t *p; size_t n, cap; } gif = { NULL, 0, 0 };
    CHECK(mdrt_clip_gif(clip, MDRT_RGB_LINEAR, 2, 60, put_mem, &gif, NULL));
    int frames = 0, delays[4] = { 0 };
    for (size_t i = 13; gif.p && i + 8 < gif.n; i++)
        if (gif.p[i] == 0x21 && gif.p[i + 1] == 0xF9 && frames < 4) delays[frames++] = gif.p[i + 4] | gif.p[i + 5] << 8;
    CHECK(gif.p && gif.n > 40 && !memcmp(gif.p, "GIF89a", 6) && gif.p[6] == 8 && gif.p[8] == 4 && gif.p[gif.n - 1] == 0x3B);
    CHECK(frames == 2 && delays[0] == 3 && delays[1] == 4);
    for (size_t i = 13; gif.p && i + 13 < gif.n; i++)
        if (gif.p[i] == 0x2C && gif.p[i + 5] == 8 && gif.p[i + 7] == 4) {   /* the first image: its table's first colour */
            CHECK((gif.p[i + 9] & 0x80) && gif.p[i + 10] == 182 && gif.p[i + 11] == 0 && gif.p[i + 12] == 0);
            break;
        }
    free(gif.p);
    mdrt_clip_free(clip);
    /* the recording's file: all 8 frames, read back the same */
    mdrt_clip *file = mdrt_clip_file(rec);
    order = 1;
    CHECK_EQ(mdrt_clip_frames(file, &w, &h), 8);
    for (int k = 0; mdrt_clip_next(file, &f); k++) order &= f.palette[0] == (uint16_t)((k % 7 + 1) << 1);
    CHECK(order);
    mdrt_clip_free(file);
    fclose(rec);
    mdrt_capture_free(c);
}

int main(void)
{
    settings();
    views();
    png();
    menu();
    upscalers();
    looks();
    captures();
    return check_done("platform_unit");
}
