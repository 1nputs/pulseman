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
#include "mdrt/capture.h"
#include "mdrt/look.h"
#include "mdrt/menu.h"
#include "mdrt/platform_sdl2.h"
#include "mdrt/png.h"
#include "mdrt/settings.h"
#include "mdrt/upscale.h"
#include "mdrt/view.h"

/* ---- the keys: what each does, its setting and its default ------------------------------------ */

/* the controls on the function keys, in the keyboard's groups of four: the sound and rewind;
 * time (pause, fast-forward, slow motion, a frame); captures (F9 free: the replay, recording, a
 * screenshot). States and full screen are the menu's (and Alt+Enter). A control's key may have
 * Shift, Ctrl or Alt. */
enum { K_MUTE, K_VOL_DOWN, K_VOL_UP, K_REWIND, K_PAUSE, K_FAST, K_SLOW, K_STEP, K_REPLAY, K_RECORD, K_SHOT, K_MENU, K_COUNT };
static const char *const key_setting[K_COUNT][3] = {
    { "key.mute", "F1", "Mute" }, { "key.volume_down", "F2", "Volume down" }, { "key.volume_up", "F3", "Volume up" },
    { "key.rewind", "F4", "Rewind (held)" }, { "key.pause", "F5", "Pause" },
    { "key.fast_forward", "F6", "Fast-forward (held)" }, { "key.slow_motion", "F7", "Slow motion" },
    { "key.frame_step", "F8", "A frame, while paused" }, { "key.save_replay", "F10", "Save the replay" },
    { "key.record", "F11", "Record (again: stop)" }, { "key.screenshot", "F12", "Screenshot" },
    { "key.menu", "Escape", "Menu" },
};
enum { BUTTONS = 12 };
static const char *const button_name[BUTTONS] = { "up", "down", "left", "right", "a", "b", "c", "start", "x", "y", "z", "mode" };
static const char *const button_label[BUTTONS] = { "up", "down", "left", "right", "A", "B", "C", "Start", "X", "Y", "Z", "Mode" };
static const uint16_t button_bit[BUTTONS] = { MDRT_PAD_UP, MDRT_PAD_DOWN, MDRT_PAD_LEFT, MDRT_PAD_RIGHT, MDRT_PAD_A, MDRT_PAD_B,
                                              MDRT_PAD_C, MDRT_PAD_START, MDRT_PAD_X, MDRT_PAD_Y, MDRT_PAD_Z, MDRT_PAD_MODE };
static const char *const button_default[MDRT_PADS][BUTTONS] = {
    { "Up", "Down", "Left", "Right", "Z", "X", "C", "Return", "", "", "", "" },
    { "I", "K", "J", "L", "Q", "W", "E", "Tab", "", "", "", "" },
    { "", "", "", "", "", "", "", "", "", "", "", "" },
    { "", "", "", "", "", "", "", "", "", "", "", "" },
};
/* turbo: A, B or C pressed and let go over and over while the turbo key or button is held */
enum { TURBOS = 3 };
static const char *const turbo_name[TURBOS] = { "turbo_a", "turbo_b", "turbo_c" };
static const char *const turbo_label[TURBOS] = { "turbo A", "turbo B", "turbo C" };
static const uint16_t turbo_bit[TURBOS] = { MDRT_PAD_A, MDRT_PAD_B, MDRT_PAD_C };
/* a controller's buttons for the pad's (SDL's names: a b x y start back leftshoulder ...); the d-pad
 * and the left stick are the directions. The last three are turbo. */
enum { CBUTTONS = 11, SLOTS = 8 };
static const char *const cbutton_name[CBUTTONS] = { "a", "b", "c", "start", "x", "y", "z", "mode", "turbo_a", "turbo_b", "turbo_c" };
static const char *const cbutton_label[CBUTTONS] = { "A", "B", "C", "Start", "X", "Y", "Z", "Mode", "Turbo A", "Turbo B", "Turbo C" };
static const char *const cbutton_default[CBUTTONS] = { "x", "a", "b", "start", "leftshoulder", "y", "rightshoulder", "", "", "", "" };
static const uint16_t cbutton_bit[CBUTTONS] = { MDRT_PAD_A, MDRT_PAD_B, MDRT_PAD_C, MDRT_PAD_START, MDRT_PAD_X, MDRT_PAD_Y,
                                                MDRT_PAD_Z, MDRT_PAD_MODE, MDRT_PAD_A, MDRT_PAD_B, MDRT_PAD_C };
static const char *const adaptor_label[2] = { "two pads", "EA 4 Way Play" };
static const char *const adaptor_name[2] = { "none", "ea4way" };
static const int turbo_rates[6] = { 5, 8, 10, 12, 15, 20 };
static const char *const turbo_rate_label[6] = { "5 a second", "8 a second", "10 a second", "12 a second", "15 a second", "20 a second" };
static const char *const scale_name[3] = { "sharp", "integer", "fit" };
static const char *const aspect_name[2] = { "square", "4:3" };
static const char *const aspect_label[2] = { "square pixels", "4:3" };
static const char *const off_on[2] = { "off", "on" };
/* the colours' levels (mdrt_vdp_rgb's curves), and the looks (mdrt/look.h) */
static const char *const colours_name[3] = { "standard", "full", "console" };
static const char *const colours_label[3] = { "standard", "full range", "the console's" };
static const int colours_curve[3] = { MDRT_RGB_GPGX, MDRT_RGB_LINEAR, MDRT_RGB_CONSOLE };
static const char *const blend_name[MDRT_BLEND_COUNT] = { "off", "dither", "soft" };
static const char *const blend_label[MDRT_BLEND_COUNT] = { "off", "dithering", "everything" };
static const char *const scanlines_name[MDRT_SCANLINES_COUNT] = { "off", "light", "medium", "heavy" };
static const char *const mask_name[MDRT_MASK_COUNT] = { "off", "aperture", "slot", "dot" };
static const char *const mask_label[MDRT_MASK_COUNT] = { "off", "aperture grille", "slot mask", "shadow mask" };
static const char *const chip_name[2] = { "YM2612", "YM3438" };
static const struct { const char *name; Uint16 mod; } key_mods[3] = { { "Shift+", KMOD_SHIFT }, { "Ctrl+", KMOD_CTRL }, { "Alt+", KMOD_ALT } };
/* captures: the replay's seconds, a GIF's size */
static const int replay_secs[5] = { 0, 10, 20, 30, 60 };
static const char *const replay_label[5] = { "off", "10 seconds", "20 seconds", "30 seconds", "60 seconds" };
static const char *const gif_scale_label[3] = { "1x", "2x", "3x" };
static const int fast_speeds[6] = { 2, 3, 4, 6, 8, 0 };
static const char *const fast_label[6] = { "2x", "3x", "4x", "6x", "8x", "as fast as it goes" };

#define VOLUME_STEP 10
#define ROM_MAX (16u << 20)                               /* (no cartridge is larger) */

