/* The SDL2 platform (mdrt/platform_sdl2.h). The game thread hands each picture over through a
 * mutex and queues its sound itself; the main thread draws and handles events. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/platform_sdl2.h"

#define QUEUE_FRAMES 3                                    /* sound queued before the next frame goes */

typedef struct sdl {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_AudioDeviceID dev;
    uint32_t rate;
    SDL_mutex *lock;
    uint32_t picture[320 * 240];
    int width, height, fresh;
    SDL_atomic_t buttons[2], quit, paused, done;
    SDL_atomic_t saves, loads, rewind;      /* presses of the state keys since the game last asked; held */
    SDL_GameController *pads[2];
    const mdrt_sdl_options *o;
    mdrt_config *cfg;
    mdrt *m;
    mdrt_game_fn game;
    void *user;
    uint64_t next_tick;         /* pacing without sound */
    unsigned frames;
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

static void audio(void *user, const int16_t *samples, int frames)
{
    sdl *s = user;
    if (!s->dev) return;
    SDL_QueueAudio(s->dev, samples, (Uint32)frames * 4);
    if (++s->frames == QUEUE_FRAMES) SDL_PauseAudioDevice(s->dev, 0);   /* start once there is some */
}

static int wait(void *user)
{
    sdl *s = user;
    if (s->frames % 300 == 0) sram_save(s);               /* every 5 s when written */
    while (SDL_AtomicGet(&s->paused) && !SDL_AtomicGet(&s->quit)) SDL_Delay(10);
    if (s->dev) {
        Uint32 enough = (Uint32)(QUEUE_FRAMES * s->rate / 60 * 4);
        while (SDL_GetQueuedAudioSize(s->dev) > enough && !SDL_AtomicGet(&s->quit)) SDL_Delay(1);
    } else {                                              /* no sound: the clock */
        uint64_t freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
        uint64_t period = freq * mdrt_frame_mclks(s->m) / mdrt_master_clock(s->m);
        if (!s->next_tick || now > s->next_tick + period * 4) s->next_tick = now;
        s->next_tick += period;
        while ((now = SDL_GetPerformanceCounter()) < s->next_tick && !SDL_AtomicGet(&s->quit)) {
            uint64_t ms = (s->next_tick - now) * 1000 / freq;
            if (ms > 1) SDL_Delay((Uint32)(ms - 1));
        }
    }
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

/* ---- the main thread's side ----------------------------------------------------------------- */

static void read_buttons(sdl *s)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    static const SDL_Scancode keys[2][8] = {
        { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_Z, SDL_SCANCODE_RETURN },
        { SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_TAB },
    };
    static const SDL_GameControllerButton pad[8] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_START,
    };
    static const uint16_t bit[8] = { MDRT_PAD_UP, MDRT_PAD_DOWN, MDRT_PAD_LEFT, MDRT_PAD_RIGHT, MDRT_PAD_B, MDRT_PAD_C, MDRT_PAD_A, MDRT_PAD_START };
    for (int p = 0; p < 2; p++) {
        uint16_t b = 0;
        for (int i = 0; i < 8; i++) {
            if (k[keys[p][i]]) b |= bit[i];
            if (s->pads[p] && SDL_GameControllerGetButton(s->pads[p], pad[i])) b |= bit[i];
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
    SDL_AtomicSet(&s->rewind, k[SDL_SCANCODE_BACKSPACE] ? 1 : 0);
}

static void draw(sdl *s)
{
    SDL_LockMutex(s->lock);
    int fresh = s->fresh, w = s->width, h = s->height;
    if (fresh) SDL_UpdateTexture(s->tex, NULL, s->picture, 320 * 4);
    s->fresh = 0;
    SDL_UnlockMutex(s->lock);
    if (!fresh || !w) return;
    int ww, wh;
    SDL_GetRendererOutputSize(s->ren, &ww, &wh);
    /* the biggest picture that fits: square pixels, or 4:3 */
    double pw = s->o->aspect ? h * 4.0 / 3 : w, k = (double)ww / pw < (double)wh / h ? (double)ww / pw : (double)wh / h;
    SDL_Rect src = { 0, 0, w, h }, dst = { (int)(ww - pw * k) / 2, (int)(wh - h * k) / 2, (int)(pw * k), (int)(h * k) };
    SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
    SDL_RenderClear(s->ren);
    SDL_RenderCopy(s->ren, s->tex, &src, &dst);
    SDL_RenderPresent(s->ren);
}

int mdrt_sdl_run(mdrt_config *cfg, const mdrt_sdl_options *o, mdrt_game_fn game, void *user)
{
    sdl *s = calloc(1, sizeof *s);
    if (!s) return 1;
    s->o = o; s->cfg = cfg; s->game = game; s->user = user;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    int scale = o->scale > 0 ? o->scale : 3;
    s->win = SDL_CreateWindow(o->title ? o->title : "pulseman", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              (o->aspect ? 299 : 320) * scale, 224 * scale, SDL_WINDOW_RESIZABLE);
    s->ren = s->win ? SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (s->win && !s->ren) s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_SOFTWARE);   /* no GPU */
    s->tex = s->ren ? SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240) : NULL;
    if (!s->tex) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_AudioSpec want = { 0 }, got;
    want.freq = cfg->audio_rate ? (int)cfg->audio_rate : 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    s->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
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
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) SDL_AtomicSet(&s->quit, 1);
                else if (k == SDLK_F5) SDL_AtomicAdd(&s->saves, 1);
                else if (k == SDLK_F9) SDL_AtomicAdd(&s->loads, 1);
                else if (k == SDLK_p) SDL_AtomicSet(&s->paused, !SDL_AtomicGet(&s->paused));
                else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))
                    SDL_SetWindowFullscreen(s->win, (SDL_GetWindowFlags(s->win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                for (int p = 0; p < 2; p++) if (!s->pads[p]) { s->pads[p] = SDL_GameControllerOpen(e.cdevice.which); break; }
            }
        } while (SDL_PollEvent(&e));
        read_buttons(s);
        draw(s);
        if (SDL_AtomicGet(&s->quit)) {                    /* give the game a moment to stop */
            for (int i = 0; i < 200 && !SDL_AtomicGet(&s->done); i++) SDL_Delay(10);
            break;
        }
    }
    if (SDL_AtomicGet(&s->done)) SDL_WaitThread(t, NULL);
    else SDL_DetachThread(t);                             /* (it would not stop: the process ends it) */
    if (s->dev) SDL_CloseAudioDevice(s->dev);
    int stopped = SDL_AtomicGet(&s->done);
    if (stopped) { mdrt_free(s->m); SDL_DestroyMutex(s->lock); free(s); }   /* (else the thread still has them) */
    SDL_Quit();
    return 0;
}
