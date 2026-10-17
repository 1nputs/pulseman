/* The SDL2 platform played as a player would (make sdl-check): a thread presses keys (SDL events)
 * while a small game runs under SDL's dummy video and real-time disk sound. It checks the frame
 * rate at full speed, with fast-forward held and in slow motion; pause and a frame step; pausing
 * when the window loses focus (and the keys let go); a screenshot (its size and colour); the
 * volume and mute in the sound written to disk; the settings kept in the file in the directory it
 * runs in, with its comment and an unknown key; run again, keys bound in that file by hand; and
 * the menu: a ROM chosen in its browser (one the client turns down first), the upscaler (MMPX 4x
 * from then on, and on the next start) and the looks (the console's colours, dithering blended,
 * scanlines and a shadow mask, from then on too), the scaling and FM chip
 * changed, a reset, another ROM while playing, a key changed by pressing it, and the ROM chosen
 * last found again on the next start; and four players: EA's 4 Way Play in the ports, two virtual
 * controllers given to players 1 and 3, a button moved, a turbo button, the stick's dead zone, a
 * controller pulled out, the Players page and a controller button changed by pressing it; and the
 * save slots (F9, F10, the Save and Load pages, a save out of play that changes nothing) and quick
 * resume (the state at quitting, back at the next start; none when quitting out of play). */
#include <SDL2/SDL.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#define make_dir(d) _mkdir(d)
#define remove_dir(d) _rmdir(d)
#else
#include <unistd.h>
#define make_dir(d) mkdir(d, 0777)
#define remove_dir(d) rmdir(d)
#endif
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/platform_sdl2.h"
#include "mdrt/png.h"

#define BACKDROP 0x0E42                                   /* the picture: the backdrop's colour */

static const char *const shots = "shots", *const sound = "sound.raw", *const settings_path = "settings.ini";
static SDL_atomic_t frame, held, starts, fm_second, pads4[MDRT_PADS], adaptor, game_frame, refuse_saves;
static int run;
static char rom_seen[16];                                 /* the start of the ROM the client was given */

static void quit_event(void)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}

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
        quit_event();
        return 0;
    }
    double normal = rate(1000);
    key(SDL_SCANCODE_F6, 1);
    double fast = rate(500);
    key(SDL_SCANCODE_F6, 0);
    press(SDL_SCANCODE_F7);
    double slow = rate(1000);
    press(SDL_SCANCODE_F7);
    printf("  frames a second: %.1f, fast-forward %.1f, slow motion %.1f\n", normal, fast, slow);
    CHECK(normal > 50 && normal < 70);
#ifndef __SANITIZE_ADDRESS__                             /* (the sanitizers' build runs no faster than about 65) */
    CHECK(fast > normal * 3);                             /* (4 times, in the settings) */
#endif
    CHECK(slow > normal * 0.4 && slow < normal * 0.6);
    press(SDL_SCANCODE_F12);
    press(SDL_SCANCODE_F2);
    press(SDL_SCANCODE_F2);
    SDL_Delay(1000);
    /* pause, two frames stepped, on again */
    press(SDL_SCANCODE_F5);
    SDL_Delay(300);
    int c0 = SDL_AtomicGet(&frame);
    SDL_Delay(300);
    CHECK_EQ(SDL_AtomicGet(&frame), c0);
    press(SDL_SCANCODE_F8);
    SDL_Delay(200);
    CHECK_EQ(SDL_AtomicGet(&frame), c0 + 1);
    press(SDL_SCANCODE_F8);
    SDL_Delay(200);
    CHECK_EQ(SDL_AtomicGet(&frame), c0 + 2);
    press(SDL_SCANCODE_F5);
    CHECK(rate(300) > 40);
    /* the window loses focus with fast-forward held: paused, and the key let go */
    key(SDL_SCANCODE_F6, 1);
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
    key(SDL_SCANCODE_F6, 0);
    press(SDL_SCANCODE_F1);
    SDL_Delay(800);
    quit_event();
    return 0;
}

/* the menu, as a player works it: run 3 starts with no ROM it can find */
static void keys(SDL_Scancode k, int n) { for (int i = 0; i < n; i++) { press(k); SDL_Delay(40); } }