/* the menu's pages and items */
enum { PAGE_MAIN, PAGE_VIDEO, PAGE_SOUND, PAGE_OPTIONS, PAGE_KEYS, PAGE_ROM, PAGE_PLAYERS, PAGE_CBUTTONS, PAGE_SAVE, PAGE_LOAD, PAGE_CAPTURE };
enum { SLOT_COUNT = 8 };                                  /* save slots; 0 is quick resume's */
enum { I_RESUME = 1, I_ROM, I_RESET, I_SAVE, I_LOAD, I_VIDEO, I_SOUND, I_OPTIONS, I_KEYS, I_QUIT, I_BACK,
       I_UPSCALE, I_SCALE, I_ASPECT, I_FULL, I_COLOURS, I_BLEND, I_SCANLINES, I_MASK, I_VOLUME, I_MUTE, I_FM_CORE, I_FM_CHIP, I_FOCUS, I_FAST, I_UP,
       I_PLAYERS, I_ADAPTOR, I_TURBO, I_DEADZONE, I_CBUTTONS, I_RESUME_OPT,
       I_CAPTURE, I_CAP_REPLAY, I_CAP_RECORD, I_CAP_SECONDS, I_CAP_GIFSCALE,
       I_SLOT = 800,             /* + slot (1-8) */
       I_PLAYER = 900,           /* + player (0-3) */
       I_PAD = 1000,             /* + pad * BUTTONS + button */
       I_CONTROL = 2000,         /* + K_ */
       I_TURBOKEY = 2500,        /* + pad * TURBOS + turbo */
       I_CBUTTON = 3000,         /* + a controller button (cbutton_*) */
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
    uint16_t raw[320 * 240];    /* the picture's VDP colours (made RGB again when the colours change) */
    int width, height, fresh, redraw;
    SDL_atomic_t curve;         /* the colours' levels: mdrt_vdp_rgb's curve */
    SDL_atomic_t buttons[MDRT_PADS], quit, stop, paused, in_menu, done;
    SDL_atomic_t saves, loads, rewind;      /* presses of the state keys since the game last asked; held */
    SDL_atomic_t fast, slow, step, volume, mute;
    SDL_atomic_t fm_want;       /* 1 + the FM core's index x 2 + the model: for the game thread to make */
    SDL_GameController *ctrl[SLOTS];   /* the controllers, each in the slot it was given when plugged in */
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
    SDL_Scancode key[K_COUNT], button_key[MDRT_PADS][BUTTONS], turbo_key[MDRT_PADS][TURBOS];
    Uint16 key_mod[K_COUNT];    /* a control's Shift, Ctrl or Alt (0: none) */
    uint8_t down[SDL_NUM_SCANCODES];
    uint16_t pad_ignore[MDRT_PADS];   /* controller buttons held when the menu closed, until let go */
    int adaptor;                /* MDRT_ADAPTOR_* in the ports (the game looks at power on) */
    int player_ctrl[MDRT_PADS]; /* each player's controller slot + 1 (0: none) */
    SDL_GameControllerButton cbutton[CBUTTONS];
    int turbo_rate, dead_zone;  /* turbo presses a second; the stick's dead zone, in % */
    char ctrl_label[SLOTS + 1][64];
    const char *ctrl_choice[SLOTS + 1];
    /* save slots and quick resume (states/slotN.state with slotN.png; states/resume.state) */
    char req_path[1100], game_path[1100];   /* the file the next SAVE / LOAD names (s->lock); the game thread's copy */
    int pending;                /* a save asked for: its slot (0 quick resume), -1 none */
    uint32_t pending_until;
    long pending_size;          /* its file's size when last seen growing (-1: not there yet) */
    uint32_t *pending_pic;      /* the picture when it was asked for */
    int pending_w, pending_h;
    int slot, resume, quitting; /* the slot chosen last; quick resume; quitting once it is saved */
    uint32_t thumb[320 * 240];  /* the menu's picture of the slot selected */
    int scale, aspect, focus_pause, auto_paused;
    int upscale;                /* MDRT_UPSCALE_: the game's frames through it, before the scaling */
    int colours, blend, scanlines, mask;   /* colours_*; MDRT_BLEND_, _SCANLINES_, _MASK_ */
    uint32_t *frame, *blended, *big, *big_scratch;   /* the frame taken from the game thread; it blended; upscaled; mmpx4x's halfway */
    SDL_Texture *tube;          /* the scanlines and mask over the picture, for: */
    int tube_w, tube_h, tube_cols, tube_lines, tube_scan, tube_mask;
    SDL_Texture *big_tex;       /* the upscaled frame, big_f times the picture's texture */
    int big_f;
    const char *upscale_label[MDRT_UPSCALE_COUNT];
    /* captures (mdrt/capture.h): the replay kept and a recording, the game thread's frames into
     * them under cap_lock; clips saved on threads of their own, one at a time */
    SDL_mutex *cap_lock, *save_lock;
    mdrt_capture *cap;
    SDL_atomic_t capturing;     /* the replay on, or a recording */
    int replay, gif_scale;      /* replay_secs' index; a GIF's size - 1 */
    FILE *rec;                  /* the recording's file, */
    char rec_path[1100];
    uint32_t rec_since, status_at;
    struct clip_save *clips[4]; /* clips being saved */
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
    int curve = SDL_AtomicGet(&s->curve);
    SDL_LockMutex(s->lock);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t c = px[y * pitch + x];
            s->raw[y * 320 + x] = c;
            s->picture[y * 320 + x] = 0xFF000000u | mdrt_vdp_rgb(c, curve);
        }
    s->width = w;
    s->height = h;
    s->fresh = 1;
    SDL_UnlockMutex(s->lock);
    if (SDL_AtomicGet(&s->capturing)) {                   /* (the frame for the replay and a recording) */
        SDL_LockMutex(s->cap_lock);
        mdrt_capture_picture(s->cap, px, w, h, pitch);
        SDL_UnlockMutex(s->cap_lock);
    }
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

static void input(void *user, uint16_t buttons[MDRT_PADS])
{
    sdl *s = user;
    for (int p = 0; p < MDRT_PADS; p++) buttons[p] = (uint16_t)SDL_AtomicGet(&s->buttons[p]);
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

/* the file the requests name: the platform's slot (the game thread's copy, until the next handshake) */
static const char *request_path(void *user)
{
    sdl *s = user;
    SDL_LockMutex(s->lock);
    snprintf(s->game_path, sizeof s->game_path, "%s", s->req_path);
    SDL_UnlockMutex(s->lock);
    return s->game_path[0] ? s->game_path : NULL;
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
    mdrt_pads_set_adaptor(mdrt_get_pads(s->m), s->adaptor);   /* (in the ports at power on) */
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

/* a control's key: a name, perhaps after Shift+, Ctrl+ or Alt+ */
static SDL_Scancode bound_mod(const char *name, Uint16 *mod)
{
    *mod = 0;
    for (int i = 0; name && i < 3; i++)
        if (!strncmp(name, key_mods[i].name, strlen(key_mods[i].name))) { *mod = key_mods[i].mod; name += strlen(key_mods[i].name); break; }
    return bound(name);
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
            "# video.upscaler: off, scale2x, scale3x, mmpx2x, mmpx4x, xbr2x, xbr3x or xbr4x (pixel-art\n"
            "# magnification, before the scaling). video.scale: sharp, integer or fit. video.aspect: square or 4:3. video.colours:\n"
            "# standard, full or console (the levels a console puts out). video.blend: off, dither or soft.\n"
            "# video.scanlines: off, light, medium or heavy. video.mask: off, aperture, slot or dot.\n"
            "# audio.volume: 0-100.\n"
            "# audio.device: empty for the default. audio.latency: frames of sound queued (1-10).\n"
            "# audio.fm_core: nuked or ymfm. audio.fm_chip: YM2612 or YM3438.\n"
            "# general.fast_forward: times the speed while it is held (0: as fast as it goes).\n"
            "# capture.replay: the seconds kept for the replay (0: off). capture.gif_scale: 1-3. A\n"
            "# control's key may have Shift+, Ctrl+ or Alt+ before it.\n";
        mdrt_settings_parse(s->set, head, sizeof head - 1);
    }
    const char *defaults[][2] = {
        { "game.rom", "" },
        { "window.width", "960" }, { "window.height", "672" }, { "window.fullscreen", "0" },
        { "video.upscaler", "off" }, { "video.scale", "sharp" }, { "video.aspect", "square" }, { "video.colours", "standard" },
        { "video.blend", "off" }, { "video.scanlines", "off" }, { "video.mask", "off" },
        { "audio.volume", "100" }, { "audio.mute", "0" }, { "audio.device", "" }, { "audio.latency", "3" },
        { "audio.fm_core", mdrt_fm_core() }, { "audio.fm_chip", "YM2612" },
        { "general.pause_on_focus_loss", "1" }, { "general.fast_forward", "4" }, { "general.screenshots", "screenshots" },
    };
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++) mdrt_settings_default(s->set, defaults[i][0], defaults[i][1]);
    for (int p = 0; p < MDRT_PADS; p++) {
        char k[48];
        for (int b = 0; b < BUTTONS; b++) {
            snprintf(k, sizeof k, "pad%d.%s", p + 1, button_name[b]);
            mdrt_settings_default(s->set, k, button_default[p][b]);
            s->button_key[p][b] = bound(mdrt_settings_get(s->set, k));
        }
        for (int t = 0; t < TURBOS; t++) {
            snprintf(k, sizeof k, "pad%d.%s", p + 1, turbo_name[t]);
            mdrt_settings_default(s->set, k, "");
            s->turbo_key[p][t] = bound(mdrt_settings_get(s->set, k));
        }
        snprintf(k, sizeof k, "player%d.controller", p + 1);
        static const char *const slot[MDRT_PADS] = { "1", "2", "3", "4" };
        mdrt_settings_default(s->set, k, slot[p]);
        s->player_ctrl[p] = clamp(mdrt_settings_get_int(s->set, k, p + 1), 0, SLOTS);
    }
    for (int i = 0; i < CBUTTONS; i++) {
        char k[48];
        snprintf(k, sizeof k, "controller.%s", cbutton_name[i]);
        mdrt_settings_default(s->set, k, cbutton_default[i]);
        const char *v = mdrt_settings_get(s->set, k);
        s->cbutton[i] = v && *v ? SDL_GameControllerGetButtonFromString(v) : SDL_CONTROLLER_BUTTON_INVALID;
        if (v && *v && s->cbutton[i] == SDL_CONTROLLER_BUTTON_INVALID) fprintf(stderr, "settings: no controller button called \"%s\"\n", v);
    }
    mdrt_settings_default(s->set, "input.adaptor", "none");
    mdrt_settings_default(s->set, "controller.dead_zone", "50");
    mdrt_settings_default(s->set, "general.turbo_rate", "10");
    s->adaptor = named(mdrt_settings_get(s->set, "input.adaptor"), adaptor_name, 2, MDRT_ADAPTOR_NONE);
    s->dead_zone = clamp(mdrt_settings_get_int(s->set, "controller.dead_zone", 50), 5, 95);
    s->turbo_rate = clamp(mdrt_settings_get_int(s->set, "general.turbo_rate", 10), 1, 30);
    for (int i = 0; i < K_COUNT; i++) {
        mdrt_settings_default(s->set, key_setting[i][0], key_setting[i][1]);
        s->key[i] = bound_mod(mdrt_settings_get(s->set, key_setting[i][0]), &s->key_mod[i]);
    }
    s->scale = named(mdrt_settings_get(s->set, "video.scale"), scale_name, 3, MDRT_SCALE_SHARP);
    const char *up = mdrt_settings_get(s->set, "video.upscaler");
    for (int i = 0; i < MDRT_UPSCALE_COUNT; i++) {
        s->upscale_label[i] = mdrt_upscale_label(i);
        if (up && !strcmp(up, mdrt_upscale_name(i))) s->upscale = i;
    }
    s->colours = named(mdrt_settings_get(s->set, "video.colours"), colours_name, 3, 0);
    SDL_AtomicSet(&s->curve, colours_curve[s->colours]);
    s->blend = named(mdrt_settings_get(s->set, "video.blend"), blend_name, MDRT_BLEND_COUNT, MDRT_BLEND_OFF);
    s->scanlines = named(mdrt_settings_get(s->set, "video.scanlines"), scanlines_name, MDRT_SCANLINES_COUNT, MDRT_SCANLINES_OFF);
    s->mask = named(mdrt_settings_get(s->set, "video.mask"), mask_name, MDRT_MASK_COUNT, MDRT_MASK_OFF);
    s->aspect = o->aspect ? MDRT_ASPECT_4_3 : named(mdrt_settings_get(s->set, "video.aspect"), aspect_name, 2, MDRT_ASPECT_SQUARE);
    SDL_AtomicSet(&s->volume, clamp(mdrt_settings_get_int(s->set, "audio.volume", 100), 0, 100));
    SDL_AtomicSet(&s->mute, mdrt_settings_get_int(s->set, "audio.mute", 0) != 0);
    s->queue_frames = clamp(mdrt_settings_get_int(s->set, "audio.latency", 3), 1, 10);
    SDL_AtomicSet(&s->fast_speed, clamp(mdrt_settings_get_int(s->set, "general.fast_forward", 4), 0, 100));
    s->focus_pause = mdrt_settings_get_int(s->set, "general.pause_on_focus_loss", 1) != 0;
    mdrt_settings_default(s->set, "general.states", "states");
    mdrt_settings_default(s->set, "general.slot", "1");
    mdrt_settings_default(s->set, "general.resume", "1");
    s->slot = clamp(mdrt_settings_get_int(s->set, "general.slot", 1), 1, SLOT_COUNT);
    s->resume = mdrt_settings_get_int(s->set, "general.resume", 1) != 0;
    s->pending = -1;
    mdrt_settings_default(s->set, "capture.replay", "20");
    mdrt_settings_default(s->set, "capture.gif_scale", "1");
    mdrt_settings_default(s->set, "general.videos", "videos");
    int secs = mdrt_settings_get_int(s->set, "capture.replay", 20);
    for (int i = 0; i < 5; i++) if (replay_secs[i] <= secs) s->replay = i;   /* (the nearest below) */
    static const char *const gone[] = { "key.save_state", "key.load_state", "key.fullscreen", "capture.format", "capture.ffmpeg",
                                        "capture.video_scale" };
    for (size_t i = 0; i < sizeof gone / sizeof *gone; i++) mdrt_settings_remove(s->set, gone[i]);   /* (an older build's) */
    s->gif_scale = clamp(mdrt_settings_get_int(s->set, "capture.gif_scale", 1), 1, 3) - 1;
    settings_write(s);                                    /* (a new file, or new settings: written now) */
}

