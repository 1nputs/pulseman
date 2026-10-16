/* The SDL2 platform (mdrt/platform_sdl2.h). The game thread hands each picture over through a
 * mutex and queues its sound itself; the main thread draws, handles the keys, the window and the
 * menu, keeps the settings, and starts and stops the game (a reset, a new ROM). */
#include <SDL2/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(d) _mkdir(d)
#define get_cwd _getcwd
#define SEP '\\'
#else
#include <unistd.h>
#define make_dir(d) mkdir(d, 0777)
#define get_cwd getcwd
#define SEP '/'
#endif
#include "mdrt/menu.h"
#include "mdrt/platform_sdl2.h"
#include "mdrt/png.h"
#include "mdrt/settings.h"
#include "mdrt/view.h"

/* ---- the keys: what each does, its setting and its default ------------------------------------ */

enum { K_MUTE, K_VOL_DOWN, K_VOL_UP, K_REWIND, K_SLOW, K_FAST, K_SAVE, K_LOAD, K_STEP, K_PAUSE,
       K_FULLSCREEN, K_SHOT, K_MENU, K_COUNT };
static const char *const key_setting[K_COUNT][3] = {     /* (the controls on the function keys, in order) */
    { "key.mute", "F1", "Mute" }, { "key.volume_down", "F2", "Volume down" }, { "key.volume_up", "F3", "Volume up" },
    { "key.rewind", "F4", "Rewind (held)" }, { "key.slow_motion", "F5", "Slow motion" },
    { "key.fast_forward", "F6", "Fast-forward (held)" }, { "key.save_state", "F7", "Save a state" },
    { "key.load_state", "F8", "Load the state" }, { "key.frame_step", "F9", "A frame, while paused" },
    { "key.pause", "F10", "Pause" }, { "key.fullscreen", "F11", "Full screen" }, { "key.screenshot", "F12", "Screenshot" },
    { "key.menu", "Escape", "Menu" },
};
enum { BUTTONS = 12 };
static const char *const button_name[BUTTONS] = { "up", "down", "left", "right", "a", "b", "c", "start", "x", "y", "z", "mode" };
static const char *const button_label[BUTTONS] = { "up", "down", "left", "right", "A", "B", "C", "Start", "X", "Y", "Z", "Mode" };
static const uint16_t button_bit[BUTTONS] = { MDRT_PAD_UP, MDRT_PAD_DOWN, MDRT_PAD_LEFT, MDRT_PAD_RIGHT, MDRT_PAD_A, MDRT_PAD_B,
                                              MDRT_PAD_C, MDRT_PAD_START, MDRT_PAD_X, MDRT_PAD_Y, MDRT_PAD_Z, MDRT_PAD_MODE };
static const char *const button_default[2][BUTTONS] = {
    { "Up", "Down", "Left", "Right", "Z", "X", "C", "Return", "", "", "", "" },
    { "I", "K", "J", "L", "Q", "W", "E", "Tab", "", "", "", "" },
};
static const SDL_GameControllerButton pad_button[8] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_START,
};
static const char *const scale_name[3] = { "sharp", "integer", "fit" };
static const char *const aspect_name[2] = { "square", "4:3" };
static const char *const aspect_label[2] = { "square pixels", "4:3" };
static const char *const off_on[2] = { "off", "on" };
static const char *const chip_name[2] = { "YM2612", "YM3438" };
static const int fast_speeds[6] = { 2, 3, 4, 6, 8, 0 };
static const char *const fast_label[6] = { "2x", "3x", "4x", "6x", "8x", "as fast as it goes" };

#define VOLUME_STEP 10
#define ROM_MAX (16u << 20)                               /* (no cartridge is larger) */

/* the menu's pages and items */
enum { PAGE_MAIN, PAGE_VIDEO, PAGE_SOUND, PAGE_OPTIONS, PAGE_KEYS, PAGE_ROM };
enum { I_RESUME = 1, I_ROM, I_RESET, I_SAVE, I_LOAD, I_VIDEO, I_SOUND, I_OPTIONS, I_KEYS, I_QUIT, I_BACK,
       I_SCALE, I_ASPECT, I_FULL, I_VOLUME, I_MUTE, I_FM_CORE, I_FM_CHIP, I_FOCUS, I_FAST, I_UP,
       I_PAD = 1000,             /* + pad * BUTTONS + button */
       I_CONTROL = 2000,         /* + K_ */
       I_ENTRY = 10000 };        /* + a directory entry's index */

typedef struct sdl {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex, *pre;     /* the picture; sharp scaling's enlarged copy */
    int pre_w, pre_h, targets;
    SDL_AudioDeviceID dev;
    uint32_t rate;
    int queue_frames;           /* sound queued before the next frame goes (the latency) */
    SDL_atomic_t fast_speed;    /* fast-forward's times the speed (0: as fast as it goes) */
    SDL_mutex *lock;
    uint32_t picture[320 * 240];
    int width, height, fresh, redraw;
    SDL_atomic_t buttons[2], quit, stop, paused, in_menu, done;
    SDL_atomic_t saves, loads, rewind;      /* presses of the state keys since the game last asked; held */
    SDL_atomic_t fast, slow, step, volume, mute;
    SDL_atomic_t fm_want;       /* 1 + the FM core's index x 2 + the model: for the game thread to make */
    SDL_GameController *pads[2];
    const mdrt_sdl_options *o;
    mdrt_config *cfg;
    mdrt *m;
    SDL_Thread *thread;
    mdrt_game_fn game;
    void *user;
    uint64_t next_tick;         /* pacing by the clock */
    unsigned waits, queued;
    int16_t *loud;              /* the frame's sound at the volume */
    size_t loud_cap;
    /* the main thread's */
    mdrt_settings *set;
    const char *set_path;
    uint32_t set_saved;         /* SDL_GetTicks of the last write */
    SDL_Scancode key[K_COUNT], button_key[2][BUTTONS];
    uint8_t down[SDL_NUM_SCANCODES];
    uint16_t pad_ignore[2];     /* controller buttons held when the menu closed, until let go */
    int scale, aspect, focus_pause, auto_paused;
    char title[200];
    uint32_t notice_until;
    uint8_t *rom;               /* the game's ROM (given to the client by rom_use) */
    size_t rom_size;
    char rom_name[1024];        /* its path, as the settings keep it */
    /* the menu */
    mdrt_menu menu;
    int menu_open, page, confirm, binding;
    uint32_t menu_px[320 * 240];
    char dir[1024];             /* the ROM page's directory */
    char **entries;             /* its folders (ending in the separator) and ROM files */
    int nentries;
    const char *core_label[8];
    int ncores;
} sdl;

/* ---- the game thread's side --------------------------------------------------------------- */

static void sram_save(sdl *s)
{
    mdrt_sram *r = mdrt_get_sram(s->m);
    if (!s->o->sram_path || !r->size || !r->dirty) return;
    FILE *f = fopen(s->o->sram_path, "wb");
    if (!f) return;
    if (fwrite(r->bytes, 1, r->size, f) == r->size) r->dirty = 0;
    fclose(f);
}

static void sram_load(sdl *s)
{
    mdrt_sram *r = mdrt_get_sram(s->m);
    if (!s->o->sram_path || !r->size) return;
    FILE *f = fopen(s->o->sram_path, "rb");
    if (!f) return;
    if (fread(r->bytes, 1, r->size, f) != r->size) fprintf(stderr, "%s: short save RAM file\n", s->o->sram_path);
    fclose(f);
}

