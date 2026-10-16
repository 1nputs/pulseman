/* The SDL2 platform (mdrt/platform_sdl2.h). The game thread hands each picture over through a
 * mutex and queues its sound itself; the main thread draws, handles the keys and the window, and
 * keeps the settings. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(d) _mkdir(d)
#else
#include <sys/stat.h>
#define make_dir(d) mkdir(d, 0777)
#endif
#include "mdrt/platform_sdl2.h"
#include "mdrt/png.h"
#include "mdrt/settings.h"
#include "mdrt/view.h"

/* ---- the keys: what each does, its setting and its default ------------------------------------ */

enum { K_MUTE, K_VOL_DOWN, K_VOL_UP, K_REWIND, K_SLOW, K_FAST, K_SAVE, K_LOAD, K_STEP, K_PAUSE,
       K_FULLSCREEN, K_SHOT, K_QUIT, K_COUNT };
static const char *const key_setting[K_COUNT][2] = {   /* (the controls on the function keys, in order) */
    { "key.mute", "F1" }, { "key.volume_down", "F2" }, { "key.volume_up", "F3" },
    { "key.rewind", "F4" }, { "key.slow_motion", "F5" }, { "key.fast_forward", "F6" },
    { "key.save_state", "F7" }, { "key.load_state", "F8" }, { "key.frame_step", "F9" },
    { "key.pause", "F10" }, { "key.fullscreen", "F11" }, { "key.screenshot", "F12" },
    { "key.quit", "Escape" },
};
enum { BUTTONS = 12 };
static const char *const button_name[BUTTONS] = { "up", "down", "left", "right", "a", "b", "c", "start", "x", "y", "z", "mode" };
static const uint16_t button_bit[BUTTONS] = { MDRT_PAD_UP, MDRT_PAD_DOWN, MDRT_PAD_LEFT, MDRT_PAD_RIGHT, MDRT_PAD_A, MDRT_PAD_B,
                                              MDRT_PAD_C, MDRT_PAD_START, MDRT_PAD_X, MDRT_PAD_Y, MDRT_PAD_Z, MDRT_PAD_MODE };
static const char *const button_default[2][BUTTONS] = {
    { "Up", "Down", "Left", "Right", "Z", "X", "C", "Return", "", "", "", "" },
    { "I", "K", "J", "L", "Q", "W", "E", "Tab", "", "", "", "" },
};
static const char *const scale_name[3] = { "sharp", "integer", "fit" };
static const char *const aspect_name[2] = { "square", "4:3" };

#define VOLUME_STEP 10