/* ---- the window's title, screenshots, full screen ------------------------------------------- */

static int saving_percent(sdl *s);

/* the title: the game's, paused, recording (for how long), saving a capture (how far) */
static void title_now(sdl *s)
{
    if (SDL_GetTicks() < s->notice_until) return;
    s->notice_until = 0;
    size_t n = (size_t)snprintf(s->title, sizeof s->title, "%s", s->o->title ? s->o->title : "pulseman");
    if (SDL_AtomicGet(&s->paused) && !s->menu_open && n < sizeof s->title) n += (size_t)snprintf(s->title + n, sizeof s->title - n, " - paused");
    if (s->rec && n < sizeof s->title) {
        uint32_t t = (SDL_GetTicks() - s->rec_since) / 1000;
        n += (size_t)snprintf(s->title + n, sizeof s->title - n, " - recording %u:%02u", t / 60, t % 60);
    }
    int pc = saving_percent(s);
    if (pc >= 0 && n < sizeof s->title) snprintf(s->title + n, sizeof s->title - n, " - saving the GIF, %d%%", pc);
    SDL_SetWindowTitle(s->win, s->title);
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

/* a new file in a directory (made if need be), named for the game and the time: the first name not
 * taken; NULL when none can be made */
static FILE *new_file(sdl *s, const char *dir, const char *prefix, const char *ext, char *path, size_t size)
{
    const char *name = s->o->app ? s->o->app : "pulseman";
    make_dir(dir);
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(path, size, "%s/%s%s-%s%s", dir, prefix, name, stamp, ext);
        else snprintf(path, size, "%s/%s%s-%s-%d%s", dir, prefix, name, stamp, k, ext);
        FILE *t = fopen(path, "rb");
        if (t) { fclose(t); continue; }
        return fopen(path, "wb");
    }
    return NULL;
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
    const char *dir = mdrt_settings_get(s->set, "general.screenshots");
    if (!dir || !*dir) dir = ".";
    char path[1024];
    FILE *f = new_file(s, dir, "", ".png", path, sizeof path);
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

/* ---- captures: the replay, recordings, and saving them ------------------------------------- */

static double frame_rate(sdl *s)
{
    return s->cfg->pal ? (double)MDRT_MASTER_CLOCK_PAL / (MDRT_LINE_MCLKS * MDRT_LINES_PAL)
                       : (double)MDRT_MASTER_CLOCK_NTSC / (MDRT_LINE_MCLKS * MDRT_LINES_NTSC);
}

/* the frames kept for the replay, and whether the game thread hands them over */
static void capture_set(sdl *s)
{
    SDL_LockMutex(s->cap_lock);
    mdrt_capture_keep(s->cap, (int)(replay_secs[s->replay] * frame_rate(s) + 0.5));
    SDL_UnlockMutex(s->cap_lock);
    SDL_AtomicSet(&s->capturing, s->replay > 0 || s->rec != NULL);
}

/* a clip being saved as a GIF, on a thread of its own (one saves at a time, under save_lock) */
typedef struct clip_save {
    mdrt_clip *clip;
    FILE *src;                  /* a recording's file (closed and removed after), or NULL */
    char src_path[1100], path[1100], msg[1200];
    int curve, gif_scale;
    double fps;
    volatile long frames, done; /* (how far, for the title) */
    SDL_mutex *lock;
    SDL_Thread *thread;
    SDL_atomic_t finished;
} clip_save;

static void put_file(void *user, const void *b, size_t n) { (void)fwrite(b, 1, n, user); }   /* (ferror at the end) */

static int save_thread(void *user)
{
    clip_save *v = user;
    SDL_LockMutex(v->lock);
    long frames = mdrt_clip_frames(v->clip, NULL, NULL);
    v->frames = frames > 0 ? frames : 1;
    FILE *f = fopen(v->path, "wb");
    int ok = 0;
    if (f) {
        ok = mdrt_clip_gif(v->clip, v->curve, v->gif_scale, v->fps, put_file, f, &v->done);
        ok = !ferror(f) && ok;
        fclose(f);
    }
    if (!ok) remove(v->path);
    if (ok) snprintf(v->msg, sizeof v->msg, "saved: %s", v->path);
    else snprintf(v->msg, sizeof v->msg, "could not save the GIF");
    mdrt_clip_free(v->clip);
    if (v->src) { fclose(v->src); remove(v->src_path); }
    SDL_UnlockMutex(v->lock);
    SDL_AtomicSet(&v->finished, 1);
    return 0;
}

/* a clip saved (from the replay, or a recording's file at src_path); 0 when it cannot be started */
static int start_saving(sdl *s, mdrt_clip *clip, FILE *src, const char *src_path)
{
    int slot = 0;
    while (slot < 4 && s->clips[slot]) slot++;
    clip_save *v = slot < 4 ? calloc(1, sizeof *v) : NULL;
    const char *dir = mdrt_settings_get(s->set, "general.videos");
    if (!dir || !*dir) dir = ".";
    FILE *f = v ? new_file(s, dir, "", ".gif", v->path, sizeof v->path) : NULL;
    if (!f) {
        notice(s, slot < 4 ? "cannot write a GIF there" : "still saving the last ones");
        free(v);
        mdrt_clip_free(clip);
        if (src) { fclose(src); remove(src_path); }
        return 0;
    }
    fclose(f);                                            /* (the name taken; written by the thread) */
    v->clip = clip;
    v->src = src;
    if (src_path) snprintf(v->src_path, sizeof v->src_path, "%s", src_path);
    v->curve = SDL_AtomicGet(&s->curve);
    v->gif_scale = s->gif_scale + 1;
    v->fps = frame_rate(s);
    v->lock = s->save_lock;
    v->thread = SDL_CreateThread(save_thread, "capture", v);
    if (!v->thread) { remove(v->path); free(v); mdrt_clip_free(clip); if (src) { fclose(src); remove(src_path); } return 0; }
    s->clips[slot] = v;
    return 1;
}

/* how far the clip saving now is (-1: none saving) */
static int saving_percent(sdl *s)
{
    int pc = -1;
    for (int i = 0; i < 4; i++) {
        clip_save *v = s->clips[i];
        if (v && !SDL_AtomicGet(&v->finished)) {
            long n = v->frames, d = v->done;
            int p = n > 0 ? (int)(d * 100 / n) : 0;
            if (p > pc) pc = p;
        }
    }
    return pc;
}

/* the saves finished: their answers in the title; with wait, until none is left */
static void check_saves(sdl *s, int wait)
{
    for (int i = 0; i < 4; i++) {
        clip_save *v = s->clips[i];
        if (!v || (!SDL_AtomicGet(&v->finished) && !wait)) continue;
        SDL_WaitThread(v->thread, NULL);
        notice(s, v->msg);
        free(v);
        s->clips[i] = NULL;
    }
}

/* the replay: the last seconds kept, saved (the replay keeps on from now); what became of it */
static const char *save_replay(sdl *s)
{
    const char *msg = "saving the replay";
    SDL_LockMutex(s->cap_lock);
    mdrt_clip *clip = s->replay ? mdrt_capture_take(s->cap) : NULL;
    SDL_UnlockMutex(s->cap_lock);
    if (!s->replay) msg = "the replay is off (in the menu: Capture)";
    else if (!clip) msg = "no replay yet";
    else if (!start_saving(s, clip, NULL, NULL)) return "";   /* (it says why) */
    notice(s, msg);
    return msg;
}

/* a recording started (to a file in the videos' directory), or stopped and saved */
static void record(sdl *s)
{
    if (!s->rec) {
        const char *dir = mdrt_settings_get(s->set, "general.videos");
        FILE *f = new_file(s, dir && *dir ? dir : ".", ".recording-", ".clip", s->rec_path, sizeof s->rec_path);
        if (f) fclose(f);
        f = f ? fopen(s->rec_path, "w+b") : NULL;          /* (written, then read back) */
        if (!f) { notice(s, "cannot record there"); return; }
        s->rec = f;
        s->rec_since = SDL_GetTicks();
        SDL_LockMutex(s->cap_lock);
        mdrt_capture_record(s->cap, f);
        SDL_UnlockMutex(s->cap_lock);
        capture_set(s);
        notice(s, "recording");
        return;
    }
    SDL_LockMutex(s->cap_lock);
    mdrt_capture_record(s->cap, NULL);
    long n = mdrt_capture_recorded(s->cap);
    SDL_UnlockMutex(s->cap_lock);
    FILE *f = s->rec;
    s->rec = NULL;
    capture_set(s);
    mdrt_clip *clip = n > 0 && fflush(f) == 0 ? mdrt_clip_file(f) : NULL;
    if (!clip) {
        fclose(f);
        remove(s->rec_path);
        notice(s, n < 0 ? "the recording could not be written (the disk full?)" : "nothing was recorded");
        return;
    }
    if (start_saving(s, clip, f, s->rec_path)) notice(s, "saving the recording");
}

/* a second on: the title's clock, and a recording that could not be written stopped */
static void capture_status(sdl *s)
{
    if (!s->rec && saving_percent(s) < 0) return;
    if (SDL_GetTicks() - s->status_at < 500) return;
    s->status_at = SDL_GetTicks();
    if (s->rec) {
        SDL_LockMutex(s->cap_lock);
        long n = mdrt_capture_recorded(s->cap);
        SDL_UnlockMutex(s->cap_lock);
        if (n < 0) record(s);
    }
    title_now(s);
}

/* ---- the menu ------------------------------------------------------------------------------------ */

static const char *key_name(SDL_Scancode k) { return k == SDL_SCANCODE_UNKNOWN ? "none" : SDL_GetScancodeName(k); }

/* a control's key as the settings and the menu name it: "F12", "Shift+F12" ("" for none) */
static const char *control_name(sdl *s, int c, char *buf, size_t n)
{
    const char *m = "";
    for (int i = 0; i < 3; i++) if (s->key_mod[c] == key_mods[i].mod) m = key_mods[i].name;
    snprintf(buf, n, "%s%s", s->key[c] == SDL_SCANCODE_UNKNOWN ? "" : m, s->key[c] == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(s->key[c]));
    return buf;
}

static void control_detail(sdl *s, int c, char *buf, size_t n)
{
    control_name(s, c, buf, n);
    if (!buf[0]) snprintf(buf, n, "none");
}

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

/* the Players page's choices: none, then each controller slot with what is plugged into it */
static void ctrl_choices(sdl *s)
{
    snprintf(s->ctrl_label[0], sizeof s->ctrl_label[0], "none");
    for (int i = 0; i < SLOTS; i++) {
        const char *name = s->ctrl[i] ? SDL_GameControllerName(s->ctrl[i]) : NULL;
        snprintf(s->ctrl_label[i + 1], sizeof s->ctrl_label[i + 1], "controller %d: %.28s", i + 1, s->ctrl[i] ? name ? name : "?" : "none yet");
    }
    for (int i = 0; i <= SLOTS; i++) s->ctrl_choice[i] = s->ctrl_label[i];
}

/* the controller's buttons held, as the pad's (turbo_on: the turbo buttons' moment to press) */
static uint16_t ctrl_buttons(sdl *s, SDL_GameController *c, int turbo_on)
{
    static const SDL_GameControllerButton dpad[4] = { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                                      SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT };
    uint16_t b = 0;
    for (int i = 0; i < 4; i++) if (SDL_GameControllerGetButton(c, dpad[i])) b |= button_bit[i];
    for (int i = 0; i < CBUTTONS; i++)
        if (s->cbutton[i] != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(c, s->cbutton[i]) && (i < 8 || turbo_on))
            b |= cbutton_bit[i];
    int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    int dead = 32767 * s->dead_zone / 100;
    if (y < -dead) b |= MDRT_PAD_UP;
    if (y > dead) b |= MDRT_PAD_DOWN;
    if (x < -dead) b |= MDRT_PAD_LEFT;
    if (x > dead) b |= MDRT_PAD_RIGHT;
    return b;
}

static SDL_GameController *player_ctrl(sdl *s, int p) { return s->player_ctrl[p] ? s->ctrl[s->player_ctrl[p] - 1] : NULL; }

/* a controller plugged in (device index): into the first free slot, unless it is open already */
static void ctrl_added(sdl *s, int index)
{
    if (!SDL_IsGameController(index)) return;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    for (int i = 0; i < SLOTS; i++)
        if (s->ctrl[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s->ctrl[i])) == id) return;
    for (int i = 0; i < SLOTS; i++)
        if (!s->ctrl[i]) { s->ctrl[i] = SDL_GameControllerOpen(index); break; }
}