static void present(void *user, const uint16_t *px, int w, int h, int pitch)
{
    sdl *s = user;
    SDL_LockMutex(s->lock);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) s->picture[y * 320 + x] = 0xFF000000u | mdrt_vdp_rgb(px[y * pitch + x], 1);
    s->width = w;
    s->height = h;
    s->fresh = 1;
    SDL_UnlockMutex(s->lock);
}

static int held(sdl *s) { return SDL_AtomicGet(&s->paused) || SDL_AtomicGet(&s->in_menu); }

/* the frame's sound, at the volume; none while fast, slow, paused or in the menu (the clock paces those) */
static void audio(void *user, const int16_t *samples, int frames)
{
    sdl *s = user;
    if (!s->dev || SDL_AtomicGet(&s->fast) || SDL_AtomicGet(&s->slow) || held(s)) return;
    int vol = SDL_AtomicGet(&s->mute) ? 0 : SDL_AtomicGet(&s->volume);
    if (vol < 100) {
        size_t n = (size_t)frames * 2;
        if (n > s->loud_cap) {
            int16_t *b = realloc(s->loud, n * sizeof *b);
            if (!b) return;
            s->loud = b;
            s->loud_cap = n;
        }
        for (size_t i = 0; i < n; i++) s->loud[i] = (int16_t)(samples[i] * vol / 100);
        samples = s->loud;
    }
    SDL_QueueAudio(s->dev, samples, (Uint32)frames * 4);
    if (++s->queued == (unsigned)s->queue_frames) SDL_PauseAudioDevice(s->dev, 0);   /* start once there is some */
}

static int ending(sdl *s) { return SDL_AtomicGet(&s->quit) || SDL_AtomicGet(&s->stop); }

static int wait(void *user)
{
    sdl *s = user;
    if (++s->waits % 300 == 0) sram_save(s);              /* every 5 s when written */
    while (held(s) && !ending(s)) {
        if (!SDL_AtomicGet(&s->in_menu) && SDL_AtomicSet(&s->step, 0)) break;   /* one frame on */
        SDL_Delay(5);
    }
    int fm = SDL_AtomicSet(&s->fm_want, 0);               /* the player's FM chip, between frames */
    if (fm) mdrt_set_fm(s->m, mdrt_fm_cores((fm - 1) / 2), (fm - 1) % 2);
    int fast = SDL_AtomicGet(&s->fast), slow = SDL_AtomicGet(&s->slow), speed = SDL_AtomicGet(&s->fast_speed);
    if (s->dev && !fast && !slow) {                       /* the sound */
        if (s->queued >= (unsigned)s->queue_frames) {
            Uint32 enough = (Uint32)((unsigned)s->queue_frames * s->rate / 60 * 4);
            while (SDL_GetQueuedAudioSize(s->dev) > enough && !ending(s)) SDL_Delay(1);
        }
        s->next_tick = 0;
    } else if (!fast || speed) {                          /* no sound, slow motion, fast-forward: the clock */
        uint64_t freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
        uint64_t period = freq * mdrt_frame_mclks(s->m) / mdrt_master_clock(s->m);
        if (slow) period *= 2;
        if (fast) period /= (uint64_t)speed;
        if (!s->next_tick || now > s->next_tick + period * 4) s->next_tick = now;
        s->next_tick += period;
        while ((now = SDL_GetPerformanceCounter()) < s->next_tick && !ending(s)) {
            uint64_t ms = (s->next_tick - now) * 1000 / freq;
            if (ms > 1) SDL_Delay((Uint32)(ms - 1));
        }
    } else s->next_tick = 0;
    return ending(s);
}

static void input(void *user, uint16_t buttons[2])
{
    sdl *s = user;
    buttons[0] = (uint16_t)SDL_AtomicGet(&s->buttons[0]);
    buttons[1] = (uint16_t)SDL_AtomicGet(&s->buttons[1]);
}

static unsigned requests(void *user)
{
    sdl *s = user;
    unsigned r = 0;
    if (SDL_AtomicSet(&s->saves, 0)) r |= MDRT_REQUEST_SAVE;
    if (SDL_AtomicSet(&s->loads, 0)) r |= MDRT_REQUEST_LOAD;
    if (SDL_AtomicGet(&s->rewind)) r |= MDRT_REQUEST_REWIND;
    return r;
}

static int game_thread(void *user)
{
    sdl *s = user;
    sram_load(s);
    s->game(s->m, s->user);
    sram_save(s);
    SDL_AtomicSet(&s->done, 1);
    return 0;
}

/* ---- the game: started, stopped -------------------------------------------------------------- */

/* a new console and the game on it, from the start */
static int start_game(sdl *s)
{
    s->m = mdrt_new(s->cfg);
    if (!s->m) return 0;
    s->waits = s->queued = 0;
    s->next_tick = 0;
    if (s->dev) { SDL_PauseAudioDevice(s->dev, 1); SDL_ClearQueuedAudio(s->dev); }
    SDL_AtomicSet(&s->done, 0);
    SDL_AtomicSet(&s->stop, 0);
    SDL_AtomicSet(&s->fm_want, 0);
    s->thread = SDL_CreateThread(game_thread, "game", s);
    if (!s->thread) { mdrt_free(s->m); s->m = NULL; }
    return s->thread != NULL;
}

/* the game asked to return (mdrt_wait_vblank nonzero) and its console freed; 0 when it would not
 * stop in 3 seconds (then it is left running: the process ends it) */
static int stop_game(sdl *s)
{
    if (!s->thread) return 1;
    SDL_AtomicSet(&s->stop, 1);
    for (int i = 0; i < 300 && !SDL_AtomicGet(&s->done); i++) SDL_Delay(10);
    if (!SDL_AtomicGet(&s->done)) return 0;
    SDL_WaitThread(s->thread, NULL);
    s->thread = NULL;
    mdrt_free(s->m);
    s->m = NULL;
    SDL_AtomicSet(&s->stop, 0);
    return 1;
}

/* ---- the ROM ------------------------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *size, const char **why)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "cannot open it"; return NULL; }
    uint8_t *b = NULL;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (n <= 0 || (unsigned long)n > ROM_MAX) *why = n <= 0 ? "it is empty, or not a file" : "it is too large for a cartridge";
    else if (fseek(f, 0, SEEK_SET) || !(b = malloc((size_t)n))) *why = "cannot read it";
    else if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; *why = "cannot read it"; }
    fclose(f);
    *size = b ? (size_t)n : 0;
    return b;
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* the client's verdict on a file (NULL: it will do); with use, it becomes the game's ROM (only
 * while the game is stopped) */
static const char *try_rom(sdl *s, const char *path, int use)
{
    size_t n;
    const char *why = NULL;
    uint8_t *b = read_file(path, &n, &why);
    if (!b) return why;
    why = s->o->rom_check(b, n, s->user);
    if (why || !use) { free(b); return why; }
    if (s->o->rom_use) s->o->rom_use(b, n, s->user);
    free(s->rom);
    s->rom = b;
    s->rom_size = n;
    snprintf(s->rom_name, sizeof s->rom_name, "%s", path);
    return NULL;
}

/* this run's ROM: the command line's, else the one chosen last, else the client's default; when
 * none will do, why the one given or chosen would not, or why the default's file would not (one
 * that is not there says nothing: the menu's hint does) */