typedef struct sdl {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex, *pre;     /* the picture; sharp scaling's enlarged copy */
    int pre_w, pre_h, targets;
    SDL_AudioDeviceID dev;
    uint32_t rate;
    int queue_frames;           /* sound queued before the next frame goes (the latency) */
    int fast_speed;             /* fast-forward's times the speed (0: as fast as it goes) */
    SDL_mutex *lock;
    uint32_t picture[320 * 240];
    int width, height, fresh, redraw;
    SDL_atomic_t buttons[2], quit, paused, done;
    SDL_atomic_t saves, loads, rewind;      /* presses of the state keys since the game last asked; held */
    SDL_atomic_t fast, slow, step, volume, mute;
    SDL_GameController *pads[2];
    const mdrt_sdl_options *o;
    mdrt_config *cfg;
    mdrt *m;
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
    int scale, aspect, focus_pause, auto_paused;
    char title[200];
    uint32_t notice_until;
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

/* the frame's sound, at the volume; none while fast, slow or paused (the clock paces those) */
static void audio(void *user, const int16_t *samples, int frames)
{
    sdl *s = user;
    if (!s->dev || SDL_AtomicGet(&s->fast) || SDL_AtomicGet(&s->slow) || SDL_AtomicGet(&s->paused)) return;
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

static int wait(void *user)
{
    sdl *s = user;
    if (++s->waits % 300 == 0) sram_save(s);              /* every 5 s when written */
    while (SDL_AtomicGet(&s->paused) && !SDL_AtomicGet(&s->quit)) {
        if (SDL_AtomicSet(&s->step, 0)) break;            /* one frame on */
        SDL_Delay(5);
    }
    int fast = SDL_AtomicGet(&s->fast), slow = SDL_AtomicGet(&s->slow);
    if (s->dev && !fast && !slow) {                       /* the sound */
        if (s->queued >= (unsigned)s->queue_frames) {
            Uint32 enough = (Uint32)((unsigned)s->queue_frames * s->rate / 60 * 4);
            while (SDL_GetQueuedAudioSize(s->dev) > enough && !SDL_AtomicGet(&s->quit)) SDL_Delay(1);
        }
        s->next_tick = 0;
    } else if (!fast || s->fast_speed) {                  /* no sound, slow motion, fast-forward: the clock */
        uint64_t freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
        uint64_t period = freq * mdrt_frame_mclks(s->m) / mdrt_master_clock(s->m);
        if (slow) period *= 2;
        if (fast) period /= (uint64_t)s->fast_speed;
        if (!s->next_tick || now > s->next_tick + period * 4) s->next_tick = now;
        s->next_tick += period;
        while ((now = SDL_GetPerformanceCounter()) < s->next_tick && !SDL_AtomicGet(&s->quit)) {
            uint64_t ms = (s->next_tick - now) * 1000 / freq;
            if (ms > 1) SDL_Delay((Uint32)(ms - 1));
        }
    } else s->next_tick = 0;
    return SDL_AtomicGet(&s->quit);
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
            "# pulseman's settings for this game, kept by the game: edit them while it is closed.\n"
            "# Keys are SDL's key names, empty for none: A, 1, F1, Up, Return, Space, Tab, Backspace,\n"
            "# Escape, Left Shift, Right Ctrl, Keypad 8, PageUp, or the key's own sign (, . - = ; [).\n"
            "# On a Mac the function keys need Fn held, unless they are set to act as F1, F2 ...\n"
            "# video.scale: sharp, integer or fit. video.aspect: square or 4:3. audio.volume: 0-100.\n"
            "# audio.device: empty for the default. audio.latency: frames of sound queued (1-10).\n"
            "# general.fast_forward: times the speed while it is held (0: as fast as it goes).\n";
        mdrt_settings_parse(s->set, head, sizeof head - 1);
    }
    static const char *const defaults[][2] = {
        { "window.width", "960" }, { "window.height", "672" }, { "window.fullscreen", "0" },
        { "video.scale", "sharp" }, { "video.aspect", "square" },
        { "audio.volume", "100" }, { "audio.mute", "0" }, { "audio.device", "" }, { "audio.latency", "3" },
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
    s->fast_speed = clamp(mdrt_settings_get_int(s->set, "general.fast_forward", 4), 0, 100);
    s->focus_pause = mdrt_settings_get_int(s->set, "general.pause_on_focus_loss", 1) != 0;
    settings_write(s);                                    /* (a new file, or new settings: written now) */
}

/* ---- the main thread's side ----------------------------------------------------------------- */

static void title_now(sdl *s)
{
    if (SDL_GetTicks() < s->notice_until) return;
    s->notice_until = 0;
    const char *t = s->o->title ? s->o->title : "pulseman";
    if (SDL_AtomicGet(&s->paused)) {
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

static void fullscreen(sdl *s)
{
    int on = !(SDL_GetWindowFlags(s->win) & SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_SetWindowFullscreen(s->win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    mdrt_settings_set_int(s->set, "window.fullscreen", on);
    s->redraw = 1;
}

static void key_pressed(sdl *s, SDL_Scancode k, SDL_Keymod mod)
{
    if (k == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) { fullscreen(s); return; }
    int a = 0;
    while (a < K_COUNT && (s->key[a] != k || k == SDL_SCANCODE_UNKNOWN)) a++;
    switch (a) {
    case K_QUIT: SDL_AtomicSet(&s->quit, 1); break;
    case K_PAUSE:
        SDL_AtomicSet(&s->paused, !SDL_AtomicGet(&s->paused));
        s->auto_paused = 0;
        s->notice_until = 0;
        title_now(s);
        break;
    case K_FULLSCREEN: fullscreen(s); break;
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

static void read_buttons(sdl *s)
{
    static const SDL_GameControllerButton pad[8] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_START,
    };
    for (int p = 0; p < 2; p++) {
        uint16_t b = 0;
        for (int i = 0; i < BUTTONS; i++) {
            SDL_Scancode k = s->button_key[p][i];
            if (k != SDL_SCANCODE_UNKNOWN && s->down[k]) b |= button_bit[i];
            if (i < 8 && s->pads[p] && SDL_GameControllerGetButton(s->pads[p], pad[i])) b |= button_bit[i];
        }
        if (s->pads[p]) {
            int x = SDL_GameControllerGetAxis(s->pads[p], SDL_CONTROLLER_AXIS_LEFTX);
            int y = SDL_GameControllerGetAxis(s->pads[p], SDL_CONTROLLER_AXIS_LEFTY);
            if (y < -16000) b |= MDRT_PAD_UP;
            if (y > 16000) b |= MDRT_PAD_DOWN;
            if (x < -16000) b |= MDRT_PAD_LEFT;
            if (x > 16000) b |= MDRT_PAD_RIGHT;
        }
        SDL_AtomicSet(&s->buttons[p], b);
    }
    SDL_AtomicSet(&s->rewind, s->key[K_REWIND] != SDL_SCANCODE_UNKNOWN && s->down[s->key[K_REWIND]]);
    SDL_AtomicSet(&s->fast, s->key[K_FAST] != SDL_SCANCODE_UNKNOWN && s->down[s->key[K_FAST]]);
}

static void draw(sdl *s)
{
    SDL_LockMutex(s->lock);
    int fresh = s->fresh, w = s->width, h = s->height;
    if (fresh) SDL_UpdateTexture(s->tex, NULL, s->picture, 320 * 4);
    s->fresh = 0;
    SDL_UnlockMutex(s->lock);
    if ((!fresh && !s->redraw) || !w) return;
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
    int full = (SDL_GetWindowFlags(s->win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    switch (e->event) {
    case SDL_WINDOWEVENT_FOCUS_LOST:
        memset(s->down, 0, sizeof s->down);               /* (no key stays held) */
        if (s->focus_pause && !SDL_AtomicGet(&s->paused)) {
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    settings_load(s);
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
    s->m = mdrt_new(cfg);
    if (!s->m) { fprintf(stderr, "out of memory\n"); SDL_Quit(); return 1; }
    SDL_Thread *t = SDL_CreateThread(game_thread, "game", s);
    while (!SDL_AtomicGet(&s->done)) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) do {
            if (e.type == SDL_QUIT) SDL_AtomicSet(&s->quit, 1);
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                SDL_Scancode k = e.key.keysym.scancode;
                if (k > SDL_SCANCODE_UNKNOWN && k < SDL_NUM_SCANCODES) s->down[k] = e.type == SDL_KEYDOWN;
                if (e.type == SDL_KEYDOWN && !e.key.repeat) key_pressed(s, k, (SDL_Keymod)e.key.keysym.mod);
            } else if (e.type == SDL_WINDOWEVENT) window_event(s, &e.window);
            else if (e.type == SDL_RENDER_TARGETS_RESET) s->redraw = 1;
            else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                for (int p = 0; p < 2; p++) if (!s->pads[p]) { s->pads[p] = SDL_GameControllerOpen(e.cdevice.which); break; }
            }
        } while (SDL_PollEvent(&e));
        read_buttons(s);
        draw(s);
        if (s->notice_until) title_now(s);
        if (mdrt_settings_dirty(s->set) && SDL_GetTicks() - s->set_saved > 1000) settings_write(s);
        if (SDL_AtomicGet(&s->quit)) {                    /* give the game a moment to stop */
            for (int i = 0; i < 200 && !SDL_AtomicGet(&s->done); i++) SDL_Delay(10);
            break;
        }
    }
    settings_write(s);
    if (SDL_AtomicGet(&s->done)) SDL_WaitThread(t, NULL);
    else SDL_DetachThread(t);                             /* (it would not stop: the process ends it) */
    if (s->dev) SDL_CloseAudioDevice(s->dev);
    int stopped = SDL_AtomicGet(&s->done);
    mdrt_settings_free(s->set);
    if (stopped) { mdrt_free(s->m); SDL_DestroyMutex(s->lock); free(s->loud); free(s); }   /* (else the thread still has them) */
    SDL_Quit();
    return 0;
}
