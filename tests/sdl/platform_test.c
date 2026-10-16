/* The SDL2 platform played as a player would (make sdl-check): a thread presses keys (SDL events)
 * while a small game runs under SDL's dummy video and real-time disk sound. It checks the frame
 * rate at full speed, with fast-forward held and in slow motion; pause and a frame step; pausing
 * when the window loses focus (and the keys let go); a screenshot (its size and colour); the
 * volume and mute in the sound written to disk; the settings kept in the file with its comment
 * and an unknown key; and, run again, keys bound in that file by hand. */
#include <SDL2/SDL.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/platform_sdl2.h"
#include "mdrt/png.h"

#define BACKDROP 0x0E42                                   /* the picture: the backdrop's colour */

static const char *dir;
static char settings_path[512], shots[512], sound[512];
static SDL_atomic_t frame, held;
static int run;

static void key(SDL_Scancode k, int down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = k;
    e.key.keysym.sym = SDL_GetKeyFromScancode(k);
    SDL_PushEvent(&e);
}

static void press(SDL_Scancode k) { key(k, 1); key(k, 0); }

static void focus(int gained)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_WINDOWEVENT;
    e.window.event = gained ? SDL_WINDOWEVENT_FOCUS_GAINED : SDL_WINDOWEVENT_FOCUS_LOST;
    SDL_PushEvent(&e);
}

/* frames a second over ms */
static double rate(int ms)
{
    int f0 = SDL_AtomicGet(&frame);
    Uint64 t0 = SDL_GetPerformanceCounter();
    SDL_Delay((Uint32)ms);
    int f1 = SDL_AtomicGet(&frame);
    return (f1 - f0) * (double)SDL_GetPerformanceFrequency() / (double)(SDL_GetPerformanceCounter() - t0);
}

static void until_frame(int n) { while (SDL_AtomicGet(&frame) < n) SDL_Delay(5); }

static int player(void *user)
{
    (void)user;
    until_frame(30);
    if (run == 2) {                                       /* the keys rebound by hand */
        key(SDL_SCANCODE_S, 1); SDL_Delay(200);
        CHECK_EQ(SDL_AtomicGet(&held) & (MDRT_PAD_START | MDRT_PAD_A), MDRT_PAD_START);
        key(SDL_SCANCODE_S, 0); key(SDL_SCANCODE_RETURN, 1); key(SDL_SCANCODE_Z, 1); SDL_Delay(200);
        CHECK_EQ(SDL_AtomicGet(&held) & (MDRT_PAD_START | MDRT_PAD_A), 0);
        key(SDL_SCANCODE_RETURN, 0); key(SDL_SCANCODE_Z, 0); key(SDL_SCANCODE_K, 1); SDL_Delay(200);
        CHECK_EQ(SDL_AtomicGet(&held) & 0xFFF, MDRT_PAD_B);
        key(SDL_SCANCODE_K, 0);
        press(SDL_SCANCODE_Q);                            /* (quit, here) */
        return 0;
    }
    double normal = rate(1000);
    key(SDL_SCANCODE_SPACE, 1);
    double fast = rate(500);
    key(SDL_SCANCODE_SPACE, 0);
    press(SDL_SCANCODE_COMMA);
    double slow = rate(1000);
    press(SDL_SCANCODE_COMMA);
    printf("  frames a second: %.1f, fast-forward %.1f, slow motion %.1f\n", normal, fast, slow);
    CHECK(normal > 50 && normal < 70);
#ifndef __SANITIZE_ADDRESS__                             /* (the sanitizers' build runs no faster than about 65) */
    CHECK(fast > normal * 3);                             /* (4 times, in the settings) */
#endif
    CHECK(slow > normal * 0.4 && slow < normal * 0.6);
    press(SDL_SCANCODE_F12);
    press(SDL_SCANCODE_MINUS);
    press(SDL_SCANCODE_MINUS);
    SDL_Delay(1000);
    /* pause, two frames stepped, on again */
    press(SDL_SCANCODE_P);
    SDL_Delay(300);
    int c0 = SDL_AtomicGet(&frame);
    SDL_Delay(300);
    CHECK_EQ(SDL_AtomicGet(&frame), c0);
    press(SDL_SCANCODE_PERIOD);
    SDL_Delay(200);
    CHECK_EQ(SDL_AtomicGet(&frame), c0 + 1);
    press(SDL_SCANCODE_PERIOD);
    SDL_Delay(200);
    CHECK_EQ(SDL_AtomicGet(&frame), c0 + 2);
    press(SDL_SCANCODE_P);
    CHECK(rate(300) > 40);
    /* the window loses focus with fast-forward held: paused, and the key let go */
    key(SDL_SCANCODE_SPACE, 1);
    SDL_Delay(200);
    focus(0);
    SDL_Delay(200);
    int d0 = SDL_AtomicGet(&frame);
    SDL_Delay(300);
    CHECK_EQ(SDL_AtomicGet(&frame), d0);
    focus(1);
    double back = rate(600);
    printf("  after focus came back: %.1f frames a second\n", back);
    CHECK(back > normal * 0.8 && back < normal * 1.3);
    key(SDL_SCANCODE_SPACE, 0);
    press(SDL_SCANCODE_F6);                               /* sharp -> integer */
    press(SDL_SCANCODE_F7);                               /* square -> 4:3 */
    press(SDL_SCANCODE_M);
    SDL_Delay(800);
    press(SDL_SCANCODE_ESCAPE);
    return 0;
}