static int menu_player(void *user)
{
    (void)user;
    while (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_Delay(10);
    SDL_Delay(500);
    if (run == 4) {                                       /* the ROM chosen last: playing at once */
        SDL_Delay(300);
        CHECK_EQ(SDL_AtomicGet(&starts), 1);
        CHECK(!strcmp(rom_seen, "GOOD-other"));
        quit_event();
        return 0;
    }
    CHECK_EQ(SDL_AtomicGet(&starts), 0);                  /* the ROM page: [.., roms/, shots/, bad.gen, good.gen] */
    keys(SDL_SCANCODE_RETURN, 1);                         /* bad.gen, selected first: turned down */
    SDL_Delay(300);
    CHECK_EQ(SDL_AtomicGet(&starts), 0);
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RETURN, 1);                         /* good.gen: the game starts */
    SDL_Delay(500);
    CHECK_EQ(SDL_AtomicGet(&starts), 1);
    CHECK(!strcmp(rom_seen, "GOOD-good"));
    CHECK(SDL_AtomicGet(&frame) > 10);
    /* the menu: [Resume, Game ROM, Reset, Save, Load, Video, Sound, Options, Keys, Quit] */
    keys(SDL_SCANCODE_ESCAPE, 1);
    SDL_Delay(200);
    int f0 = SDL_AtomicGet(&frame);
    SDL_Delay(300);
    CHECK_EQ(SDL_AtomicGet(&frame), f0);                  /* the game waits */
    keys(SDL_SCANCODE_DOWN, 5);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Video: [Upscaler, Scaling, Aspect, Colours, Blend, Scanlines, Mask, Full screen, Back] */
    keys(SDL_SCANCODE_RIGHT, 4);                          /* off -> scale2x -> scale3x -> mmpx2x -> mmpx4x (from here on) */
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RIGHT, 1);                          /* integer (run 1's file) -> fit */
    keys(SDL_SCANCODE_DOWN, 2);
    keys(SDL_SCANCODE_RIGHT, 2);                          /* the colours: standard -> full range -> the console's */
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RIGHT, 1);                          /* blend dithering (and from here on, with: */
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RIGHT, 2);                          /* medium scanlines */
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_LEFT, 1);                           /* and a shadow mask: off, round to the last) */
    keys(SDL_SCANCODE_ESCAPE, 1);
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Sound: [Volume, Mute, FM core, FM chip, Back] */
    keys(SDL_SCANCODE_DOWN, 2);
    keys(SDL_SCANCODE_RIGHT, 1);                          /* the next FM core */
    keys(SDL_SCANCODE_ESCAPE, 2);                         /* back, and out */
    SDL_Delay(300);
    CHECK(SDL_AtomicGet(&frame) > f0 + 5);
    CHECK_EQ(SDL_AtomicGet(&fm_second), mdrt_fm_cores(1) != NULL);   /* (changed between frames) */
    keys(SDL_SCANCODE_ESCAPE, 1);
    keys(SDL_SCANCODE_DOWN, 2);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Reset: chosen once, asked again */
    SDL_Delay(200);
    CHECK_EQ(SDL_AtomicGet(&starts), 1);
    keys(SDL_SCANCODE_RETURN, 1);
    SDL_Delay(400);
    CHECK_EQ(SDL_AtomicGet(&starts), 2);
    f0 = SDL_AtomicGet(&frame);
    SDL_Delay(200);
    CHECK(SDL_AtomicGet(&frame) > f0);                    /* playing again, the menu closed */
    keys(SDL_SCANCODE_ESCAPE, 1);
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Game ROM: good.gen selected */
    keys(SDL_SCANCODE_UP, 3);
    keys(SDL_SCANCODE_RETURN, 1);                         /* roms/: [.., other.gen] */
    keys(SDL_SCANCODE_RETURN, 2);                         /* other.gen, chosen again: the game starts with it */
    SDL_Delay(400);
    CHECK_EQ(SDL_AtomicGet(&starts), 3);
    CHECK(!strcmp(rom_seen, "GOOD-other"));
    keys(SDL_SCANCODE_ESCAPE, 1);
    keys(SDL_SCANCODE_DOWN, 9);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Keys: pad 1's up, down ... */
    keys(SDL_SCANCODE_DOWN, 7);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Pad 1 Start: press the new key */
    keys(SDL_SCANCODE_HOME, 1);
    keys(SDL_SCANCODE_ESCAPE, 2);
    SDL_Delay(200);
    key(SDL_SCANCODE_HOME, 1);
    SDL_Delay(200);
    CHECK_EQ(SDL_AtomicGet(&held) & MDRT_PAD_START, MDRT_PAD_START);
    key(SDL_SCANCODE_HOME, 0);
    quit_event();
    return 0;
}