/* a controller pulled out (instance id): its slot empty, the others where they were */
static void ctrl_removed(sdl *s, SDL_JoystickID id)
{
    for (int i = 0; i < SLOTS; i++)
        if (s->ctrl[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s->ctrl[i])) == id) {
            SDL_GameControllerClose(s->ctrl[i]);
            s->ctrl[i] = NULL;
        }
}

/* ---- save slots ------------------------------------------------------------------------------ */

/* a slot's file (0: quick resume's), with ext after its name (".state", ".png", ".state.new") */
static void slot_path(sdl *s, char *out, size_t n, int slot, const char *ext)
{
    const char *dir = mdrt_settings_get(s->set, "general.states");
    if (!dir || !*dir) dir = ".";
    if (slot) snprintf(out, n, "%s/slot%d%s", dir, slot, ext);
    else snprintf(out, n, "%s/resume%s", dir, ext);
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) ? -1 : (long)st.st_size;
}

/* "Slot 3  16 Oct 14:03", or "Slot 3  empty"; 1 when it holds a state */
static int slot_label(sdl *s, int slot, char *out, size_t n)
{
    char path[1100];
    struct stat st;
    slot_path(s, path, sizeof path, slot, ".state");
    if (stat(path, &st)) { snprintf(out, n, "Slot %d  empty", slot); return 0; }
    char when[32];
    time_t t = st.st_mtime;
    strftime(when, sizeof when, "%d %b %H:%M", localtime(&t));
    snprintf(out, n, "Slot %d  %s", slot, when);
    return 1;
}