static void game(mdrt *m, void *user)
{
    (void)user;
    mdrt_vdp *v = mdrt_get_vdp(m);
    static const uint8_t regs[] = { 0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
    for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
    mdrt_vdp_ctrl32(v, 0xC0000000);
    mdrt_vdp_data16(v, BACKDROP);
    mdrt_psg_port(m, 0, 0x80 | (254 & 15));               /* a 440 Hz square, loud */
    mdrt_psg_port(m, 100, 254 >> 4);
    mdrt_psg_port(m, 200, 0x90);
    SDL_Thread *t = SDL_CreateThread(player, "player", NULL);
    for (;;) {
        mdrt_vblank_end(m);
        if (mdrt_wait_vblank(m)) break;
        SDL_AtomicSet(&held, mdrt_get_pads(m)->buttons[0]);
        SDL_AtomicAdd(&frame, 1);
    }
    SDL_WaitThread(t, NULL);
}

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*len + 1);
    if (b && fread(b, 1, (size_t)*len, f) != (size_t)*len) { free(b); b = NULL; }
    if (b) b[*len] = 0;
    fclose(f);
    return b;
}

static void spit(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int play(void)
{
    SDL_AtomicSet(&frame, 0);
    SDL_AtomicSet(&held, 0);
    mdrt_config c = { 0 };
    c.audio_rate = 48000;
    mdrt_sdl_options o = { .title = "platform test", .app = "platform-test", .settings_path = settings_path };
    return mdrt_sdl_run(&c, &o, game, NULL);
}

/* the screenshot: one PNG, the picture's size, the backdrop's colour */
static void check_screenshot(void)
{
    DIR *d = opendir(shots);
    struct dirent *e;
    int n = 0;
    char path[1024] = "";
    while (d && (e = readdir(d)))
        if (strstr(e->d_name, ".png")) { n++; snprintf(path, sizeof path, "%s/%s", shots, e->d_name); }
    if (d) closedir(d);
    CHECK_EQ(n, 1);
    CHECK(strstr(path, "/platform-test-") != NULL);
    long len = 0;
    uint8_t *b = (uint8_t *)slurp(path, &len);
    static uint32_t px[320 * 224];
    CHECK(b && (size_t)len == mdrt_png_encode(px, 320, 224, 320, NULL, 0));
    if (!b || len < 60) return;
    CHECK(!memcmp(b + 12, "IHDR", 4) && b[18] == 320 >> 8 && b[19] == (320 & 255) && b[22] == 0 && b[23] == 224);
    uint32_t rgb = mdrt_vdp_rgb(BACKDROP, 1), first = (uint32_t)b[49] << 16 | (uint32_t)b[50] << 8 | b[51];
    CHECK_EQ(first, rgb & 0xFFFFFF);
    printf("  screenshot: %s, %ld bytes, colour %06X\n", path, len, first);
    free(b);
}

/* the sound on disk, a tenth of a second at a time: full volume, then 80%, and silent at the end */
static void check_sound(void)
{
    long len = 0;
    int16_t *s = (int16_t *)slurp(sound, &len);
    CHECK(s != NULL);
    if (!s) return;
    long frames = len / 4, windows = frames / 4800;
    int peak[200] = { 0 }, top = 0, full = 0, eighty = 0;
    for (long w = 0; w < windows && w < 200; w++) {
        for (long i = w * 9600; i < (w + 1) * 9600; i++) if (abs(s[i]) > peak[w]) peak[w] = abs(s[i]);
        if (w > 0 && peak[w] > top) top = peak[w];       /* (the first tenth: the filters settling) */
    }
    for (long w = 0; w < windows && w < 200; w++) {
        full += abs(peak[w] - top) < top / 50;
        eighty += abs(peak[w] - top * 4 / 5) < top / 50;
    }
    printf("  sound: %.1f s, peak %d, %d tenths at full volume, %d at 80%%, the last %d and %d\n",
           frames / 48000.0, top, full, eighty, windows > 1 ? peak[windows - 2] : -1, windows > 0 ? peak[windows - 1] : -1);
    CHECK(top > 1000 && full >= 5 && eighty >= 5);
    CHECK(windows > 3 && peak[windows - 2] == 0 && peak[windows - 1] == 0);
    free(s);
}

static void check_settings(void)
{
    long len = 0;
    char *t = slurp(settings_path, &len);
    CHECK(t != NULL);
    if (!t) return;
    static const char *const want[] = {
        "# the test's own comment\n", "mystery.key = kept\n", "general.screenshots = ", "general.fast_forward = 4\n",
        "audio.volume = 80\n", "audio.mute = 1\n", "video.scale = integer\n", "video.aspect = 4:3\n",
        "pad1.start = Return\n", "pad2.a = Q\n", "key.screenshot = F12\n", "key.slow_motion = ,\n",
    };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++)
        if (!strstr(t, want[i])) { printf("  settings: no \"%s\"\n", want[i]); CHECK(0); }
    free(t);
}

