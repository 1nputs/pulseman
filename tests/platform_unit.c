/* The platform's pure helpers: the settings store (a hand-edited file read, changed and written
 * back with its comments, order and unknown keys kept), where the picture goes in a window for
 * each scaling mode and aspect, and the PNG encoder (decoded back here: chunks, CRCs, the stored
 * deflate blocks, Adler-32, every pixel); and the menu: its moves and values, a long list's pages,
 * its drawing (MDRT_MENU_PNG=FILE writes a page as a PNG to look at); and the upscalers: hand-worked
 * cases, their outputs on generated pixel art against hashes of the MMPX authors' reference code's
 * (MDRT_UPSCALE_DUMP=DIR writes the inputs for that code), Scale3x against a second formulation. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include <stdio.h>
#include "mdrt/menu.h"
#include "mdrt/png.h"
#include "mdrt/settings.h"
#include "mdrt/upscale.h"
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
    }
}

int main(void)
{
    settings();
    views();
    png();
    menu();
    upscalers();
    return check_done("platform_unit");
}
