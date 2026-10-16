/* The native platform (SDL2): a window, sound, the keyboard and game controllers, save RAM in a
 * file. The client's game runs on its own thread (it blocks in mdrt_wait_vblank); the calling
 * thread shows the pictures and handles the window. Frames are paced by the sound: the next
 * frame goes when the sound queued drops under a few frames' worth, so the game runs at the
 * console's rate (59.92 Hz NTSC) and the sound never runs dry or piles up.
 *
 * Keys: pad 1 arrows, Z / X / C = A / B / C, Enter = Start; pad 2 I J K L, Q / W / E, Tab. The
 * first two game controllers are pads 1 and 2 (X / A / B = A / B / C, Start). Esc or closing the
 * window quits, P pauses, F11 or Alt+Enter toggles full screen. F5 and F9 ask the game to save and
 * load a state, Backspace held to step back (mdrt_requests). */
#ifndef MDRT_PLATFORM_SDL2_H
#define MDRT_PLATFORM_SDL2_H
#include "mdrt/mdrt.h"

typedef struct mdrt_sdl_options {
    const char *title;
    int scale;                  /* window size in multiples of the picture (0: 3) */
    int aspect;                 /* 1: stretch the picture to 4:3 */
    const char *sram_path;      /* the save RAM's file (NULL: not kept) */
} mdrt_sdl_options;

/* the client's game: runs on the game thread with the console made from the configuration; when
 * mdrt_wait_vblank returns nonzero (the window is closing) it should return */
typedef void (*mdrt_game_fn)(mdrt *m, void *user);

/* open the window and sound, run the game until it returns or the window closes; 0 on success.
 * cfg->platform is filled in here. */
int mdrt_sdl_run(mdrt_config *cfg, const mdrt_sdl_options *o, mdrt_game_fn game, void *user);

#endif
