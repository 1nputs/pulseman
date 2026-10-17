/* The native platform (SDL2): a window, sound, the keyboard and game controllers, save RAM in a
 * file, and a settings file the player can edit. The client's game runs on its own thread (it
 * blocks in mdrt_wait_vblank); the calling thread shows the pictures and handles the window.
 * Frames are paced by the sound: the next frame goes when the sound queued drops under a few
 * frames' worth, so the game runs at the console's rate (59.92 Hz NTSC) and the sound never runs
 * dry or piles up.
 *
 * The menu (Esc, or a controller's Back or Guide button; the game waits while it is open): the
 * game's ROM (a file browser; the client checks each one), a reset, states saved and loaded in
 * eight slots (states/slotN.state, each with its picture, slotN.png, shown beside the list), the
 * video (a pixel-art upscaler for the game's frames: mdrt/upscale.h; scaling, aspect, the
 * looks: mdrt/look.h; full screen), captures (the replay and recordings as GIFs: mdrt/capture.h),
 * the sound (volume, mute, the FM core and chip, changed as the game plays), the players (two pads, or EA's 4 Way Play for four, from the next power on;
 * each player's controller; the controllers' buttons, turbo A / B / C, the stick's dead zone),
 * the client's own options (a page it describes: mdrt_sdl_option), the platform's options (pausing
 * when the window loses focus, fast-forward's speed) and every key, each changed
 * by pressing the new one (a controller's button, on a controller). Arrows and Enter work it, or a controller's d-pad, A
 * and B. It is drawn over the picture at the console's resolution (mdrt/menu.h).
 *
 * Keys (the defaults; every one can be changed in the menu or the settings file, by SDL's key
 * name: A, F1, Up, Return, Space, Left Shift, Keypad 8, the sign of , . - = ...):
 *   pad 1  arrows, Z / X / C = A / B / C, Enter = Start
 *   pad 2  I J K L, Q / W / E = A / B / C, Tab = Start
 *   pads 3 and 4, and turbo A / B / C on each pad: no keys until given some
 *   F1 mute, F2 / F3 volume down / up, F4 held to step back (mdrt_requests); F5 pause, F6 held
 *   fast-forward, F7 slow motion, F8 one frame while paused; F9 none; F10 saves the replay (the
 *   last 20 seconds, kept as they are played) as a GIF, F11 starts and stops recording one, F12
 *   a screenshot; Esc the menu; Alt+Enter full screen. States are saved and loaded from the menu.
 * Saves and loads reach the client as MDRT_REQUEST_SAVE / LOAD with the slot's file
 * (mdrt_request_path); a save is written to slotN.state.new and kept only when the client writes it
 * (a game that takes states only in play leaves the slot as it was). Quick resume (on by default):
 * the window closing asks for a state into states/resume.state, which the next start loads; when
 * none is written (out of play) the old one goes, and the next start is a fresh one.
 * Controllers keep the slot they were given when plugged in; by default controller N is player N's
 * (X / A / B = A / B / C, Start, the left shoulder X, Y Y, the right shoulder Z; the d-pad and the
 * left stick are the directions). A change shows for a moment in the window's title.
 *
 * The settings file (settings.ini in the directory the game runs in, beside its saves and
 * screenshots) is written with every setting the first time and whenever one changes; edit it
 * while the game is closed. It keeps the ROM chosen last, the window's size, place and full
 * screen, the upscaler, the scaling (sharp, integer, fit) and aspect (square, 4:3), the volume and mute, the
 * sound device and how many frames of sound are queued (latency), the FM core and chip, the
 * adaptor in the ports, each player's controller, the controllers' buttons, turbo's speed, the
 * stick's dead zone, pausing when the window loses focus, fast-forward's speed, the states'
 * directory, the slot chosen last, quick resume, the screenshots' directory, the looks, the
 * replay's length, a GIF's size, the videos' directory and every key. */
#ifndef MDRT_PLATFORM_SDL2_H
#define MDRT_PLATFORM_SDL2_H
#include "mdrt/mdrt.h"

/* one of the client's own options (a game's rules, say), on its page of the menu: a choice of
 * some named values, or a number; kept in the settings file under its key */
typedef struct mdrt_sdl_option {
    const char *key;            /* its setting ("rules.period_lengths") */
    const char *label;          /* on the page */
    int count;                  /* choices (names and labels, count of each); 0: a number */
    const char *const *names;   /* the choices as the settings file has them */
    const char *const *labels;  /* and as the page shows them (NULL: the names) */
    int min, max, step;         /* a number's range and step */
    int def;                    /* the default: the choice's index, or the number */
} mdrt_sdl_option;

typedef struct mdrt_sdl_options {
    const char *title;
    int scale;                  /* window size in multiples of the picture (0: the settings', or 3) */
    int aspect;                 /* 1: 4:3 for this run (0: the settings' aspect) */
    const char *sram_path;      /* the save RAM's file (NULL: not kept) */
    const char *app;            /* the name screenshots begin with (NULL: "screenshot") */
    const char *settings_path;  /* the settings file (NULL: settings.ini in the working directory) */
    /* the game's ROM, for a client that runs one (rom_check NULL: none). rom_check says whether
     * bytes are a ROM the game can run (NULL) or why not, in a few words for the player; rom_use
     * gives the client the ROM before its game starts (the bytes are the platform's, kept until
     * the next rom_use). The ROM is rom_path (the command line's), else the one the player chose
     * last, else rom_default; when none will do, the menu opens on its ROM page, with rom_hint
     * (which ROM the game wants, for the player) or why the one given or chosen would not do. */
    const char *rom_path, *rom_default, *rom_hint;
    const char *(*rom_check)(const uint8_t *rom, size_t size, void *user);
    void (*rom_use)(const uint8_t *rom, size_t size, void *user);
    /* the client's own options (noptions of them; none: no page): a page of the menu called
     * options_title, with options_note under it. The client gets their values (each option's
     * choice index or number, in order) through options_use before every power on, on the
     * window's thread while the game is stopped, so they hold for the whole of a game; a change
     * made while one is played shows from the next (the menu says so). options_changed (may be
     * NULL) hears of each change on the page and may change the other values (a preset); nonzero
     * when it did. */
    const char *options_title, *options_note;
    const mdrt_sdl_option *options;
    int noptions;
    void (*options_use)(const int *values, void *user);
    int (*options_changed)(int which, int *values, void *user);
} mdrt_sdl_options;

/* the client's game: runs on the game thread with the console made from the configuration; when
 * mdrt_wait_vblank returns nonzero (the window is closing, or the game starting again) it should
 * return. It is called again on a new console after the menu's reset or a new ROM, and must then
 * start from the beginning (power on). */
typedef void (*mdrt_game_fn)(mdrt *m, void *user);

/* open the window and sound, run the game until it returns or the window closes; 0 on success.
 * cfg->platform is filled in here, and cfg->fm_core and fm_model when cfg->fm_core is NULL (the
 * settings', the player's choice). The user pointer goes to game, rom_check and rom_use. */
int mdrt_sdl_run(mdrt_config *cfg, const mdrt_sdl_options *o, mdrt_game_fn game, void *user);

/* Windows: SDL2's compile flags (pkg-config --cflags sdl2) rename the client's main to SDL_main,
 * and SDL2main supplies the real entry point; this gives it its prototype there */
#ifdef main
int main(int argc, char *argv[]);
#endif

#endif