/* run 5: two virtual controllers (SDL's standard layout), plugged in while the game runs */
static void vbutton(SDL_Joystick *j, SDL_GameControllerButton b, int on) { SDL_JoystickSetVirtualButton(j, b, (Uint8)on); SDL_Delay(120); }

static int players(void *user)
{
    (void)user;
    until_frame(20);
    int i1 = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    SDL_Joystick *c1 = SDL_JoystickOpen(i1);
    int i2 = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    SDL_Joystick *c2 = SDL_JoystickOpen(i2);
    SDL_Delay(300);
    CHECK_EQ(SDL_AtomicGet(&adaptor), MDRT_ADAPTOR_EA4WAY);
    vbutton(c1, SDL_CONTROLLER_BUTTON_Y, 1);              /* controller 1 is player 3's; C is on Y */
    CHECK_EQ(SDL_AtomicGet(&pads4[2]) & 0xFF, MDRT_PAD_C);   /* (Y too: still the pad's Y, which a 3-button pad lacks) */
    CHECK_EQ(SDL_AtomicGet(&pads4[0]), 0);
    vbutton(c1, SDL_CONTROLLER_BUTTON_Y, 0);
    vbutton(c2, SDL_CONTROLLER_BUTTON_DPAD_UP, 1);        /* controller 2 is player 1's */
    CHECK_EQ(SDL_AtomicGet(&pads4[0]), MDRT_PAD_UP);
    vbutton(c2, SDL_CONTROLLER_BUTTON_DPAD_UP, 0);
    SDL_JoystickSetVirtualAxis(c2, SDL_CONTROLLER_AXIS_LEFTX, 20000);   /* 61%: past the dead zone (50%) */
    SDL_Delay(120);
    CHECK_EQ(SDL_AtomicGet(&pads4[0]), MDRT_PAD_RIGHT);
    SDL_JoystickSetVirtualAxis(c2, SDL_CONTROLLER_AXIS_LEFTX, 12000);   /* 37%: inside it */
    SDL_Delay(120);
    CHECK_EQ(SDL_AtomicGet(&pads4[0]), 0);
    SDL_JoystickSetVirtualAxis(c2, SDL_CONTROLLER_AXIS_LEFTX, 0);
    SDL_JoystickSetVirtualButton(c1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);   /* turbo B: on and off, ten a second */
    int changes = 0, last = 0;
    for (int f = SDL_AtomicGet(&frame), k = 0; k < 60; k++) {
        while (SDL_AtomicGet(&frame) == f) SDL_Delay(1);
        f = SDL_AtomicGet(&frame);
        int b = (SDL_AtomicGet(&pads4[2]) & MDRT_PAD_B) != 0;
        changes += b != last;
        last = b;
    }
    SDL_JoystickSetVirtualButton(c1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0);
    printf("  turbo: %d changes of B in 60 frames\n", changes);
    CHECK(changes >= 12 && changes <= 30);
    SDL_JoystickClose(c2);                                /* controller 2 pulled out: controller 1 keeps its slot */
    SDL_JoystickDetachVirtual(i2);
    SDL_Delay(200);
    vbutton(c1, SDL_CONTROLLER_BUTTON_Y, 1);
    CHECK_EQ(SDL_AtomicGet(&pads4[2]) & 0xFF, MDRT_PAD_C);   /* (Y too: still the pad's Y, which a 3-button pad lacks) */
    vbutton(c1, SDL_CONTROLLER_BUTTON_Y, 0);
    /* the menu: [Resume, Reset, Save, Load, Video, Sound, Players, Options, Keys, Quit] */
    keys(SDL_SCANCODE_ESCAPE, 1);
    keys(SDL_SCANCODE_DOWN, 6);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Players: [Controllers, Player 1-4, Controller buttons ...] */
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RIGHT, 1);                          /* player 1: controller 2 -> 3 */
    keys(SDL_SCANCODE_DOWN, 4);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Controller buttons: [A, B, C ...] */
    keys(SDL_SCANCODE_RETURN, 1);                         /* A: press the button */
    vbutton(c1, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
    vbutton(c1, SDL_CONTROLLER_BUTTON_LEFTSTICK, 0);
    keys(SDL_SCANCODE_ESCAPE, 3);
    SDL_Delay(200);
    SDL_JoystickClose(c1);
    SDL_JoystickDetachVirtual(i1);
    quit_event();
    return 0;
}