/* the selected slot's picture beside the list (none when it has none) */
static void slot_picture(sdl *s)
{
    mdrt_menu_item *it = s->menu.count ? &s->menu.items[s->menu.sel] : NULL;
    s->menu.image = NULL;
    if (!it || it->id < I_SLOT || it->id > I_SLOT + SLOT_COUNT) return;
    char path[1100];
    size_t n = 0;
    const char *why = NULL;
    slot_path(s, path, sizeof path, it->id - I_SLOT, ".png");
    if (file_size(path) <= 0) return;
    uint8_t *png = read_file(path, &n, &why);
    int w, h;
    if (png && !mdrt_png_decode(png, n, s->thumb, 320, 240, &w, &h)) {
        s->menu.image = s->thumb;
        s->menu.image_w = w;
        s->menu.image_h = h;
        s->menu.image_pitch = 320;
    }
    free(png);
}

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
            mdrt_menu_add(m, I_SAVE, MDRT_ITEM_ACTION, "Save a state");
            mdrt_menu_add(m, I_LOAD, MDRT_ITEM_ACTION, "Load a state");
        }
        mdrt_menu_add(m, I_VIDEO, MDRT_ITEM_ACTION, "Video");
        mdrt_menu_add(m, I_CAPTURE, MDRT_ITEM_ACTION, "Capture");
        mdrt_menu_add(m, I_SOUND, MDRT_ITEM_ACTION, "Sound");
        mdrt_menu_add(m, I_PLAYERS, MDRT_ITEM_ACTION, "Players");
        mdrt_menu_add(m, I_OPTIONS, MDRT_ITEM_ACTION, "Options");
        mdrt_menu_add(m, I_KEYS, MDRT_ITEM_ACTION, "Keys");
        mdrt_menu_add(m, I_QUIT, MDRT_ITEM_ACTION, "Quit");
        if (!running && s->o->rom_check) snprintf(m->subtitle, sizeof m->subtitle, "Choose a ROM to play");
        break;
    case PAGE_VIDEO:
        mdrt_menu_page(m, "Video");
        mdrt_menu_add_choice(m, I_UPSCALE, "Upscaler", s->upscale_label, MDRT_UPSCALE_COUNT, s->upscale);
        mdrt_menu_add_choice(m, I_SCALE, "Scaling", scale_name, 3, s->scale);
        mdrt_menu_add_choice(m, I_ASPECT, "Aspect", aspect_label, 2, s->aspect);
        mdrt_menu_add_choice(m, I_COLOURS, "Colours", colours_label, 3, s->colours);
        mdrt_menu_add_choice(m, I_BLEND, "Blend", blend_label, MDRT_BLEND_COUNT, s->blend);
        mdrt_menu_add_choice(m, I_SCANLINES, "Scanlines", scanlines_name, MDRT_SCANLINES_COUNT, s->scanlines);
        mdrt_menu_add_choice(m, I_MASK, "Mask", mask_label, MDRT_MASK_COUNT, s->mask);
        mdrt_menu_add_choice(m, I_FULL, "Full screen", off_on, 2, is_fullscreen(s));
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        snprintf(m->subtitle, sizeof m->subtitle, "Sharp: smooth edges. Integer: even pixels. Fit: fills.");
        break;
    case PAGE_CAPTURE: {
        char a[64], b[64];
        mdrt_menu_page(m, "Capture");
        it = mdrt_menu_add(m, I_CAP_REPLAY, s->replay ? MDRT_ITEM_ACTION : MDRT_ITEM_TEXT, "Save the replay");
        if (it) {
            if (s->replay) snprintf(it->detail, sizeof it->detail, "the last %d s", replay_secs[s->replay]);
            else snprintf(it->detail, sizeof it->detail, "(off)");
        }
        if ((it = mdrt_menu_add(m, I_CAP_RECORD, MDRT_ITEM_ACTION, s->rec ? "Stop recording" : "Start recording")) && s->rec) {
            uint32_t t = (SDL_GetTicks() - s->rec_since) / 1000;
            snprintf(it->detail, sizeof it->detail, "%u:%02u", t / 60, t % 60);
        }
        mdrt_menu_add_choice(m, I_CAP_SECONDS, "Replay kept", replay_label, 5, s->replay);
        mdrt_menu_add_choice(m, I_CAP_GIFSCALE, "GIF size", gif_scale_label, 3, s->gif_scale);
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        control_detail(s, K_REPLAY, a, sizeof a);
        control_detail(s, K_RECORD, b, sizeof b);
        snprintf(m->subtitle, sizeof m->subtitle, "%s: the replay. %s: record.", a, b);
        break;
    }
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
        mdrt_menu_add_choice(m, I_RESUME_OPT, "Resume where you left off", off_on, 2, s->resume);
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        break;
    }
    case PAGE_KEYS:
        mdrt_menu_page(m, "Keys");
        snprintf(m->subtitle, sizeof m->subtitle, "Enter: change one");
        for (int p = 0; p < MDRT_PADS; p++) {
            char label[48];
            for (int b = 0; b < BUTTONS; b++) {
                snprintf(label, sizeof label, "Pad %d %s", p + 1, button_label[b]);
                if ((it = mdrt_menu_add(m, I_PAD + p * BUTTONS + b, MDRT_ITEM_ACTION, label)))
                    snprintf(it->detail, sizeof it->detail, "%s", key_name(s->button_key[p][b]));
            }
            for (int t = 0; t < TURBOS; t++) {
                snprintf(label, sizeof label, "Pad %d %s", p + 1, turbo_label[t]);
                if ((it = mdrt_menu_add(m, I_TURBOKEY + p * TURBOS + t, MDRT_ITEM_ACTION, label)))
                    snprintf(it->detail, sizeof it->detail, "%s", key_name(s->turbo_key[p][t]));
            }
        }
        for (int k = 0; k < K_COUNT; k++)
            if ((it = mdrt_menu_add(m, I_CONTROL + k, MDRT_ITEM_ACTION, key_setting[k][2]))) control_detail(s, k, it->detail, sizeof it->detail);
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        break;
    case PAGE_PLAYERS: {
        mdrt_menu_page(m, "Players");
        snprintf(m->subtitle, sizeof m->subtitle, "%s", s->adaptor ? "EA 4 Way Play: four players (from power on)"
                                                                   : "Players 3 and 4 need EA's 4 Way Play");
        mdrt_menu_add_choice(m, I_ADAPTOR, "Controllers", adaptor_label, 2, s->adaptor);
        ctrl_choices(s);
        for (int p = 0; p < MDRT_PADS; p++) {
            char label[48];
            snprintf(label, sizeof label, "Player %d", p + 1);
            mdrt_menu_add_choice(m, I_PLAYER + p, label, s->ctrl_choice, SLOTS + 1, s->player_ctrl[p]);
        }
        mdrt_menu_add(m, I_CBUTTONS, MDRT_ITEM_ACTION, "Controller buttons");
        int rate = 2;
        for (int i = 0; i < 6; i++) if (turbo_rates[i] == s->turbo_rate) rate = i;
        mdrt_menu_add_choice(m, I_TURBO, "Turbo", turbo_rate_label, 6, rate);
        mdrt_menu_add_number(m, I_DEADZONE, "Stick dead zone", s->dead_zone, 10, 90, 10, "%");
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        break;
    }
    case PAGE_CBUTTONS:
        mdrt_menu_page(m, "Controller buttons");
        snprintf(m->subtitle, sizeof m->subtitle, "Enter: change one (Esc: keep, Delete: none)");
        for (int i = 0; i < CBUTTONS; i++)
            if ((it = mdrt_menu_add(m, I_CBUTTON + i, MDRT_ITEM_ACTION, cbutton_label[i])))
                snprintf(it->detail, sizeof it->detail, "%s", s->cbutton[i] == SDL_CONTROLLER_BUTTON_INVALID ? "none"
                         : SDL_GameControllerGetStringForButton(s->cbutton[i]));
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        break;
    case PAGE_SAVE: case PAGE_LOAD:
        mdrt_menu_page(m, page == PAGE_SAVE ? "Save a state" : "Load a state");
        snprintf(m->subtitle, sizeof m->subtitle, "%s", page == PAGE_SAVE ? "States are taken during play" : "Then the game goes on from there");
        for (int i = 1; i <= SLOT_COUNT; i++) {
            char label[64];
            int full = slot_label(s, i, label, sizeof label);
            mdrt_menu_add(m, I_SLOT + i, page == PAGE_LOAD && !full ? MDRT_ITEM_TEXT : MDRT_ITEM_ACTION, label);
        }
        mdrt_menu_add(m, I_BACK, MDRT_ITEM_ACTION, "Back");
        mdrt_menu_select(m, select);
        slot_picture(s);
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
    for (int p = 0; p < MDRT_PADS; p++) {
        SDL_GameController *c = player_ctrl(s, p);
        s->pad_ignore[p] = c ? ctrl_buttons(s, c, 1) : 0;
    }
    SDL_AtomicSet(&s->in_menu, 0);
    SDL_LockMutex(s->lock);
    s->fresh = 1;                                         /* (the picture without the menu) */
    SDL_UnlockMutex(s->lock);
    title_now(s);
}

static void quit(sdl *s) { SDL_AtomicSet(&s->quit, 1); }

static void set_request_path(sdl *s, const char *path)
{
    SDL_LockMutex(s->lock);
    snprintf(s->req_path, sizeof s->req_path, "%s", path);
    SDL_UnlockMutex(s->lock);
}

/* a state into a slot (0: quick resume's): the game writes slotN.state.new when it can (in play),
 * which becomes slotN.state once whole, with the picture as it was asked for beside it */
static void ask_save(sdl *s, int slot)
{
    if (!s->thread || s->pending >= 0) return;
    char path[1100];
    const char *dir = mdrt_settings_get(s->set, "general.states");
    make_dir(dir && *dir ? dir : ".");
    slot_path(s, path, sizeof path, slot, ".state.new");
    remove(path);
    SDL_LockMutex(s->lock);
    if (!s->pending_pic) s->pending_pic = malloc(320 * 240 * sizeof *s->pending_pic);
    if (s->pending_pic) memcpy(s->pending_pic, s->picture, sizeof s->picture);
    s->pending_w = s->width;
    s->pending_h = s->height;
    SDL_UnlockMutex(s->lock);
    set_request_path(s, path);
    s->pending = slot;
    s->pending_size = -1;
    s->pending_until = SDL_GetTicks() + 1500;
    SDL_AtomicAdd(&s->saves, 1);
}

static void ask_load(sdl *s, int slot)
{
    char path[1100], msg[64];
    slot_path(s, path, sizeof path, slot, ".state");
    if (!s->thread || file_size(path) <= 0) {
        if (slot) { snprintf(msg, sizeof msg, "slot %d is empty", slot); notice(s, msg); }
        return;
    }
    set_request_path(s, path);
    SDL_AtomicAdd(&s->loads, 1);
    if (slot) snprintf(msg, sizeof msg, "state loaded: slot %d", slot);
    else snprintf(msg, sizeof msg, "back where you left off");
    notice(s, msg);
}

