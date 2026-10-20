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
 * looks: mdrt/look.h; full screen; vsync, the frames shown in step with the screen, the game
 * still paced by its sound), captures (the replay and recordings as GIFs: mdrt/capture.h),
 * the sound (volume, mute, the FM core and chip, changed as the game plays), the players (two pads, or EA's 4 Way Play for four, from the next power on;
 * each player's controller; the controllers' buttons, turbo A / B / C, the stick's dead zone),
 * the client's own options (a page it describes: mdrt_sdl_option), the platform's options (pausing
 * when the window loses focus, fast-forward's speed, quick resume, frames a second shown at the
 * picture's top right: the frames the game makes, over each half second) and every key, each changed
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
 * screenshots: a macOS bundle's from the Finder, mdrt_sdl_home's) is written with every setting the first time and whenever one changes; edit it
 * while the game is closed. It keeps the ROM chosen last, the window's size, place and full
 * screen, the upscaler, the scaling (sharp, integer, fit) and aspect (square, 4:3), the volume and mute, the
 * sound device and how many frames of sound are queued (latency), the FM core and chip, the
 * adaptor in the ports, each player's controller, the controllers' buttons, turbo's speed, the
 * stick's dead zone, pausing when the window loses focus, fast-forward's speed, frames a second, the states'
 * directory, the slot chosen last, quick resume, the screenshots' directory, the looks, the
 * replay's length, a GIF's size, the videos' directory and every key. */
#ifndef MDRT_PLATFORM_SDL2_H
#define MDRT_PLATFORM_SDL2_H
#include "mdrt/mdrt.h"
#include "mdrt/netplay.h"

/* how a choice can also be found on disk (mdrt_sdl_option.browse): a line under it, label's,
 * with options_detail's text at its right, opens the menu's file browser in dir (from the working
 * directory; missing: there), titled title, listing folders and the files whose names end in one
 * of files' endings (".zip .7z"; NULL or "": none), and "Use this folder" when folders. What is
 * chosen goes to options_pick, which makes it a choice */
typedef struct mdrt_sdl_browse {
    const char *title, *label, *dir, *files;
    int folders;
} mdrt_sdl_browse;

/* one of the client's own options (a game's rules, say), on its page of the menu: a choice of
 * some named values, or a number; kept in the settings file under its key.
 *
 * A choice can stand for numbers (numbers not NULL): the client gets the number of the one
 * chosen, and the settings file keeps a choice's name or any number from min to max. A number
 * that is none of the choices' (from the file, or set by options_changed) is one more choice on
 * the page, after them, shown as "custom" with the number and its unit. */
typedef struct mdrt_sdl_option {
    const char *key;            /* its setting ("rules.period_lengths") */
    const char *label;          /* on the page */
    int count;                  /* choices (names and labels, count of each); 0: a number */
    const char *const *names;   /* the choices as the settings file has them */
    const char *const *labels;  /* and as the page shows them (NULL: the names) */
    int min, max, step;         /* a number's range and step (a choice's numbers: the file's range) */
    int def;                    /* the default: the choice's index, or the number */
    const int *numbers;         /* the number each choice stands for (count of them; NULL: none) */
    const char *unit;           /* after a number on the page ("%", " min"; NULL: none) */
    const mdrt_sdl_browse *browse;   /* a choice that can also be found on disk (NULL: not) */
} mdrt_sdl_option;

/* a page of the client's options, an item of the menu of its own: options first to first + count
 * - 1, with the note under its title (NULL: none) */
typedef struct mdrt_sdl_page {
    const char *title, *note;
    int first, count;
} mdrt_sdl_page;

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
     * NULL) hears of each change on the page (which: the option changed), and of the values as
     * the settings file had them (which: -1), and may change the other values (a preset that
     * agrees with them); nonzero when it did. */
    const char *options_title, *options_note;
    const mdrt_sdl_option *options;
    int noptions;
    void (*options_use)(const int *values, void *user);
    int (*options_changed)(int which, int *values, void *user);
    /* the options on pages of their own (npages of them; none: the one page, options_title's) */
    const mdrt_sdl_page *pages;
    int npages;
    /* lines under a page's options (may be NULL): the client's text for page p (0 when there are
     * no pages) with the values now, \n between lines, a tab in one putting what follows it at
     * the right; asked when the page is shown and after each change on it, on the window's thread.
     * In sections (a \f before each section, its first line its title; 16 at most): the lines
     * before the first, then a line ("2 of 6", the title) that LEFT / RIGHT step through the
     * sections with, then the lines of the one shown */
    void (*options_text)(int page, const int *values, char *text, size_t size, void *user);
    /* a browsed option's (mdrt_sdl_browse): path is what the player chose there (a folder
     * or a file, from the working directory when inside it), or what the settings file names
     * when it is none of the choices: the client makes it one (growing the option's names,
     * labels and count, which are its own) and returns its index, or -1 with why (the menu says
     * it); options_detail writes the text the browse line shows for the choice value (NULL: its
     * name) */
    int (*options_pick)(int which, const char *path, char *why, size_t why_size, void *user);
    void (*options_detail)(int which, int value, char *text, size_t size, void *user);
    /* the save RAM's file for the game about to start, asked after options_use (so it can follow
     * the client's options: another save for another set of its data); NULL, or a NULL return: sram_path */
    const char *(*sram_file)(void *user);
    /* netplay (mdrt/netplay.h). net_identity: the client's identity for a session (what must match
     * between the two: its build, the ROM, settings), which puts the Online page in the menu: the
     * player picks host, join or both connecting (two machines), the address, port and input
     * delay, and for a game of more than two players (net_pads) the players at this machine and a
     * host's players in all (kept in the settings file), and starts a session there; NULL: no netplay. net: a session the client made
     * from its command line instead (mdrt_net_parse, mdrt_net_open_udp, mdrt_net_new; its
     * local_input reading mdrt_platform_input), and net_udp its UDP link (NULL: another link).
     * Either way the window connects first, showing it (Esc gives up), then powers on a new console
     * with the session started on it (mdrt_set_net, mdrt_net_start: the console's pad source); the
     * game's function finds it with mdrt_get_net and calls mdrt_net_frame at its waits, and
     * mdrt_idle while told to wait (the window waits a millisecond there, and says when it is
     * closing). The window's own sessions take each player's buttons from the window's players in
     * turn (pad 1's controls for the first pad the peer plays). While the session lasts the window paces the frames by
     * its clock, never making up the time the game spent waiting on the other (in mdrt_idle: so a
     * frame given back to keep in step is given), and
     * stretches the sound a little to keep its queue; the menu does not stop the game; pause, the
     * frame step, slow motion, fast-forward, rewind, the save slots, quick resume, reset (but the
     * host's, for a game that gives net_reset), another
     * ROM, the FM core and chip, the Players page's adaptor (the session's pads say) and pausing when the window loses
     * focus are not to be had; save RAM is not read from the player's file, nor ever written back to it
     * by that console (after the session too, while it plays on alone); the title shows the round trip, the
     * delay and the rollbacks. When it ends (the other left, the connection lost, a desync) the
     * window says why and the game plays on alone. */
    const char *net_identity;
    mdrt_net *net;
    struct mdrt_udp *net_udp;
    /* the most pads the game plays in the window's sessions: 0 to 2, two (a player at each of two
     * machines); up to MDRT_PADS, the Online page asks each machine its players and a host the
     * players in all, the host waits for them all (up to four machines), and the window puts EA's
     * 4 Way Play in the ports when pads 3 or 4 are played, for a game that can play with it */
    int net_pads;
    /* the session's settings this player sends if it hosts (mdrt_net_config.settings: what the
     * game plays by that is not compared in the identity but sent, such as its options, its data
     * and save RAM), asked when the Online page's Start makes a session, after options_use with the
     * menu's values (and before sram_file is asked again): the bytes and *size, or NULL for none.
     * The session copies them. At power on the game finds the host's with mdrt_net_settings (the
     * host's own on the host) and plays by them on both; NULL: no settings. fresh_save: the Online
     * page's Save RAM is "a new cartridge's" (net_save_ram), so the game sends none of its own. */
    const void *(*net_settings)(size_t *size, int fresh_save, void *user);
    /* the game sends its save RAM in its settings: a host's Online page then has Save RAM, "yours"
     * (the game's own, as its save file has it) or "a new cartridge's" (none: the game as new), kept
     * in the settings file and given to net_settings */
    int net_save_ram;
    /* the game starts again from its power on when its session says MDRT_NET_RESET: the menu's
     * Reset of the host then resets the console for everyone at once (the bit with its buttons:
     * every machine powered on again at the same frame, the session going on), a guest's saying
     * only the host can; 0: no reset in a session */
    int net_reset;
    /* the values the session's game plays by (the host's, which its settings gave the game), for
     * the client's pages while a session lasts, asked on the window's thread each time one is
     * shown: the client fills values (noptions of them, as options_use has them; they come
     * holding the player's own) and returns nonzero, or 0 for none yet (the pages as ever). The
     * pages then show those, greyed: no option there changes until the session ends, and the
     * settings keep the player's own. NULL: the pages as ever in a session too. */
    int (*options_session)(int *values, void *user);
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

/* where the game's files go (the settings, saves, states, screenshots, videos: all relative to the
 * working directory): call it first thing in main, before any of them is opened. A macOS app
 * bundle started from the Finder (or `open`) runs in "/", where nothing can be written, so there
 * it works in ~/Library/Application Support/APP (made if need be) and this returns that
 * directory. Run from a shell (its program, APP.app/Contents/MacOS/...), outside a bundle, and on
 * every other system, the game keeps the working directory it was given, as its command line's
 * paths expect, and this returns NULL. */
const char *mdrt_sdl_home(const char *app);

/* Windows: SDL2's compile flags (pkg-config --cflags sdl2) rename the client's main to SDL_main,
 * and SDL2main supplies the real entry point; this gives it its prototype there */
#ifdef main
int main(int argc, char *argv[]);
#endif

#endif