/* runs 6 and 7: the save slots and quick resume */
static int state_in(const char *path)
{
    FILE *f = fopen(path, "rb");
    int n = -1;
    if (f && fscanf(f, "STATE %d", &n) != 1) n = -1;
    if (f) fclose(f);
    return n;
}

static int exists(const char *path) { FILE *f = fopen(path, "rb"); if (f) fclose(f); return f != NULL; }

static int slots(void *user)
{
    (void)user;
    until_frame(20);
    if (run == 7) {                                       /* started again: back where run 6 quit */
        int was = state_in("states/resume.state.seen");
        printf("  quick resume: the game at %d, run 6 quit at %d\n", SDL_AtomicGet(&game_frame), was);
        CHECK(was > 0 && SDL_AtomicGet(&game_frame) >= was && SDL_AtomicGet(&game_frame) < was + 60);
        SDL_AtomicSet(&refuse_saves, 1);                  /* quitting out of play: no state to come back to */
        quit_event();
        return 0;
    }
    press(SDL_SCANCODE_F9);                               /* slot 1 */
    SDL_Delay(500);
    int s1 = state_in("states/slot1.state");
    CHECK(s1 > 0 && exists("states/slot1.png") && !exists("states/slot1.state.new"));
    /* the menu: [Resume, Reset, Save a state, Load the state, Video ...]; Save: [slot 1-8, Back] */
    keys(SDL_SCANCODE_ESCAPE, 1);
    keys(SDL_SCANCODE_DOWN, 2);
    keys(SDL_SCANCODE_RETURN, 1);
    keys(SDL_SCANCODE_DOWN, 1);
    keys(SDL_SCANCODE_RETURN, 1);                         /* slot 2 */
    SDL_Delay(500);
    int s2 = state_in("states/slot2.state");
    CHECK(s2 > s1 && exists("states/slot2.png"));
    SDL_Delay(300);
    press(SDL_SCANCODE_F10);                              /* slot 2 again: the one chosen last */
    SDL_Delay(200);
    CHECK(SDL_AtomicGet(&game_frame) >= s2 && SDL_AtomicGet(&game_frame) < s2 + 20);
    keys(SDL_SCANCODE_ESCAPE, 1);
    keys(SDL_SCANCODE_DOWN, 3);
    keys(SDL_SCANCODE_RETURN, 1);                         /* Load: slot 2 selected */
    keys(SDL_SCANCODE_UP, 1);
    keys(SDL_SCANCODE_RETURN, 1);                         /* slot 1 */
    SDL_Delay(200);
    CHECK(SDL_AtomicGet(&game_frame) >= s1 && SDL_AtomicGet(&game_frame) < s1 + 20);
    SDL_AtomicSet(&refuse_saves, 1);                      /* out of play: slot 1 as it was */
    press(SDL_SCANCODE_F9);
    SDL_Delay(1800);
    CHECK_EQ(state_in("states/slot1.state"), s1);
    CHECK(!exists("states/slot1.state.new"));
    SDL_AtomicSet(&refuse_saves, 0);
    quit_event();                                         /* quick resume: the state at quitting */
    return 0;
}

static const char *rom_check(const uint8_t *rom, size_t size, void *user)
{
    (void)user;
    return size >= 4 && !memcmp(rom, "GOOD", 4) ? NULL : "not a ROM this game can run";
}

static void rom_use(const uint8_t *rom, size_t size, void *user)
{
    (void)user;
    snprintf(rom_seen, sizeof rom_seen, "%.*s", (int)(size < 15 ? size : 15), (const char *)rom);
}