/* a save asked for: whole (its size has stopped changing) or not coming (out of play) */
static void check_pending(sdl *s)
{
    if (s->pending < 0) return;
    char made[1100], path[1100], msg[64];
    slot_path(s, made, sizeof made, s->pending, ".state.new");
    slot_path(s, path, sizeof path, s->pending, ".state");
    long n = file_size(made);
    if (n > 0 && n == s->pending_size) {                  /* (whole: the same size twice) */
        remove(path);                                     /* (Windows renames over nothing) */
        if (rename(made, path) == 0 && s->pending) {
            slot_path(s, path, sizeof path, s->pending, ".png");
            size_t len = s->pending_pic && s->pending_w ? mdrt_png_encode(s->pending_pic, s->pending_w, s->pending_h, 320, NULL, 0) : 0;
            uint8_t *png = len ? malloc(len) : NULL;
            FILE *f = png ? fopen(path, "wb") : NULL;
            if (f) { mdrt_png_encode(s->pending_pic, s->pending_w, s->pending_h, 320, png, len); fwrite(png, 1, len, f); fclose(f); }
            free(png);
            snprintf(msg, sizeof msg, "state saved: slot %d", s->pending);
            notice(s, msg);
        }
        s->pending = -1;
    } else if (SDL_GetTicks() > s->pending_until) {       /* not in play: nothing changed */
        remove(made);
        if (s->pending) notice(s, "states are saved during play");
        else remove(path);                                /* (quick resume: no old moment to come back to) */
        s->pending = -1;
    } else s->pending_size = n;
}

/* the window closing: with quick resume, the game's state first (in play), then quit */
static void request_quit(sdl *s)
{
    if (!s->resume || !s->thread || s->quitting) { quit(s); return; }
    s->quitting = 1;
    if (s->menu_open) close_menu(s);
    SDL_AtomicSet(&s->paused, 0);
    s->auto_paused = 0;
    if (s->pending >= 0) s->pending = -1;                 /* (a slot's save gives way) */
    ask_save(s, 0);
}

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

/* a controller button for a pad's button (SDL_CONTROLLER_BUTTON_INVALID: none) */
static void bind_cbutton(sdl *s, int id, SDL_GameControllerButton b)
{
    char setting[32];
    s->binding = -1;
    s->cbutton[id - I_CBUTTON] = b;
    snprintf(setting, sizeof setting, "controller.%s", cbutton_name[id - I_CBUTTON]);
    mdrt_settings_set(s->set, setting, b == SDL_CONTROLLER_BUTTON_INVALID ? "" : SDL_GameControllerGetStringForButton(b));
    build_page(s, PAGE_CBUTTONS, id);
    for (int i = 0; i < CBUTTONS && b != SDL_CONTROLLER_BUTTON_INVALID; i++)   /* the button's other use, if it has one */
        if (i != id - I_CBUTTON && s->cbutton[i] == b)
            snprintf(s->menu.message, sizeof s->menu.message, "%s is also %s", SDL_GameControllerGetStringForButton(b), cbutton_label[i]);
}

/* a key for the item being bound (Esc keeps the old one, Delete leaves none) */
static int is_modifier(SDL_Scancode k)
{
    return k == SDL_SCANCODE_LSHIFT || k == SDL_SCANCODE_RSHIFT || k == SDL_SCANCODE_LCTRL || k == SDL_SCANCODE_RCTRL
        || k == SDL_SCANCODE_LALT || k == SDL_SCANCODE_RALT || k == SDL_SCANCODE_LGUI || k == SDL_SCANCODE_RGUI;
}

static void bind_key(sdl *s, SDL_Scancode k, SDL_Keymod mod)
{
    int id = s->binding, sel = id;
    if (id >= I_CONTROL && id < I_TURBOKEY && is_modifier(k)) return;   /* (a control: Shift, Ctrl or Alt waits for its key) */
    s->binding = -1;
    if (id >= I_CBUTTON) {                                /* (a controller button: the keyboard keeps or clears it) */
        if (k == SDL_SCANCODE_DELETE) bind_cbutton(s, id, SDL_CONTROLLER_BUTTON_INVALID);
        else if (k == SDL_SCANCODE_ESCAPE) build_page(s, PAGE_CBUTTONS, sel);
        else s->binding = id;
        return;
    }
    if (k == SDL_SCANCODE_ESCAPE) { build_page(s, PAGE_KEYS, sel); return; }
    if (k == SDL_SCANCODE_DELETE) k = SDL_SCANCODE_UNKNOWN;
    char setting[32];
    const char *other = NULL;
    if (id >= I_TURBOKEY) {
        int p = (id - I_TURBOKEY) / TURBOS, t = (id - I_TURBOKEY) % TURBOS;
        s->turbo_key[p][t] = k;
        snprintf(setting, sizeof setting, "pad%d.%s", p + 1, turbo_name[t]);
    } else if (id >= I_CONTROL) {
        if (id - I_CONTROL == K_MENU && k == SDL_SCANCODE_UNKNOWN) {
            build_page(s, PAGE_KEYS, sel);
            snprintf(s->menu.message, sizeof s->menu.message, "The menu needs a key");
            return;
        }
        s->key[id - I_CONTROL] = k;
        s->key_mod[id - I_CONTROL] = k == SDL_SCANCODE_UNKNOWN ? 0 : (mod & KMOD_SHIFT) ? KMOD_SHIFT : (mod & KMOD_CTRL) ? KMOD_CTRL
                                     : (mod & KMOD_ALT) ? KMOD_ALT : 0;
        snprintf(setting, sizeof setting, "%s", key_setting[id - I_CONTROL][0]);
    } else {
        int p = (id - I_PAD) / BUTTONS, b = (id - I_PAD) % BUTTONS;
        s->button_key[p][b] = k;
        snprintf(setting, sizeof setting, "pad%d.%s", p + 1, button_name[b]);
    }
    char name[64];
    if (id >= I_CONTROL && id < I_TURBOKEY) control_name(s, id - I_CONTROL, name, sizeof name);
    else snprintf(name, sizeof name, "%s", k == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(k));
    mdrt_settings_set(s->set, setting, name);
    build_page(s, PAGE_KEYS, sel);
    for (int i = 0; i < s->menu.count && name[0]; i++)    /* the key's other use, if it has one */
        if (s->menu.items[i].id != id && s->menu.items[i].id >= I_PAD && !strcmp(s->menu.items[i].detail, name))
            other = s->menu.items[i].label;
    if (other) snprintf(s->menu.message, sizeof s->menu.message, "%s is also %s", name, other);
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
    case I_SAVE: build_page(s, PAGE_SAVE, I_SLOT + s->slot); return;
    case I_LOAD: build_page(s, PAGE_LOAD, I_SLOT + s->slot); return;
    case I_RESUME_OPT:
        s->resume = it->value;
        mdrt_settings_set_int(s->set, "general.resume", s->resume);
        return;
    case I_VIDEO: build_page(s, PAGE_VIDEO, I_UPSCALE); return;
    case I_CAPTURE: build_page(s, PAGE_CAPTURE, s->replay ? I_CAP_REPLAY : I_CAP_RECORD); return;
    case I_CAP_REPLAY:                                    /* (saved while the menu stays) */
        snprintf(s->menu.message, sizeof s->menu.message, "%s", save_replay(s));
        return;
    case I_CAP_RECORD:
        if (s->rec) {
            record(s);
            build_page(s, PAGE_CAPTURE, I_CAP_RECORD);
            snprintf(s->menu.message, sizeof s->menu.message, "Saving the recording");
        } else if (running) { close_menu(s); record(s); }   /* (recording from the moment play goes on) */
        else snprintf(s->menu.message, sizeof s->menu.message, "No game is running");
        return;
    case I_CAP_SECONDS:
        s->replay = it->value;
        mdrt_settings_set_int(s->set, "capture.replay", replay_secs[s->replay]);
        capture_set(s);
        build_page(s, PAGE_CAPTURE, I_CAP_SECONDS);
        return;
    case I_CAP_GIFSCALE:
        s->gif_scale = it->value;
        mdrt_settings_set_int(s->set, "capture.gif_scale", s->gif_scale + 1);
        return;
    case I_SOUND: build_page(s, PAGE_SOUND, I_VOLUME); return;
    case I_OPTIONS: build_page(s, PAGE_OPTIONS, I_FOCUS); return;
    case I_KEYS: build_page(s, PAGE_KEYS, I_PAD); return;
    case I_PLAYERS: build_page(s, PAGE_PLAYERS, I_ADAPTOR); return;
    case I_CBUTTONS: build_page(s, PAGE_CBUTTONS, I_CBUTTON); return;
    case I_ADAPTOR:
        s->adaptor = it->value;
        mdrt_settings_set(s->set, "input.adaptor", adaptor_name[s->adaptor]);
        build_page(s, PAGE_PLAYERS, I_ADAPTOR);
        if (running) snprintf(s->menu.message, sizeof s->menu.message, "From the next power on: Reset");
        return;
    case I_TURBO:
        s->turbo_rate = turbo_rates[it->value];
        mdrt_settings_set_int(s->set, "general.turbo_rate", s->turbo_rate);
        return;
    case I_DEADZONE:
        s->dead_zone = it->value;
        mdrt_settings_set_int(s->set, "controller.dead_zone", s->dead_zone);
        return;
    case I_QUIT: if (!running || confirmed(s, id, "quit")) request_quit(s); return;
    case I_BACK:
        if (s->page == PAGE_CBUTTONS) { build_page(s, PAGE_PLAYERS, I_CBUTTONS); return; }
        build_page(s, PAGE_MAIN, s->page == PAGE_VIDEO ? I_VIDEO : s->page == PAGE_CAPTURE ? I_CAPTURE : s->page == PAGE_SOUND ? I_SOUND : s->page == PAGE_OPTIONS ? I_OPTIONS
                                 : s->page == PAGE_PLAYERS ? I_PLAYERS : s->page == PAGE_SAVE ? I_SAVE : s->page == PAGE_LOAD ? I_LOAD : I_KEYS);
        return;
    case I_UPSCALE:                                       /* (the frame is upscaled again when the menu closes) */
        s->upscale = it->value;
        mdrt_settings_set(s->set, "video.upscaler", mdrt_upscale_name(s->upscale));
        return;
    case I_SCALE:
        s->scale = it->value;
        mdrt_settings_set(s->set, "video.scale", scale_name[s->scale]);
        return;
    case I_ASPECT:
        s->aspect = it->value;
        mdrt_settings_set(s->set, "video.aspect", aspect_name[s->aspect]);
        return;
    case I_COLOURS:                                       /* (the picture under the menu made again) */
        s->colours = it->value;
        mdrt_settings_set(s->set, "video.colours", colours_name[s->colours]);
        SDL_AtomicSet(&s->curve, colours_curve[s->colours]);
        SDL_LockMutex(s->lock);
        for (int y = 0; y < s->height; y++)
            for (int x = 0; x < s->width; x++) s->picture[y * 320 + x] = 0xFF000000u | mdrt_vdp_rgb(s->raw[y * 320 + x], colours_curve[s->colours]);
        s->fresh = 1;
        SDL_UnlockMutex(s->lock);
        return;
    case I_BLEND:                                         /* (these three: from when the menu closes) */
        s->blend = it->value;
        mdrt_settings_set(s->set, "video.blend", blend_name[s->blend]);
        return;
    case I_SCANLINES:
        s->scanlines = it->value;
        mdrt_settings_set(s->set, "video.scanlines", scanlines_name[s->scanlines]);
        return;
    case I_MASK:
        s->mask = it->value;
        mdrt_settings_set(s->set, "video.mask", mask_name[s->mask]);
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
    } else if (id > I_SLOT && id <= I_SLOT + SLOT_COUNT) {     /* a save slot: into it, or from it */
        s->slot = id - I_SLOT;
        mdrt_settings_set_int(s->set, "general.slot", s->slot);
        int saving = s->page == PAGE_SAVE;
        close_menu(s);
        if (saving) ask_save(s, s->slot);
        else ask_load(s, s->slot);
    } else if (id >= I_PLAYER && id < I_PLAYER + MDRT_PADS) {   /* a player's controller */
        char k[32];
        s->player_ctrl[id - I_PLAYER] = it->value;
        snprintf(k, sizeof k, "player%d.controller", id - I_PLAYER + 1);
        mdrt_settings_set_int(s->set, k, it->value);
    } else if (id >= I_CBUTTON && id < I_CBUTTON + CBUTTONS) {   /* a controller button to change */
        s->binding = id;
        snprintf(s->menu.message, sizeof s->menu.message, "Press a controller's button for %s", it->label);
    } else if (id >= I_PAD && id < I_ENTRY) {             /* a key to change */
        s->binding = id;
        snprintf(s->menu.message, sizeof s->menu.message, "Press a key for %s (Esc: keep, Delete: none)", it->label);
    }
}