static int first_rom(sdl *s, char *msg, size_t size)
{
    const char *tries[2] = { s->o->rom_path, NULL };
    const char *chosen = mdrt_settings_get(s->set, "game.rom");
    if (!tries[0]) { tries[0] = chosen && *chosen ? chosen : s->o->rom_default; tries[1] = chosen && *chosen ? s->o->rom_default : NULL; }
    msg[0] = 0;
    for (int i = 0; i < 2; i++) {
        if (!tries[i] || !*tries[i]) continue;
        const char *why = try_rom(s, tries[i], 1);
        if (!why) return 1;
        FILE *f = fopen(tries[i], "rb");
        if ((f || tries[i] != s->o->rom_default) && !msg[0]) snprintf(msg, size, "%s: %s", base_name(tries[i]), why);
        if (f) fclose(f);
    }
    return 0;
}

/* ---- the settings ------------------------------------------------------------------------------ */

static void settings_write(sdl *s)
{
    if (!mdrt_settings_dirty(s->set)) return;
    size_t n = mdrt_settings_format(s->set, NULL, 0);
    char *text = malloc(n);
    FILE *f = text ? fopen(s->set_path, "wb") : NULL;
    if (f) {
        mdrt_settings_format(s->set, text, n);
        if (fwrite(text, 1, n - 1, f) == n - 1) mdrt_settings_clean(s->set);
        fclose(f);
    }
    free(text);
    s->set_saved = SDL_GetTicks();
}

static SDL_Scancode bound(const char *name)
{
    SDL_Scancode k = name && *name ? SDL_GetScancodeFromName(name) : SDL_SCANCODE_UNKNOWN;
    if (name && *name && k == SDL_SCANCODE_UNKNOWN) fprintf(stderr, "settings: no key called \"%s\"\n", name);
    return k;
}

static int named(const char *v, const char *const *names, int n, int fallback)
{
    for (int i = 0; v && i < n; i++) if (!strcmp(v, names[i])) return i;
    return fallback;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* the FM core and chip a console is made with: the client's, else the settings' */
static void fm_from_settings(sdl *s)
{
    if (s->cfg->fm_core) return;
    const char *want = mdrt_settings_get(s->set, "audio.fm_core");
    for (int i = 0; mdrt_fm_cores(i); i++) if (want && !strcmp(want, mdrt_fm_cores(i))) s->cfg->fm_core = mdrt_fm_cores(i);
    s->cfg->fm_model = named(mdrt_settings_get(s->set, "audio.fm_chip"), chip_name, 2, MDRT_FM_YM2612);
}

/* the file read (or begun), every setting given its default, the keys looked up */
static void settings_load(sdl *s)
{
    const mdrt_sdl_options *o = s->o;
    s->set = mdrt_settings_new();
    s->set_path = o->settings_path ? o->settings_path : "settings.ini";   /* (in the directory the game runs in) */
    FILE *f = fopen(s->set_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *text = n > 0 ? malloc((size_t)n) : NULL;
        if (text && fread(text, 1, (size_t)n, f) == (size_t)n) mdrt_settings_parse(s->set, text, (size_t)n);
        free(text);
        fclose(f);
    } else {
        static const char head[] =
            "# pulseman's settings for this game, kept by the game (and its menu, on Esc): edit them while\n"
            "# it is closed. Keys are SDL's key names, empty for none: A, 1, F1, Up, Return, Space, Tab,\n"
            "# Backspace, Escape, Left Shift, Right Ctrl, Keypad 8, PageUp, or the key's own sign (, . - =).\n"
            "# On a Mac the function keys need Fn held, unless they are set to act as F1, F2 ...\n"
            "# video.scale: sharp, integer or fit. video.aspect: square or 4:3. audio.volume: 0-100.\n"
            "# audio.device: empty for the default. audio.latency: frames of sound queued (1-10).\n"
            "# audio.fm_core: nuked or ymfm. audio.fm_chip: YM2612 or YM3438.\n"
            "# general.fast_forward: times the speed while it is held (0: as fast as it goes).\n";
        mdrt_settings_parse(s->set, head, sizeof head - 1);
    }
    const char *defaults[][2] = {
        { "game.rom", "" },
        { "window.width", "960" }, { "window.height", "672" }, { "window.fullscreen", "0" },
        { "video.scale", "sharp" }, { "video.aspect", "square" },
        { "audio.volume", "100" }, { "audio.mute", "0" }, { "audio.device", "" }, { "audio.latency", "3" },
        { "audio.fm_core", mdrt_fm_core() }, { "audio.fm_chip", "YM2612" },
        { "general.pause_on_focus_loss", "1" }, { "general.fast_forward", "4" }, { "general.screenshots", "screenshots" },
    };
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++) mdrt_settings_default(s->set, defaults[i][0], defaults[i][1]);
    for (int p = 0; p < 2; p++)
        for (int b = 0; b < BUTTONS; b++) {
            char k[32];
            snprintf(k, sizeof k, "pad%d.%s", p + 1, button_name[b]);
            mdrt_settings_default(s->set, k, button_default[p][b]);
            s->button_key[p][b] = bound(mdrt_settings_get(s->set, k));
        }
    for (int i = 0; i < K_COUNT; i++) {
        mdrt_settings_default(s->set, key_setting[i][0], key_setting[i][1]);
        s->key[i] = bound(mdrt_settings_get(s->set, key_setting[i][0]));
    }
    s->scale = named(mdrt_settings_get(s->set, "video.scale"), scale_name, 3, MDRT_SCALE_SHARP);
    s->aspect = o->aspect ? MDRT_ASPECT_4_3 : named(mdrt_settings_get(s->set, "video.aspect"), aspect_name, 2, MDRT_ASPECT_SQUARE);
    SDL_AtomicSet(&s->volume, clamp(mdrt_settings_get_int(s->set, "audio.volume", 100), 0, 100));
    SDL_AtomicSet(&s->mute, mdrt_settings_get_int(s->set, "audio.mute", 0) != 0);
    s->queue_frames = clamp(mdrt_settings_get_int(s->set, "audio.latency", 3), 1, 10);
    SDL_AtomicSet(&s->fast_speed, clamp(mdrt_settings_get_int(s->set, "general.fast_forward", 4), 0, 100));
    s->focus_pause = mdrt_settings_get_int(s->set, "general.pause_on_focus_loss", 1) != 0;
    settings_write(s);                                    /* (a new file, or new settings: written now) */
}

/* ---- the window's title, screenshots, full screen ------------------------------------------- */

static void title_now(sdl *s)
{
    if (SDL_GetTicks() < s->notice_until) return;
    s->notice_until = 0;
    const char *t = s->o->title ? s->o->title : "pulseman";
    if (SDL_AtomicGet(&s->paused) && !s->menu_open) {
        snprintf(s->title, sizeof s->title, "%s - paused", t);
        t = s->title;
    }
    SDL_SetWindowTitle(s->win, t);
}

/* a change, for a moment in the title (and on the console) */
static void notice(sdl *s, const char *msg)
{
    snprintf(s->title, sizeof s->title, "%s - %s", s->o->title ? s->o->title : "pulseman", msg);
    SDL_SetWindowTitle(s->win, s->title);
    s->notice_until = SDL_GetTicks() + 2000;
    printf("%s\n", msg);
    fflush(stdout);
}

static void volume_notice(sdl *s)
{
    char msg[48];
    snprintf(msg, sizeof msg, "volume %d%%%s", SDL_AtomicGet(&s->volume), SDL_AtomicGet(&s->mute) ? " (muted)" : "");
    notice(s, msg);
}