static void game(mdrt *m, void *user)
{
    (void)user;
    SDL_AtomicAdd(&starts, 1);
    mdrt_vdp *v = mdrt_get_vdp(m);
    static const uint8_t regs[] = { 0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
    for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
    mdrt_vdp_ctrl32(v, 0xC0000000);
    mdrt_vdp_data16(v, BACKDROP);
    mdrt_psg_port(m, 0, 0x80 | (254 & 15));               /* a 440 Hz square, loud */
    mdrt_psg_port(m, 100, 254 >> 4);
    mdrt_psg_port(m, 200, 0x90);
    SDL_Thread *t = run <= 2 ? SDL_CreateThread(player, "player", NULL) : run == 5 ? SDL_CreateThread(players, "players", NULL)
                  : run >= 6 ? SDL_CreateThread(slots, "slots", NULL) : NULL;
    for (;;) {
        mdrt_vblank_end(m);
        if (mdrt_wait_vblank(m)) break;
        SDL_AtomicAdd(&game_frame, 1);                    /* the game's own count: what a state holds */
        unsigned req = mdrt_requests(m);
        const char *path = mdrt_request_path(m);
        if ((req & MDRT_REQUEST_SAVE) && path && !SDL_AtomicGet(&refuse_saves)) {
            FILE *f = fopen(path, "wb");
            if (f) { fprintf(f, "STATE %d\n", SDL_AtomicGet(&game_frame)); fclose(f); }
        }
        if ((req & MDRT_REQUEST_LOAD) && path) {
            FILE *f = fopen(path, "rb");
            int n;
            if (f && fscanf(f, "STATE %d", &n) == 1) SDL_AtomicSet(&game_frame, n);
            if (f) fclose(f);
        }
        SDL_AtomicSet(&held, mdrt_get_pads(m)->buttons[0]);
        for (int p = 0; p < MDRT_PADS; p++) SDL_AtomicSet(&pads4[p], mdrt_get_pads(m)->buttons[p]);
        SDL_AtomicSet(&adaptor, mdrt_get_pads(m)->adaptor);
        SDL_AtomicSet(&fm_second, mdrt_fm_cores(1) && !strcmp(mdrt_fm_core_of(mdrt_get_fm(m)), mdrt_fm_cores(1)));
        SDL_AtomicAdd(&frame, 1);
    }
    if (t) SDL_WaitThread(t, NULL);
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
    SDL_AtomicSet(&starts, 0);
    mdrt_config c = { 0 };
    c.audio_rate = 48000;
    mdrt_sdl_options o = { .title = "platform test", .app = "platform-test" };   /* (settings.ini here) */
    SDL_Thread *t = NULL;
    if (run == 3 || run == 4) {
        o.rom_default = "missing.gen";
        o.rom_check = rom_check;
        o.rom_use = rom_use;
        t = SDL_CreateThread(menu_player, "menu player", NULL);
    }
    int r = mdrt_sdl_run(&c, &o, game, NULL);
    if (t) SDL_WaitThread(t, NULL);
    return r;
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
        "# the test's own comment\n", "mystery.key = kept\n", "general.screenshots = shots\n", "general.fast_forward = 4\n",
        "audio.volume = 80\n", "audio.mute = 1\n", "video.scale = integer\n", "video.aspect = square\n",
        "pad1.start = Return\n", "pad2.a = Q\n", "key.screenshot = F12\n", "key.slow_motion = F7\n", "key.pause = F5\n", "key.save_state = F9\n", "key.mute = F1\n",
        "key.menu = Escape\n", "game.rom =\n",
    };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++)
        if (!strstr(t, want[i])) { printf("  settings: no \"%s\"\n", want[i]); CHECK(0); }
    free(t);
}