static void menu_input(sdl *s, int in)
{
    int id = mdrt_menu_input(&s->menu, in);
    if (s->confirm && id != s->confirm) { s->confirm = 0; s->menu.message[0] = 0; }   /* (not chosen again) */
    if (s->page == PAGE_SAVE || s->page == PAGE_LOAD) slot_picture(s);
    if (id == MDRT_MENU_CLOSE) {
        if (s->page == PAGE_ROM) build_page(s, PAGE_MAIN, I_ROM);
        else if (s->page != PAGE_MAIN) act(s, I_BACK);   /* (the controller buttons' page: back to Players) */
        else if (s->thread) close_menu(s);
    } else if (id >= 0) act(s, id);
    s->redraw = 1;
}

/* ---- keys and controllers ------------------------------------------------------------------------ */

static void key_pressed(sdl *s, SDL_Scancode k, SDL_Keymod mod, int repeat)
{
    if (k == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) { if (!repeat) set_fullscreen(s, !is_fullscreen(s)); return; }
    if (s->menu_open) {
        if (s->binding >= 0) { if (!repeat) bind_key(s, k, mod); s->redraw = 1; return; }
        int in = k == SDL_SCANCODE_UP ? MDRT_MENU_UP : k == SDL_SCANCODE_DOWN ? MDRT_MENU_DOWN :
                 k == SDL_SCANCODE_LEFT ? MDRT_MENU_LEFT : k == SDL_SCANCODE_RIGHT ? MDRT_MENU_RIGHT :
                 k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE ? MDRT_MENU_OK :
                 k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_BACKSPACE || k == s->key[K_MENU] ? MDRT_MENU_BACK : -1;
        if (in == MDRT_MENU_OK || in == MDRT_MENU_BACK) { if (repeat) return; }
        if (in >= 0) menu_input(s, in);
        return;
    }
    if (repeat) return;
    int a = K_COUNT, held_mods = mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT);
    for (int i = 0; i < K_COUNT && a == K_COUNT; i++)     /* a control with its Shift, Ctrl or Alt held first */
        if (s->key[i] == k && k != SDL_SCANCODE_UNKNOWN && s->key_mod[i] && (held_mods & s->key_mod[i])) a = i;
    for (int i = 0; i < K_COUNT && a == K_COUNT; i++)
        if (s->key[i] == k && k != SDL_SCANCODE_UNKNOWN && !s->key_mod[i]) a = i;
    switch (a) {
    case K_MENU: open_menu(s, PAGE_MAIN, I_RESUME); break;
    case K_PAUSE:
        SDL_AtomicSet(&s->paused, !SDL_AtomicGet(&s->paused));
        s->auto_paused = 0;
        s->notice_until = 0;
        title_now(s);
        break;
    case K_SLOW:
        SDL_AtomicSet(&s->slow, !SDL_AtomicGet(&s->slow));
        notice(s, SDL_AtomicGet(&s->slow) ? "slow motion" : "full speed");
        break;
    case K_STEP: if (SDL_AtomicGet(&s->paused)) SDL_AtomicSet(&s->step, 1); break;
    case K_SHOT: screenshot(s); break;
    case K_REPLAY: (void)save_replay(s); break;
    case K_RECORD: record(s); break;
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
    if (s->binding >= I_CBUTTON) { bind_cbutton(s, s->binding, b); s->redraw = 1; return; }
    if (s->binding >= 0) return;
    int in = b == SDL_CONTROLLER_BUTTON_DPAD_UP ? MDRT_MENU_UP : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? MDRT_MENU_DOWN :
             b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? MDRT_MENU_LEFT : b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? MDRT_MENU_RIGHT :
             b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START ? MDRT_MENU_OK :
             b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_BACK || b == SDL_CONTROLLER_BUTTON_GUIDE ? MDRT_MENU_BACK : -1;
    if (in >= 0) menu_input(s, in);
}

/* a control's key held (with its Shift, Ctrl or Alt) */
static int control_held(sdl *s, int c)
{
    return s->key[c] != SDL_SCANCODE_UNKNOWN && s->down[s->key[c]] && (!s->key_mod[c] || (SDL_GetModState() & s->key_mod[c]));
}

static void read_buttons(sdl *s)
{
    int turbo_on = (int)(SDL_GetTicks() * (Uint32)s->turbo_rate * 2 / 1000) % 2 == 0;   /* (pressed half the time) */
    for (int p = 0; p < MDRT_PADS; p++) {
        uint16_t b = 0, pad = 0;
        for (int i = 0; i < BUTTONS; i++) {
            SDL_Scancode k = s->button_key[p][i];
            if (k != SDL_SCANCODE_UNKNOWN && s->down[k]) b |= button_bit[i];
        }
        for (int t = 0; t < TURBOS && turbo_on; t++) {
            SDL_Scancode k = s->turbo_key[p][t];
            if (k != SDL_SCANCODE_UNKNOWN && s->down[k]) b |= turbo_bit[t];
        }
        SDL_GameController *c = player_ctrl(s, p);
        if (c) pad = ctrl_buttons(s, c, turbo_on);
        s->pad_ignore[p] &= pad;                          /* (let go: counted again) */
        b |= pad & ~s->pad_ignore[p];
        SDL_AtomicSet(&s->buttons[p], s->menu_open ? 0 : b);
    }
    SDL_AtomicSet(&s->rewind, control_held(s, K_REWIND));
    SDL_AtomicSet(&s->fast, control_held(s, K_FAST));
}

/* ---- the picture ------------------------------------------------------------------------------- */

/* room for the game's frame taken out and blended */
static int frame_ready(sdl *s)
{
    if (!s->frame) s->frame = malloc(320 * 240 * sizeof *s->frame);
    if (!s->blended) s->blended = malloc(320 * 240 * sizeof *s->blended);
    return s->frame && s->blended;
}