/* the picture as it stands, to a PNG in the screenshots' directory */
static void screenshot(sdl *s)
{
    SDL_LockMutex(s->lock);
    int w = s->width, h = s->height;
    size_t n = w ? mdrt_png_encode(s->picture, w, h, 320, NULL, 0) : 0;
    uint8_t *png = n ? malloc(n) : NULL;
    if (png) mdrt_png_encode(s->picture, w, h, 320, png, n);
    SDL_UnlockMutex(s->lock);
    if (!png) return;
    const char *dir = mdrt_settings_get(s->set, "general.screenshots"), *name = s->o->app ? s->o->app : "screenshot";
    if (!dir || !*dir) dir = ".";
    make_dir(dir);
    char stamp[32], path[1024];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    FILE *f = NULL;
    for (int k = 1; k < 100 && !f; k++) {                 /* the first name not taken */
        if (k == 1) snprintf(path, sizeof path, "%s/%s-%s.png", dir, name, stamp);
        else snprintf(path, sizeof path, "%s/%s-%s-%d.png", dir, name, stamp, k);
        FILE *t = fopen(path, "rb");
        if (t) { fclose(t); continue; }
        f = fopen(path, "wb");
        if (!f) break;
    }
    char msg[1100];
    if (f && fwrite(png, 1, n, f) == n) snprintf(msg, sizeof msg, "screenshot: %s", path);
    else snprintf(msg, sizeof msg, "screenshot: cannot write in %s", dir);
    if (f) fclose(f);
    free(png);
    notice(s, msg);
}