int main(int argc, char **argv)
{
    if (chdir(argc > 1 ? argv[1] : "build/sdltest")) { perror("chdir"); return 1; }   /* (the game's directory) */
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "disk", 1);
    SDL_setenv("SDL_DISKAUDIOFILE", sound, 1);
    remove(sound);
    for (int i = 0; i <= 8; i++)                          /* (last time's save slots: the ROM page lists folders) */
        for (int k = 0; k < 4; k++) {
            static const char *const ext[4] = { ".state", ".png", ".state.new", ".state.seen" };
            char p[64];
            if (i) snprintf(p, sizeof p, "states/slot%d%s", i, ext[k]);
            else snprintf(p, sizeof p, "states/resume%s", ext[k]);
            remove(p);
        }
    remove_dir("states");
    DIR *d = opendir(shots);                              /* (last time's screenshots) */
    struct dirent *e;
    char path[1024];
    while (d && (e = readdir(d))) if (strstr(e->d_name, ".png")) { snprintf(path, sizeof path, "%s/%s", shots, e->d_name); remove(path); }
    if (d) closedir(d);
    char text[1024];
    snprintf(text, sizeof text, "# the test's own comment\nmystery.key = kept\ngeneral.screenshots = %s\nvideo.scale = integer\ngeneral.resume = 0\n", shots);
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
        const char *rules[][2] = { { "pad1.start = Return\n", "pad1.start = S\n" }, { "pad1.a = Z\n", "pad1.a =\n" }, { "pad1.b = X\n", "pad1.b = K\n" } };
        strcpy(edited, t);
        for (size_t i = 0; i < 3; i++) {
            char *at = strstr(edited, rules[i][0]);
            CHECK(at != NULL);
            if (at) { memmove(at + strlen(rules[i][1]), at + strlen(rules[i][0]), strlen(at + strlen(rules[i][0])) + 1); memcpy(at, rules[i][1], strlen(rules[i][1])); }
        }
        spit(settings_path, edited);
    }
    free(t); free(edited);
    run = 2;
    CHECK_EQ(play(), 0);
    /* the menu: ROMs the test's client checks ("GOOD" at the start), one in a folder */
    make_dir("roms");
    spit("bad.gen", "BAD!");
    spit("good.gen", "GOOD-good");
    spit("roms/other.gen", "GOOD-other");
    run = 3;
    CHECK_EQ(play(), 0);
    t = slurp(settings_path, &len);
    CHECK(t && strstr(t, "game.rom = roms/other.gen\n") && strstr(t, "video.scale = fit\n") && strstr(t, "pad1.start = Home\n"));
    CHECK(t && strstr(t, "video.upscaler = mmpx4x\n") && strstr(t, "video.colours = console\n") && strstr(t, "video.blend = dither\n"));
    CHECK(t && strstr(t, "video.scanlines = medium\n") && strstr(t, "video.mask = dot\n"));
    if (t && mdrt_fm_cores(1)) { char want[64]; snprintf(want, sizeof want, "audio.fm_core = %s\n", mdrt_fm_cores(1)); CHECK(strstr(t, want) != NULL); }
    free(t);
    run = 4;
    CHECK_EQ(play(), 0);
    /* four players: the adaptor, two controllers given to players 1 and 3, C on Y, turbo B */
    t = slurp(settings_path, &len);
    if (t) {
        static const char *const old[] = { "input.adaptor = none\n", "player1.controller = 1\n", "player3.controller = 3\n",
                                           "controller.c = b\n", "controller.turbo_b =\n" };
        for (size_t i = 0; i < sizeof old / sizeof *old; i++) {   /* (made comments; the new values follow) */
            char *at = strstr(t, old[i]);
            if (at) *at = '#';
        }
        char *ed = malloc((size_t)len + 512);
        snprintf(ed, (size_t)len + 512, "%sinput.adaptor = ea4way\nplayer1.controller = 2\nplayer3.controller = 1\ncontroller.c = y\n"
                 "controller.turbo_b = rightshoulder\n", t);
        spit(settings_path, ed);
        free(ed);
    }
    free(t);
    run = 5;
    CHECK_EQ(play(), 0);
    t = slurp(settings_path, &len);
    CHECK(t && strstr(t, "\nplayer1.controller = 3\n") && strstr(t, "\ncontroller.a = leftstick\n"));
    free(t);
    /* save slots and quick resume */
    for (int i = 0; i <= 2; i++) {                        /* (last time's) */
        char p[64];
        snprintf(p, sizeof p, i ? "states/slot%d.state" : "states/resume.state", i);
        remove(p);
    }
    t = slurp(settings_path, &len);                       /* quick resume on, from here */
    if (t) {
        char *at = strstr(t, "general.resume = 0\n");
        if (at) at[17] = '1';
        spit(settings_path, t);
        free(t);
    }
    SDL_AtomicSet(&game_frame, 0);
    run = 6;
    CHECK_EQ(play(), 0);
    int quit_at = state_in("states/resume.state");
    CHECK(quit_at > 0);
    t = slurp("states/resume.state", &len);
    if (t) { spit("states/resume.state.seen", t); free(t); }
    SDL_AtomicSet(&game_frame, 0);
    SDL_AtomicSet(&refuse_saves, 0);
    run = 7;
    CHECK_EQ(play(), 0);
    CHECK(!exists("states/resume.state"));                /* (quit out of play: none to come back to) */
    return check_done("platform_test");
}