/* room for the upscaled frame at factor f (the buffers once, at the largest; a texture for f) */
static int big_ready(sdl *s, int f)
{
    if (!s->big) s->big = malloc(320 * 240 * 16 * sizeof *s->big);
    if (!s->big_scratch) s->big_scratch = malloc(320 * 240 * 4 * sizeof *s->big_scratch);
    if (s->big_tex && s->big_f != f) { SDL_DestroyTexture(s->big_tex); s->big_tex = NULL; }
    if (!s->big_tex) s->big_tex = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320 * f, 240 * f);
    s->big_f = f;
    return s->big && s->big_scratch && s->big_tex;
}

/* the scanlines and mask for a picture of cols x lines shown w x h (made again when either changes) */
static int tube_ready(sdl *s, int w, int h, int cols, int lines)
{
    if (s->tube && s->tube_w == w && s->tube_h == h && s->tube_cols == cols && s->tube_lines == lines
        && s->tube_scan == s->scanlines && s->tube_mask == s->mask) return 1;
    if (s->tube) { SDL_DestroyTexture(s->tube); s->tube = NULL; }
    uint32_t *px = w > 0 && h > 0 ? malloc((size_t)w * (size_t)h * sizeof *px) : NULL;
    if (px) {
        mdrt_look_overlay(s->scanlines, s->mask, w, h, cols, lines, px, w);
        s->tube = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (s->tube) { SDL_UpdateTexture(s->tube, NULL, px, w * 4); SDL_SetTextureBlendMode(s->tube, SDL_BLENDMODE_MOD); }
    }
    free(px);
    s->tube_w = w; s->tube_h = h; s->tube_cols = cols; s->tube_lines = lines;
    s->tube_scan = s->scanlines; s->tube_mask = s->mask;
    return s->tube != NULL;
}

/* the game's frame through the looks: blended, upscaled, scaled to the window, then the tube's
 * scanlines and mask multiplied in and the picture added again at their lift (the menu: as it is) */
static void draw(sdl *s)
{
    int game = !s->menu_open, method = game ? s->upscale : MDRT_UPSCALE_OFF, blend = game ? s->blend : MDRT_BLEND_OFF;
    int tube = game && mdrt_look_tube(s->scanlines, s->mask);
    if ((method || blend) && !frame_ready(s)) method = blend = 0;
    int f = mdrt_upscale_factor(method);
    if (method && !big_ready(s, f)) { method = MDRT_UPSCALE_OFF; f = 1; }
    int work = method || blend;
    SDL_LockMutex(s->lock);
    int fresh = s->fresh, w = s->width, h = s->height;
    if (s->menu_open && (fresh || s->redraw)) {          /* the menu over the picture as it stands */
        if (!w) { w = 320; h = 224; memset(s->menu_px, 0, sizeof s->menu_px); }
        else for (int y = 0; y < h; y++) memcpy(s->menu_px + y * 320, s->picture + y * 320, (size_t)w * 4);
    } else if (fresh && work) {                           /* (taken, then worked on outside the lock) */
        for (int y = 0; y < h; y++) memcpy(s->frame + y * 320, s->picture + y * 320, (size_t)w * 4);
    } else if (fresh) SDL_UpdateTexture(s->tex, NULL, s->picture, 320 * 4);
    s->fresh = 0;
    SDL_UnlockMutex(s->lock);
    if ((!fresh && !s->redraw) || (!w && !s->menu_open)) return;
    SDL_Texture *tex = s->tex;
    int cols = w, lines = h;                              /* (the picture's own size, for the scanlines) */
    if (s->menu_open) {
        if (!w) { w = 320; h = 224; }
        mdrt_menu_draw(&s->menu, s->menu_px, w, h, 320);
        SDL_UpdateTexture(s->tex, NULL, s->menu_px, 320 * 4);
    } else if (work) {
        if (fresh) {
            const uint32_t *px = s->frame;
            if (blend) { mdrt_look_blend(blend, s->frame, w, h, 320, s->blended, 320); px = s->blended; }
            if (method) {                                 /* the game's frame, magnified */
                mdrt_upscale(method, px, w, h, 320, s->big, w * f, s->big_scratch);
                SDL_Rect r = { 0, 0, w * f, h * f };
                SDL_UpdateTexture(s->big_tex, &r, s->big, w * f * 4);
            } else {
                SDL_Rect r = { 0, 0, w, h };
                SDL_UpdateTexture(s->tex, &r, px, 320 * 4);
            }
        }
        if (method) { tex = s->big_tex; w *= f; h *= f; }
    }
    s->redraw = 0;
    int ww, wh;
    SDL_GetRendererOutputSize(s->ren, &ww, &wh);
    mdrt_view v = mdrt_view_place(ww, wh, w, h, s->scale, s->aspect);
    SDL_Rect src = { 0, 0, w, h }, dst = { v.x, v.y, v.w, v.h }, *from = &src;
    SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
    SDL_RenderClear(s->ren);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(tex, s->scale == MDRT_SCALE_SHARP ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
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
            SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
            SDL_SetRenderTarget(s->ren, s->pre);
            SDL_RenderCopy(s->ren, tex, &src, NULL);
            SDL_SetRenderTarget(s->ren, NULL);
            tex = s->pre;
            from = NULL;
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
        }
    }
    SDL_RenderCopy(s->ren, tex, from, &dst);
    int lift = tube ? mdrt_look_lift(s->scanlines, s->mask) : 0;
    if (tube && tube_ready(s, v.w, v.h, cols, lines)) {
        SDL_RenderCopy(s->ren, s->tube, NULL, &dst);
        if (lift) {                                       /* (the light they took, given back) */
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
            SDL_SetTextureColorMod(tex, (Uint8)lift, (Uint8)lift, (Uint8)lift);
            SDL_RenderCopy(s->ren, tex, from, &dst);
            SDL_SetTextureColorMod(tex, 255, 255, 255);
        }
    }
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
    for (int i = 0; i < SDL_NumJoysticks(); i++) ctrl_added(s, i);
    s->lock = SDL_CreateMutex();
    s->cap_lock = SDL_CreateMutex();
    s->save_lock = SDL_CreateMutex();
    s->cap = mdrt_capture_new(0);
    if (!s->cap) s->replay = 0;
    else capture_set(s);
    cfg->platform = (mdrt_platform){ .user = s, .present = present, .audio = audio, .wait = wait, .input = input, .requests = requests,
                                     .request_path = request_path };
    mdrt_menu_init(&s->menu);
    char why[1200] = "";
    if (o->rom_check && !first_rom(s, why, sizeof why)) {   /* no ROM yet: the menu's ROM page */
        act(s, I_ROM);
        s->menu_open = 1;
        SDL_AtomicSet(&s->in_menu, 1);
        put_text(s->menu.message, sizeof s->menu.message, why[0] ? why : o->rom_hint ? o->rom_hint : "", 0);
    } else if (!start_game(s)) { fprintf(stderr, "the game could not start (no FM core %s in this build?)\n", cfg->fm_core); SDL_Quit(); return 1; }
    else if (s->resume) ask_load(s, 0);                   /* (quick resume: where the last run left off, if it was in play) */
    while (!SDL_AtomicGet(&s->quit)) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) do {
            if (e.type == SDL_QUIT) request_quit(s);
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                SDL_Scancode k = e.key.keysym.scancode;
                if (k > SDL_SCANCODE_UNKNOWN && k < SDL_NUM_SCANCODES && !s->menu_open) s->down[k] = e.type == SDL_KEYDOWN;
                if (e.type == SDL_KEYDOWN) key_pressed(s, k, (SDL_Keymod)e.key.keysym.mod, e.key.repeat);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) pad_pressed(s, (SDL_GameControllerButton)e.cbutton.button);
            else if (e.type == SDL_WINDOWEVENT) window_event(s, &e.window);
            else if (e.type == SDL_RENDER_TARGETS_RESET) s->redraw = 1;
            else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (e.type == SDL_CONTROLLERDEVICEADDED) ctrl_added(s, e.cdevice.which);
                else ctrl_removed(s, e.cdevice.which);
                if (s->menu_open && s->page == PAGE_PLAYERS && s->binding < 0) build_page(s, PAGE_PLAYERS, s->menu.items[s->menu.sel].id);
            }
        } while (SDL_PollEvent(&e));
        if (s->thread && SDL_AtomicGet(&s->done) && !SDL_AtomicGet(&s->stop)) break;   /* the game ended by itself */
        read_buttons(s);
        draw(s);
        check_pending(s);
        check_saves(s, 0);
        capture_status(s);
        if (s->quitting && s->pending < 0) quit(s);       /* (quick resume's state saved, or not to be) */
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
    if (s->rec) record(s);                                /* (a recording at quitting: saved) */
    if (saving_percent(s) >= 0) { printf("finishing the GIF...\n"); fflush(stdout); }
    check_saves(s, 1);
    if (s->dev) SDL_CloseAudioDevice(s->dev);
    mdrt_settings_free(s->set);
    mdrt_menu_free(&s->menu);
    free_entries(s);
    free(s->frame); free(s->blended); free(s->big); free(s->big_scratch); free(s->pending_pic);
    if (stopped) {                                        /* (else the thread still has them) */
        mdrt_free(s->m);
        mdrt_capture_free(s->cap);
        SDL_DestroyMutex(s->lock); SDL_DestroyMutex(s->cap_lock); SDL_DestroyMutex(s->save_lock);
        free(s->loud); free(s->rom); free(s);
    }
    SDL_Quit();
    return 0;
}
