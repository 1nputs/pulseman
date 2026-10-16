/* The native platform (SDL2): a window, sound, the keyboard and game controllers, save RAM in a
 * file, and a settings file the player can edit. The client's game runs on its own thread (it
 * blocks in mdrt_wait_vblank); the calling thread shows the pictures and handles the window.
 * Frames are paced by the sound: the next frame goes when the sound queued drops under a few
 * frames' worth, so the game runs at the console's rate (59.92 Hz NTSC) and the sound never runs
 * dry or piles up.
 *
 * Keys (the defaults; every one can be changed in the settings file, by SDL's key name: A, F1, Up,
 * Return, Space, Left Shift, Keypad 8, the sign of , . - = ...):
 *   pad 1  arrows, Z / X / C = A / B / C, Enter = Start
 *   pad 2  I J K L, Q / W / E = A / B / C, Tab = Start
 *   F1 mute, F2 / F3 volume down / up, F4 held to step back (mdrt_requests), F5 slow motion,
 *   F6 held fast-forward, F7 / F8 ask the game to save / load a state, F9 one frame while paused,
 *   F10 pause, F11 or Alt+Enter full screen, F12 a screenshot, Esc quit
 * The first two game controllers are pads 1 and 2 (X / A / B = A / B / C, Start). A change shows
 * for a moment in the window's title.
 *
 * The settings file (settings.ini in the directory the game runs in, beside its saves and
 * screenshots) is written with every setting the first time and whenever one changes; edit it
 * while the game is closed. It keeps the window's size, place and full screen, the scaling
 * (sharp, integer, fit) and aspect (square, 4:3), the volume and mute, the sound device and how
 * many frames of sound are queued (latency), pausing when the window loses focus, fast-forward's
 * speed, the screenshots' directory and every key. */
#ifndef MDRT_PLATFORM_SDL2_H
#define MDRT_PLATFORM_SDL2_H
#include "mdrt/mdrt.h"

typedef struct mdrt_sdl_options {
    const char *title;
    int scale;                  /* window size in multiples of the picture (0: the settings', or 3) */
    int aspect;                 /* 1: 4:3 for this run (0: the settings' aspect) */
    const char *sram_path;      /* the save RAM's file (NULL: not kept) */
    const char *app;            /* the name screenshots begin with (NULL: "screenshot") */
    const char *settings_path;  /* the settings file (NULL: settings.ini in the working directory) */
} mdrt_sdl_options;

/* the client's game: runs on the game thread with the console made from the configuration; when
 * mdrt_wait_vblank returns nonzero (the window is closing) it should return */
typedef void (*mdrt_game_fn)(mdrt *m, void *user);

/* open the window and sound, run the game until it returns or the window closes; 0 on success.
 * cfg->platform is filled in here. */
int mdrt_sdl_run(mdrt_config *cfg, const mdrt_sdl_options *o, mdrt_game_fn game, void *user);

/* Windows: SDL2's compile flags (pkg-config --cflags sdl2) rename the client's main to SDL_main,
 * and SDL2main supplies the real entry point; this gives it its prototype there */
#ifdef main
int main(int argc, char *argv[]);
#endif

#endif