int main(int argc, char **argv)
{
    dir = argc > 1 ? argv[1] : "build/sdltest";
    snprintf(settings_path, sizeof settings_path, "%s/settings.ini", dir);
    snprintf(shots, sizeof shots, "%s/shots", dir);
    snprintf(sound, sizeof sound, "%s/sound.raw", dir);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "disk", 1);
    SDL_setenv("SDL_DISKAUDIOFILE", sound, 1);
    remove(sound);
    DIR *d = opendir(shots);                              /* (last time's screenshots) */
    struct dirent *e;
    char path[1024];
    while (d && (e = readdir(d))) if (strstr(e->d_name, ".png")) { snprintf(path, sizeof path, "%s/%s", shots, e->d_name); remove(path); }
    if (d) closedir(d);
    char text[1024];
    snprintf(text, sizeof text, "# the test's own comment\nmystery.key = kept\ngeneral.screenshots = %s\n", shots);
    spit(settings_path, text);
    run = 1;
    CHECK_EQ(play(), 0);
    check_screenshot();
    check_sound();
    check_settings();
    /* by hand: Start on S, A on nothing, B on K (pad 2's down, now pad 1's too) */
    long len = 0;
    char *t = slurp(settings_path, &len), *edited = t ? malloc((size_t)len + 64) : NULL;
    if (edited) {
        const char *rules[][2] = { { "pad1.start = Return\n", "pad1.start = S\n" }, { "pad1.a = Z\n", "pad1.a =\n" }, { "pad1.b = X\n", "pad1.b = K\n" },
                                   { "key.quit = Escape\n", "key.quit = Q\n" } };
        strcpy(edited, t);
        for (size_t i = 0; i < 4; i++) {
            char *at = strstr(edited, rules[i][0]);
            CHECK(at != NULL);
            if (at) { memmove(at + strlen(rules[i][1]), at + strlen(rules[i][0]), strlen(at + strlen(rules[i][0])) + 1); memcpy(at, rules[i][1], strlen(rules[i][1])); }
        }
        spit(settings_path, edited);
    }
    free(t); free(edited);
    run = 2;
    CHECK_EQ(play(), 0);
    return check_done("platform_test");
}