static int is_fullscreen(sdl *s) { return (SDL_GetWindowFlags(s->win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0; }

static void set_fullscreen(sdl *s, int on)
{
    SDL_SetWindowFullscreen(s->win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    mdrt_settings_set_int(s->set, "window.fullscreen", on);
    s->redraw = 1;
}

/* ---- the menu ------------------------------------------------------------------------------------ */

static const char *key_name(SDL_Scancode k) { return k == SDL_SCANCODE_UNKNOWN ? "none" : SDL_GetScancodeName(k); }

static int is_rom_name(const char *name)
{
    static const char *const ext[] = { ".gen", ".md", ".bin", ".smd" };
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        size_t e = strlen(ext[i]);
        if (n > e && !SDL_strcasecmp(name + n - e, ext[i])) return 1;
    }
    return 0;
}

static int sep(char c) { return c == '/' || c == '\\'; }

/* s into to (size bytes), cut at its end or, tail, at its start (a path's end matters most) */
static void put_text(char *to, size_t size, const char *s, int tail)
{
    size_t n = strlen(s);
    if (tail && n >= size) { s += n - (size - 1); n = size - 1; }
    if (n >= size) n = size - 1;
    memcpy(to, s, n);
    to[n] = 0;
}

/* the directory above (none at a root: "/" or "C:\") */
static int dir_up(char *dir)
{
    size_t n = strlen(dir);
    while (n > 1 && sep(dir[n - 1]) && !(n == 3 && dir[1] == ':')) dir[--n] = 0;
    size_t root = sep(dir[0]) ? 1 : (n >= 3 && dir[1] == ':' && sep(dir[2])) ? 3 : 0;
    if (n <= root) return 0;
    while (n > root && !sep(dir[n - 1])) n--;
    if (n > root) n--;                                    /* (the separator, unless it is the root's) */
    dir[n > root ? n : root] = 0;
    if (!dir[0]) { dir[0] = '.'; dir[1] = 0; }
    return 1;
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    snprintf(out, size, "%s%s%s", dir, n && sep(dir[n - 1]) ? "" : (char[2]){ SEP, 0 }, name);
}

static int by_name(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int dx = sep(x[strlen(x) - 1]), dy = sep(y[strlen(y) - 1]);
    return dx != dy ? dy - dx : SDL_strcasecmp(x, y);
}

static void free_entries(sdl *s)
{
    for (int i = 0; i < s->nentries; i++) free(s->entries[i]);
    free(s->entries);
    s->entries = NULL;
    s->nentries = 0;
}

/* the directory's folders and ROM files, folders first (hidden ones left out) */
static void list_dir(sdl *s)
{
    free_entries(s);
    DIR *d = opendir(s->dir);
    struct dirent *e;
    int cap = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[2048];
        struct stat st;
        join(path, sizeof path, s->dir, e->d_name);
        if (stat(path, &st)) continue;
        int folder = S_ISDIR(st.st_mode);
        if (!folder && !is_rom_name(e->d_name)) continue;
        if (s->nentries == cap) {
            char **more = realloc(s->entries, (size_t)(cap = cap ? 2 * cap : 64) * sizeof *more);
            if (!more) break;
            s->entries = more;
        }
        size_t n = strlen(e->d_name) + 2;
        char *name = malloc(n);
        if (!name) break;
        snprintf(name, n, "%s%s", e->d_name, folder ? (char[2]){ SEP, 0 } : "");
        s->entries[s->nentries++] = name;
    }
    if (d) closedir(d);
    if (s->nentries) qsort(s->entries, (size_t)s->nentries, sizeof *s->entries, by_name);
}

/* a path the settings keep: inside the working directory, from there (the folder can move) */
static void kept_path(char *out, size_t size, const char *path)
{
    char cwd[1024];
    size_t n = get_cwd(cwd, sizeof cwd) ? strlen(cwd) : 0;
    if (n && !strncmp(path, cwd, n) && sep(path[n])) snprintf(out, size, "%s", path + n + 1);
    else snprintf(out, size, "%s", path);
}

static void build_page(sdl *s, int page, int select);

static const char *core_label(const char *core) { return !strcmp(core, "nuked") ? "Nuked-OPN2" : core; }

static void build_page(sdl *s, int page, int select)
{
    mdrt_menu *m = &s->menu;
    mdrt_menu_item *it;
    int running = s->thread != NULL;
    s->page = page;
    s->confirm = 0;
    s->binding = -1;
    switch (page) {
    case PAGE_MAIN:
        mdrt_menu_page(m, s->o->title ? s->o->title : "pulseman");
        if (running) mdrt_menu_add(m, I_RESUME, MDRT_ITEM_ACTION, "Resume");
        if (s->o->rom_check && (it = mdrt_menu_add(m, I_ROM, MDRT_ITEM_ACTION, "Game ROM")))
            put_text(it->detail, sizeof it->detail, s->rom ? base_name(s->rom_name) : "none", 1);
        if (running) {
            mdrt_menu_add(m, I_RESET, MDRT_ITEM_ACTION, "Reset");
            if ((it = mdrt_menu_add(m, I_SAVE, MDRT_ITEM_ACTION, "Save a state"))) snprintf(it->detail, sizeof it->detail, "%s", key_name(s->key[K_SAVE]));
            if ((it = mdrt_menu_add(m, I_LOAD, MDRT_ITEM_ACTION, "Load the state"))) snprintf(it->detail, sizeof it->detail, "%s", key_name(s->key[K_LOAD]));
        }
        mdrt_menu_add(m, I_VIDEO, MDRT_ITEM_ACTION, "Video");
        mdrt_menu_add(m, I_SOUND, MDRT_ITEM_ACTION, "Sound");
        mdrt_menu_add(m, I_OPTIONS, MDRT_ITEM_ACTION, "Options");
        mdrt_menu_add(m, I_KEYS, MDRT_ITEM_ACTION, "Keys");
        mdrt_menu_add(m, I_QUIT, MDRT_ITEM_ACTION, "Quit");
        if (!running && s->o->rom_check) snprintf(m->subtitle, sizeof m->subtitle, "Choose a ROM to play");
        break;
    case PAGE_VIDEO:
        mdrt_menu_page(m, "Video");
        mdrt_menu_add_choice(m, I_SCALE, "Scaling", scale_name, 3, s->scale);
        mdrt_menu_add_choice(m, I_ASPECT, "Aspect", aspect_label, 2, s->aspect);
        mdrt_menu_add_choice(m, I_FULL, "Full screen", off_on, 2, is_fullscreen(s));
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        snprintf(m->subtitle, sizeof m->subtitle, "Sharp: smooth edges. Integer: even pixels. Fit: fills.");
        break;
    case PAGE_SOUND: {
        mdrt_menu_page(m, "Sound");
        mdrt_menu_add_number(m, I_VOLUME, "Volume", SDL_AtomicGet(&s->volume), 0, 100, VOLUME_STEP, "%");
        mdrt_menu_add_choice(m, I_MUTE, "Mute", off_on, 2, SDL_AtomicGet(&s->mute));
        int core = 0;
        for (int i = 0; i < s->ncores; i++) if (s->cfg->fm_core && !strcmp(s->cfg->fm_core, mdrt_fm_cores(i))) core = i;
        mdrt_menu_add_choice(m, I_FM_CORE, "FM sound core", s->core_label, s->ncores, core);
        mdrt_menu_add_choice(m, I_FM_CHIP, "FM chip", chip_name, 2, s->cfg->fm_model);
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        snprintf(m->subtitle, sizeof m->subtitle, "YM2612: the first consoles. YM3438: later ones.");
        break;
    }
    case PAGE_OPTIONS: {
        mdrt_menu_page(m, "Options");
        mdrt_menu_add_choice(m, I_FOCUS, "Pause when the window loses focus", off_on, 2, s->focus_pause);
        int speed = 2, want = SDL_AtomicGet(&s->fast_speed);
        for (int i = 0; i < 6; i++) if (fast_speeds[i] == want) speed = i;
        mdrt_menu_add_choice(m, I_FAST, "Fast-forward speed", fast_label, 6, speed);
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        break;
    }
    case PAGE_KEYS:
        mdrt_menu_page(m, "Keys");
        snprintf(m->subtitle, sizeof m->subtitle, "Enter: change one");
        for (int p = 0; p < 2; p++)
            for (int b = 0; b < BUTTONS; b++) {
                char label[48];
                snprintf(label, sizeof label, "Pad %d %s", p + 1, button_label[b]);
                if ((it = mdrt_menu_add(m, I_PAD + p * BUTTONS + b, MDRT_ITEM_ACTION, label)))
                    snprintf(it->detail, sizeof it->detail, "%s", key_name(s->button_key[p][b]));
            }
        for (int k = 0; k < K_COUNT; k++)
            if ((it = mdrt_menu_add(m, I_CONTROL + k, MDRT_ITEM_ACTION, key_setting[k][2])))
                snprintf(it->detail, sizeof it->detail, "%s", key_name(s->key[k]));
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        break;
    case PAGE_ROM:
        mdrt_menu_page(m, "Choose the game's ROM");
        list_dir(s);
        put_text(m->subtitle, sizeof m->subtitle, s->dir, 1);
        {
            char up[1024];
            snprintf(up, sizeof up, "%s", s->dir);
            if (dir_up(up)) mdrt_menu_add(m, I_UP, MDRT_ITEM_ACTION, "..  (the folder above)");
        }
        for (int i = 0; i < s->nentries; i++) {
            mdrt_menu_add(m, I_ENTRY + i, MDRT_ITEM_ACTION, s->entries[i]);
            if (select == I_ENTRY && s->rom && !strcmp(s->entries[i], base_name(s->rom_name))) select = I_ENTRY + i;
        }
        for (int i = 0; i < s->nentries && select == I_ENTRY; i++)     /* (else the first ROM here) */
            if (!sep(s->entries[i][strlen(s->entries[i]) - 1])) select = I_ENTRY + i;
        if (!s->nentries) mdrt_menu_add(m, 0, MDRT_ITEM_TEXT, "(no folders or ROMs here)");
        break;
    }
    mdrt_menu_select(m, select);
    s->redraw = 1;
}

static void open_menu(sdl *s, int page, int select)
{
    s->menu_open = 1;
    SDL_AtomicSet(&s->in_menu, 1);
    memset(s->down, 0, sizeof s->down);                   /* (the game's keys let go) */
    build_page(s, page, select);
    SDL_LockMutex(s->lock);
    s->fresh = 1;
    SDL_UnlockMutex(s->lock);
    title_now(s);
}

static void close_menu(sdl *s)
{
    s->menu_open = 0;
    memset(s->down, 0, sizeof s->down);                   /* (the key that closed it does not reach the game) */
    for (int p = 0; p < 2; p++) {
        s->pad_ignore[p] = 0;
        for (int i = 0; i < 8 && s->pads[p]; i++)
            if (SDL_GameControllerGetButton(s->pads[p], pad_button[i])) s->pad_ignore[p] |= button_bit[i];
    }
    SDL_AtomicSet(&s->in_menu, 0);
    SDL_LockMutex(s->lock);
    s->fresh = 1;                                         /* (the picture without the menu) */
    SDL_UnlockMutex(s->lock);
    title_now(s);
}

static void quit(sdl *s) { SDL_AtomicSet(&s->quit, 1); }

/* the game again from the start (the ROM page's choice first, when there is one) */
static void restart(sdl *s, const char *rom_path)
{
    if (!stop_game(s)) { snprintf(s->menu.message, sizeof s->menu.message, "The game would not stop"); return; }
    if (rom_path) {
        const char *why = try_rom(s, rom_path, 1);
        if (why) {                                        /* (it changed since it was checked) */
            snprintf(s->menu.message, sizeof s->menu.message, "%s: %s", base_name(rom_path), why);
            if (!s->rom) return;
        } else {
            char kept[1024];
            kept_path(kept, sizeof kept, rom_path);
            mdrt_settings_set(s->set, "game.rom", kept);
            snprintf(s->rom_name, sizeof s->rom_name, "%s", kept);
        }
    }
    if (!start_game(s)) { build_page(s, PAGE_MAIN, I_ROM); snprintf(s->menu.message, sizeof s->menu.message, "The game could not start"); return; }
    close_menu(s);
}

/* a key for the item being bound (Esc keeps the old one, Delete leaves none) */
static void bind_key(sdl *s, SDL_Scancode k)
{
    int id = s->binding, sel = id;
    s->binding = -1;
    if (k == SDL_SCANCODE_ESCAPE) { build_page(s, PAGE_KEYS, sel); return; }
    if (k == SDL_SCANCODE_DELETE) k = SDL_SCANCODE_UNKNOWN;
    char setting[32];
    const char *other = NULL;
    if (id >= I_CONTROL) {
        if (id - I_CONTROL == K_MENU && k == SDL_SCANCODE_UNKNOWN) {
            build_page(s, PAGE_KEYS, sel);
            snprintf(s->menu.message, sizeof s->menu.message, "The menu needs a key");
            return;
        }
        s->key[id - I_CONTROL] = k;
        snprintf(setting, sizeof setting, "%s", key_setting[id - I_CONTROL][0]);
    } else {
        int p = (id - I_PAD) / BUTTONS, b = (id - I_PAD) % BUTTONS;
        s->button_key[p][b] = k;
        snprintf(setting, sizeof setting, "pad%d.%s", p + 1, button_name[b]);
    }
    mdrt_settings_set(s->set, setting, k == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(k));
    build_page(s, PAGE_KEYS, sel);
    for (int i = 0; i < s->menu.count && k != SDL_SCANCODE_UNKNOWN; i++)   /* the key's other use, if it has one */
        if (s->menu.items[i].id != id && s->menu.items[i].id >= I_PAD && !strcmp(s->menu.items[i].detail, SDL_GetScancodeName(k)))
            other = s->menu.items[i].label;
    if (other) snprintf(s->menu.message, sizeof s->menu.message, "%s is also %s", SDL_GetScancodeName(k), other);
}

/* a choice that needs choosing twice (it ends the game in play) */
static int confirmed(sdl *s, int id, const char *what)
{
    if (s->confirm == id) { s->confirm = 0; return 1; }
    s->confirm = id;
    snprintf(s->menu.message, sizeof s->menu.message, "Choose it again to %s", what);
    return 0;
}

static void act(sdl *s, int id)
{
    mdrt_menu_item *it = mdrt_menu_find(&s->menu, id);
    int running = s->thread != NULL;
    switch (id) {
    case I_RESUME: close_menu(s); return;
    case I_ROM:
        if (!s->dir[0]) {                                 /* the first time: the ROM's folder, or the working one */
            char cwd[1024];
            if (!get_cwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
            if (!s->rom) snprintf(s->dir, sizeof s->dir, "%s", cwd);
            else if (sep(s->rom_name[0]) || (s->rom_name[0] && s->rom_name[1] == ':')) snprintf(s->dir, sizeof s->dir, "%s", s->rom_name);
            else join(s->dir, sizeof s->dir, cwd, s->rom_name);
            if (s->rom) dir_up(s->dir);
        }
        build_page(s, PAGE_ROM, I_ENTRY);
        return;
    case I_RESET: if (confirmed(s, id, "start the game again")) restart(s, NULL); return;
    case I_SAVE: SDL_AtomicAdd(&s->saves, 1); close_menu(s); return;
    case I_LOAD: SDL_AtomicAdd(&s->loads, 1); close_menu(s); return;
    case I_VIDEO: build_page(s, PAGE_VIDEO, I_SCALE); return;
    case I_SOUND: build_page(s, PAGE_SOUND, I_VOLUME); return;
    case I_OPTIONS: build_page(s, PAGE_OPTIONS, I_FOCUS); return;
    case I_KEYS: build_page(s, PAGE_KEYS, I_PAD); return;
    case I_QUIT: if (!running || confirmed(s, id, "quit")) quit(s); return;
    case I_BACK: build_page(s, PAGE_MAIN, s->page == PAGE_VIDEO ? I_VIDEO : s->page == PAGE_SOUND ? I_SOUND : s->page == PAGE_OPTIONS ? I_OPTIONS : I_KEYS); return;
    case I_SCALE:
        s->scale = it->value;
        mdrt_settings_set(s->set, "video.scale", scale_name[s->scale]);
        return;
    case I_ASPECT:
        s->aspect = it->value;
        mdrt_settings_set(s->set, "video.aspect", aspect_name[s->aspect]);
        return;
    case I_FULL: set_fullscreen(s, it->value); return;
    case I_VOLUME:
        SDL_AtomicSet(&s->volume, it->value);
        mdrt_settings_set_int(s->set, "audio.volume", it->value);
        return;
    case I_MUTE:
        SDL_AtomicSet(&s->mute, it->value);
        mdrt_settings_set_int(s->set, "audio.mute", it->value);
        return;
    case I_FM_CORE: case I_FM_CHIP: {
        mdrt_menu_item *core = mdrt_menu_find(&s->menu, I_FM_CORE), *chip = mdrt_menu_find(&s->menu, I_FM_CHIP);
        s->cfg->fm_core = mdrt_fm_cores(core->value);
        s->cfg->fm_model = chip->value;
        mdrt_settings_set(s->set, "audio.fm_core", s->cfg->fm_core);
        mdrt_settings_set(s->set, "audio.fm_chip", chip_name[chip->value]);
        if (running) SDL_AtomicSet(&s->fm_want, 1 + core->value * 2 + chip->value);   /* (made between frames) */
        return;
    }
    case I_FOCUS:
        s->focus_pause = it->value;
        mdrt_settings_set_int(s->set, "general.pause_on_focus_loss", it->value);
        return;
    case I_FAST:
        SDL_AtomicSet(&s->fast_speed, fast_speeds[it->value]);
        mdrt_settings_set_int(s->set, "general.fast_forward", fast_speeds[it->value]);
        return;
    case I_UP:
        dir_up(s->dir);
        build_page(s, PAGE_ROM, I_UP);
        return;
    default: break;
    }
    if (id >= I_ENTRY && id - I_ENTRY < s->nentries) {
        char path[2048];
        const char *name = s->entries[id - I_ENTRY];
        size_t n = strlen(name);
        join(path, sizeof path, s->dir, name);
        if (sep(name[n - 1])) {                           /* a folder: into it */
            path[strlen(path) - 1] = 0;
            put_text(s->dir, sizeof s->dir, path, 0);
            build_page(s, PAGE_ROM, I_ENTRY);
            return;
        }
        const char *why = try_rom(s, path, 0);
        if (why) { snprintf(s->menu.message, sizeof s->menu.message, "%s", why); s->confirm = 0; return; }
        if (running && !confirmed(s, id, "start the game with it")) return;
        restart(s, path);
    } else if (id >= I_PAD && id < I_ENTRY) {             /* a key to change */
        s->binding = id;
        snprintf(s->menu.message, sizeof s->menu.message, "Press a key for %s (Esc: keep, Delete: none)", it->label);
    }
}

static void menu_input(sdl *s, int in)
{
    int id = mdrt_menu_input(&s->menu, in);
    if (s->confirm && id != s->confirm) { s->confirm = 0; s->menu.message[0] = 0; }   /* (not chosen again) */
    if (id == MDRT_MENU_CLOSE) {
        if (s->page == PAGE_ROM) build_page(s, PAGE_MAIN, I_ROM);
        else if (s->page != PAGE_MAIN) act(s, I_BACK);
        else if (s->thread) close_menu(s);
    } else if (id >= 0) act(s, id);
    s->redraw = 1;
}

/* ---- keys and controllers ------------------------------------------------------------------------ */

static void key_pressed(sdl *s, SDL_Scancode k, SDL_Keymod mod, int repeat)
{
    if (k == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) { if (!repeat) set_fullscreen(s, !is_fullscreen(s)); return; }
    if (s->menu_open) {
        if (s->binding >= 0) { if (!repeat) bind_key(s, k); s->redraw = 1; return; }
        int in = k == SDL_SCANCODE_UP ? MDRT_MENU_UP : k == SDL_SCANCODE_DOWN ? MDRT_MENU_DOWN :
                 k == SDL_SCANCODE_LEFT ? MDRT_MENU_LEFT : k == SDL_SCANCODE_RIGHT ? MDRT_MENU_RIGHT :
                 k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE ? MDRT_MENU_OK :
                 k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_BACKSPACE || k == s->key[K_MENU] ? MDRT_MENU_BACK : -1;
        if (in == MDRT_MENU_OK || in == MDRT_MENU_BACK) { if (repeat) return; }
        if (in >= 0) menu_input(s, in);
        else if (k == s->key[K_FULLSCREEN] && !repeat) set_fullscreen(s, !is_fullscreen(s));
        return;
    }
    if (repeat) return;
    int a = 0;
    while (a < K_COUNT && (s->key[a] != k || k == SDL_SCANCODE_UNKNOWN)) a++;
    switch (a) {
    case K_MENU: open_menu(s, PAGE_MAIN, I_RESUME); break;
    case K_PAUSE:
        SDL_AtomicSet(&s->paused, !SDL_AtomicGet(&s->paused));
        s->auto_paused = 0;
        s->notice_until = 0;
        title_now(s);
        break;
    case K_FULLSCREEN: set_fullscreen(s, !is_fullscreen(s)); break;
    case K_SAVE: SDL_AtomicAdd(&s->saves, 1); break;
    case K_LOAD: SDL_AtomicAdd(&s->loads, 1); break;
    case K_SLOW:
        SDL_AtomicSet(&s->slow, !SDL_AtomicGet(&s->slow));
        notice(s, SDL_AtomicGet(&s->slow) ? "slow motion" : "full speed");
        break;
    case K_STEP: if (SDL_AtomicGet(&s->paused)) SDL_AtomicSet(&s->step, 1); break;
    case K_SHOT: screenshot(s); break;
    case K_VOL_DOWN: case K_VOL_UP:
        SDL_AtomicSet(&s->volume, clamp(SDL_AtomicGet(&s->volume) + (a == K_VOL_UP ? VOLUME_STEP : -VOLUME_STEP), 0, 100));
        mdrt_settings_set_int(s->set, "audio.volume", SDL_AtomicGet(&s->volume));
        volume_notice(s);
        break;
    case K_MUTE:
        SDL_AtomicSet(&s->mute, !SDL_AtomicGet(&s->mute));
        mdrt_settings_set_int(s->set, "audio.mute", SDL_AtomicGet(&s->mute));
        volume_notice(s);
        break;
    default: break;
    }
}

/* a controller's button: the menu's moves while it is open; Back or Guide opens it */
static void pad_pressed(sdl *s, SDL_GameControllerButton b)
{
    if (!s->menu_open) {
        if (b == SDL_CONTROLLER_BUTTON_BACK || b == SDL_CONTROLLER_BUTTON_GUIDE) open_menu(s, PAGE_MAIN, I_RESUME);
        return;
    }
    if (s->binding >= 0) return;
    int in = b == SDL_CONTROLLER_BUTTON_DPAD_UP ? MDRT_MENU_UP : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? MDRT_MENU_DOWN :
             b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? MDRT_MENU_LEFT : b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? MDRT_MENU_RIGHT :
             b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START ? MDRT_MENU_OK :
             b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_BACK || b == SDL_CONTROLLER_BUTTON_GUIDE ? MDRT_MENU_BACK : -1;
    if (in >= 0) menu_input(s, in);
}

static void read_buttons(sdl *s)
{
    for (int p = 0; p < 2; p++) {
        uint16_t b = 0, pad = 0;
        for (int i = 0; i < BUTTONS; i++) {
            SDL_Scancode k = s->button_key[p][i];
            if (k != SDL_SCANCODE_UNKNOWN && s->down[k]) b |= button_bit[i];
            if (i < 8 && s->pads[p] && SDL_GameControllerGetButton(s->pads[p], pad_button[i])) pad |= button_bit[i];
        }
        if (s->pads[p]) {
            int x = SDL_GameControllerGetAxis(s->pads[p], SDL_CONTROLLER_AXIS_LEFTX);
            int y = SDL_GameControllerGetAxis(s->pads[p], SDL_CONTROLLER_AXIS_LEFTY);
            if (y < -16000) pad |= MDRT_PAD_UP;
            if (y > 16000) pad |= MDRT_PAD_DOWN;
            if (x < -16000) pad |= MDRT_PAD_LEFT;
            if (x > 16000) pad |= MDRT_PAD_RIGHT;
        }
        s->pad_ignore[p] &= pad;                          /* (let go: counted again) */
        b |= pad & ~s->pad_ignore[p];
        SDL_AtomicSet(&s->buttons[p], s->menu_open ? 0 : b);
    }
    SDL_AtomicSet(&s->rewind, s->key[K_REWIND] != SDL_SCANCODE_UNKNOWN && s->down[s->key[K_REWIND]]);
    SDL_AtomicSet(&s->fast, s->key[K_FAST] != SDL_SCANCODE_UNKNOWN && s->down[s->key[K_FAST]]);
}

/* ---- the picture ------------------------------------------------------------------------------- */

static void draw(sdl *s)
{
    SDL_LockMutex(s->lock);
    int fresh = s->fresh, w = s->width, h = s->height;
    if (s->menu_open && (fresh || s->redraw)) {          /* the menu over the picture as it stands */
        if (!w) { w = 320; h = 224; memset(s->menu_px, 0, sizeof s->menu_px); }
        else for (int y = 0; y < h; y++) memcpy(s->menu_px + y * 320, s->picture + y * 320, (size_t)w * 4);
    } else if (fresh) SDL_UpdateTexture(s->tex, NULL, s->picture, 320 * 4);
    s->fresh = 0;
    SDL_UnlockMutex(s->lock);
    if ((!fresh && !s->redraw) || (!w && !s->menu_open)) return;
    if (s->menu_open) {
        if (!w) { w = 320; h = 224; }
        mdrt_menu_draw(&s->menu, s->menu_px, w, h, 320);
        SDL_UpdateTexture(s->tex, NULL, s->menu_px, 320 * 4);
    }
    s->redraw = 0;
    int ww, wh;
    SDL_GetRendererOutputSize(s->ren, &ww, &wh);
    mdrt_view v = mdrt_view_place(ww, wh, w, h, s->scale, s->aspect);
    SDL_Rect src = { 0, 0, w, h }, dst = { v.x, v.y, v.w, v.h };
    SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
    SDL_RenderClear(s->ren);
    if (s->scale == MDRT_SCALE_SHARP && v.prescale > 1 && s->targets) {
        int pw = w * v.prescale, ph = h * v.prescale;     /* enlarged by a whole multiple first, then smoothly */
        if (!s->pre || s->pre_w != pw || s->pre_h != ph) {
            if (s->pre) SDL_DestroyTexture(s->pre);
            s->pre = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, pw, ph);
            if (s->pre) SDL_SetTextureScaleMode(s->pre, SDL_ScaleModeLinear);
            s->pre_w = pw;
            s->pre_h = ph;
        }
        if (s->pre) {
            SDL_SetTextureScaleMode(s->tex, SDL_ScaleModeNearest);
            SDL_SetRenderTarget(s->ren, s->pre);
            SDL_RenderCopy(s->ren, s->tex, &src, NULL);
            SDL_SetRenderTarget(s->ren, NULL);
            SDL_RenderCopy(s->ren, s->pre, NULL, &dst);
            SDL_RenderPresent(s->ren);
            return;
        }
    }
    SDL_SetTextureScaleMode(s->tex, s->scale == MDRT_SCALE_SHARP ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_RenderCopy(s->ren, s->tex, &src, &dst);
    SDL_RenderPresent(s->ren);
}

static void window_event(sdl *s, const SDL_WindowEvent *e)
{
    int full = is_fullscreen(s);
    switch (e->event) {
    case SDL_WINDOWEVENT_FOCUS_LOST:
        memset(s->down, 0, sizeof s->down);               /* (no key stays held) */
        if (s->focus_pause && !SDL_AtomicGet(&s->paused) && !s->menu_open) {
            SDL_AtomicSet(&s->paused, 1);
            s->auto_paused = 1;
            title_now(s);
        }
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        if (s->auto_paused) {
            SDL_AtomicSet(&s->paused, 0);
            s->auto_paused = 0;
            title_now(s);
        }
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        if (!full) {
            mdrt_settings_set_int(s->set, "window.width", e->data1);
            mdrt_settings_set_int(s->set, "window.height", e->data2);
        }
        s->redraw = 1;
        break;
    case SDL_WINDOWEVENT_MOVED:
        if (!full) {
            mdrt_settings_set_int(s->set, "window.x", e->data1);
            mdrt_settings_set_int(s->set, "window.y", e->data2);
        }
        break;
    case SDL_WINDOWEVENT_EXPOSED: s->redraw = 1; break;
    default: break;
    }
}

/* a window's corner kept from last time, if a display still shows it */
static int on_screen(int x, int y)
{
    for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
        SDL_Rect r;
        if (!SDL_GetDisplayBounds(i, &r) && x + 40 >= r.x && x + 40 < r.x + r.w && y + 20 >= r.y && y + 20 < r.y + r.h) return 1;
    }
    return 0;
}

static SDL_AudioDeviceID open_sound(sdl *s, SDL_AudioSpec *want)
{
    const char *device = mdrt_settings_get(s->set, "audio.device");
    SDL_AudioSpec got;
    if (device && *device) {
        SDL_AudioDeviceID d = SDL_OpenAudioDevice(device, 0, want, &got, 0);
        if (d) return d;
        fprintf(stderr, "no sound device \"%s\" (%s); the devices are:\n", device, SDL_GetError());
        for (int i = 0; i < SDL_GetNumAudioDevices(0); i++) fprintf(stderr, "  %s\n", SDL_GetAudioDeviceName(i, 0));
    }
    return SDL_OpenAudioDevice(NULL, 0, want, &got, 0);
}

int mdrt_sdl_run(mdrt_config *cfg, const mdrt_sdl_options *o, mdrt_game_fn game, void *user)
{
    sdl *s = calloc(1, sizeof *s);
    if (!s) return 1;
    s->o = o; s->cfg = cfg; s->game = game; s->user = user;
    s->binding = -1;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    settings_load(s);
    for (s->ncores = 0; s->ncores < 8 && mdrt_fm_cores(s->ncores); s->ncores++) s->core_label[s->ncores] = core_label(mdrt_fm_cores(s->ncores));
    fm_from_settings(s);
    if (!cfg->fm_core) cfg->fm_core = mdrt_fm_core();
    int w = mdrt_settings_get_int(s->set, "window.width", 960), h = mdrt_settings_get_int(s->set, "window.height", 672);
    if (o->scale > 0) { w = (s->aspect ? 299 : 320) * o->scale; h = 224 * o->scale; }
    if (w < 160 || h < 112) { w = 960; h = 672; }
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    if (mdrt_settings_get(s->set, "window.x") && mdrt_settings_get(s->set, "window.y")) {
        int kx = mdrt_settings_get_int(s->set, "window.x", 0), ky = mdrt_settings_get_int(s->set, "window.y", 0);
        if (on_screen(kx, ky)) { x = kx; y = ky; }
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (mdrt_settings_get_int(s->set, "window.fullscreen", 0)) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    s->win = SDL_CreateWindow(o->title ? o->title : "pulseman", x, y, w, h, flags);
    s->ren = s->win ? SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE) : NULL;
    if (s->win && !s->ren) s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED);
    if (s->win && !s->ren) s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_SOFTWARE);   /* no GPU */
    s->tex = s->ren ? SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240) : NULL;
    if (!s->tex) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    s->targets = SDL_RenderTargetSupported(s->ren);
    SDL_AudioSpec want = { 0 };
    want.freq = cfg->audio_rate ? (int)cfg->audio_rate : 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    s->dev = open_sound(s, &want);
    if (!s->dev) fprintf(stderr, "no sound (%s): the frames keep time by the clock\n", SDL_GetError());
    s->rate = (uint32_t)want.freq;
    cfg->audio_rate = s->rate;
    for (int i = 0, p = 0; i < SDL_NumJoysticks() && p < 2; i++)
        if (SDL_IsGameController(i)) s->pads[p++] = SDL_GameControllerOpen(i);
    s->lock = SDL_CreateMutex();
    cfg->platform = (mdrt_platform){ .user = s, .present = present, .audio = audio, .wait = wait, .input = input, .requests = requests };
    mdrt_menu_init(&s->menu);
    char why[1200] = "";
    if (o->rom_check && !first_rom(s, why, sizeof why)) {   /* no ROM yet: the menu's ROM page */
        act(s, I_ROM);
        s->menu_open = 1;
        SDL_AtomicSet(&s->in_menu, 1);
        put_text(s->menu.message, sizeof s->menu.message, why[0] ? why : o->rom_hint ? o->rom_hint : "", 0);
    } else if (!start_game(s)) { fprintf(stderr, "the game could not start (no FM core %s in this build?)\n", cfg->fm_core); SDL_Quit(); return 1; }
    while (!SDL_AtomicGet(&s->quit)) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) do {
            if (e.type == SDL_QUIT) quit(s);
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                SDL_Scancode k = e.key.keysym.scancode;
                if (k > SDL_SCANCODE_UNKNOWN && k < SDL_NUM_SCANCODES && !s->menu_open) s->down[k] = e.type == SDL_KEYDOWN;
                if (e.type == SDL_KEYDOWN) key_pressed(s, k, (SDL_Keymod)e.key.keysym.mod, e.key.repeat);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) pad_pressed(s, (SDL_GameControllerButton)e.cbutton.button);
            else if (e.type == SDL_WINDOWEVENT) window_event(s, &e.window);
            else if (e.type == SDL_RENDER_TARGETS_RESET) s->redraw = 1;
            else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                for (int p = 0; p < 2; p++) if (!s->pads[p]) { s->pads[p] = SDL_GameControllerOpen(e.cdevice.which); break; }
            }
        } while (SDL_PollEvent(&e));
        if (s->thread && SDL_AtomicGet(&s->done) && !SDL_AtomicGet(&s->stop)) break;   /* the game ended by itself */
        read_buttons(s);
        draw(s);
        if (s->notice_until) title_now(s);
        if (mdrt_settings_dirty(s->set) && SDL_GetTicks() - s->set_saved > 1000) settings_write(s);
    }
    settings_write(s);
    quit(s);
    int stopped = 1;
    if (s->thread) {                                      /* give the game a moment to stop */
        for (int i = 0; i < 200 && !SDL_AtomicGet(&s->done); i++) SDL_Delay(10);
        stopped = SDL_AtomicGet(&s->done);
        if (stopped) SDL_WaitThread(s->thread, NULL);
        else SDL_DetachThread(s->thread);                 /* (it would not stop: the process ends it) */
    }
    if (s->dev) SDL_CloseAudioDevice(s->dev);
    mdrt_settings_free(s->set);
    mdrt_menu_free(&s->menu);
    free_entries(s);
    if (stopped) { mdrt_free(s->m); SDL_DestroyMutex(s->lock); free(s->loud); free(s->rom); free(s); }   /* (else the thread still has them) */
    SDL_Quit();
    return 0;
}
