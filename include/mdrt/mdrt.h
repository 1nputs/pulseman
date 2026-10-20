/* pulseman: the composition of the hardware modules and the timing constants.
 * See docs/API.md. */
#ifndef MDRT_MDRT_H
#define MDRT_MDRT_H
#include <stdint.h>

/* NTSC clocks (Sega Genesis Software Manual; the YM2612 divides the 68k clock by 144). A PAL
 * console's master clock is slower and its frame has more lines; the dividers are the same. */
#define MDRT_MASTER_CLOCK_NTSC 53693175u
#define MDRT_MASTER_CLOCK_PAL  53203424u
#define MDRT_M68K_CLOCK_NTSC   (MDRT_MASTER_CLOCK_NTSC / 7)    /* 7670453 Hz */
#define MDRT_Z80_CLOCK_NTSC    (MDRT_MASTER_CLOCK_NTSC / 15)   /* 3579545 Hz, also the PSG */
#define MDRT_FM_RATE_NTSC      (MDRT_M68K_CLOCK_NTSC / 144)    /* 53267 Hz */
#define MDRT_LINES_NTSC        262
#define MDRT_LINES_PAL         313
#define MDRT_LINE_MCLKS        3420

#include <stddef.h>
#include "mdrt/fm.h"
#include "mdrt/mix.h"
#include "mdrt/pad.h"
#include "mdrt/psg.h"
#include "mdrt/sram.h"
#include "mdrt/vdp.h"

/* ---- the composition: the console the client's game runs on ------------------------------
 * The game calls the modules' bus functions directly (mdrt_vdp_ctrl16 (mdrt_get_vdp (m), ...),
 * mdrt_pad_read, mdrt_sram_write8 ...) and mdrt_wait_vblank once a frame. Sound goes in with a
 * time: master clocks since the frame began (the frame is MDRT_LINES x MDRT_LINE_MCLKS long and
 * begins at the handshake, in the vertical blank); the frame's audio is made at the next handshake,
 * every write at its time. The client's own sound processor (a Z80 program ported to C) runs
 * through the sound tick while the audio is made, and may also run ahead between handshakes (when
 * the game waits on it). Nothing here blocks or reads a clock: the platform does, in its wait. */
typedef struct mdrt mdrt;

/* the platform's side of the handshake, called on the game's thread from mdrt_wait_vblank */
typedef struct mdrt_platform {
    void *user;
    /* the picture: VDP colours (0000BBB0GGG0RRR0; mdrt_vdp_rgb makes RGB) */
    void (*present)(void *user, const uint16_t *pixels, int width, int height, int pitch);
    /* the frame's sound: stereo pairs at the configured rate */
    void (*audio)(void *user, const int16_t *samples, int frames);
    /* block until the next frame may start (NULL: go on at once); nonzero: the game should stop */
    int (*wait)(void *user);
    /* the buttons held on pads 1-4 (MDRT_PAD_*; 3 and 4 only through an adaptor:
     * mdrt_pads_set_adaptor) for the frame starting (NULL: as they are) */
    void (*input)(void *user, uint16_t buttons[MDRT_PADS]);
    /* the player's requests to the client (MDRT_REQUEST_*): SAVE and LOAD once for each press since
     * the last call, REWIND while it is held (NULL: none) */
    unsigned (*requests)(void *user);
    /* the file a SAVE or LOAD request names (NULL, or no callback: the client's own choice); kept
     * by the platform until the next handshake */
    const char *(*request_path)(void *user);
    /* the client waits on something of its own between handshakes (netplay's other player):
     * called every millisecond or so while it does; nonzero: the platform is closing, and the game
     * should stop (NULL: never) */
    int (*idle)(void *user);
} mdrt_platform;

/* requests from the person playing, which the client carries out where it can (a state saved at
 * a point the game can be resumed from, loaded, or stepped back through) */
enum { MDRT_REQUEST_SAVE = 1, MDRT_REQUEST_LOAD = 2, MDRT_REQUEST_REWIND = 4 };

typedef struct mdrt_config {
    int pal;                    /* a PAL console: its master clock, 313 lines, the version bit */
    int domestic;               /* a Japanese console: the version register's overseas bit clear */
    const char *fm_core;        /* the FM core, "nuked" or "ymfm" (NULL: the build's default; mdrt_fm_cores) */
    int fm_model;               /* MDRT_FM_YM2612 (0: the first consoles) or MDRT_FM_YM3438 (later ones) */
    uint32_t audio_rate;        /* output samples a second (0: 48000) */
    int32_t fm_gain, psg_gain;  /* the mixer's gains in 1/32768ths (0: MDRT_MIX_FM_GAIN / PSG_GAIN) */
    size_t sram_size;           /* save RAM bytes (0: none) */
    mdrt_read8_fn read68k;      /* the 68k's memory, for DMA */
    void *read68k_user;
    mdrt_platform platform;
} mdrt_config;

/* NULL when out of memory or the build has not the FM core asked for */
mdrt *mdrt_new(const mdrt_config *c);
void mdrt_free(mdrt *m);
/* the console powered on again in place, between frames: its VDP, sound chips, the pads' registers
 * and the clock within the frame as mdrt_new has them, and the client's code to start again from
 * its power on. Kept: the save RAM (the cartridge's battery), the devices in the ports (the pads'
 * types, the adaptor), the frame count (mdrt_frame_count goes on: netplay's frames are the
 * console's), and all the client attached (state parts, bus maps, hooks, sources, the session,
 * the traces). Netplay does it when the host resets the console (MDRT_NET_RESET). */
void mdrt_power_on(mdrt *m);
mdrt_vdp *mdrt_get_vdp(mdrt *m);
mdrt_pads *mdrt_get_pads(mdrt *m);
mdrt_sram *mdrt_get_sram(mdrt *m);
mdrt_fm *mdrt_get_fm(mdrt *m);                  /* (its core and model: mdrt_fm_core_of, mdrt_fm_model) */
/* another FM chip in place of the console's, between frames (on the game's thread: in the
 * platform's wait, or by the client between handshakes): the core named (NULL: the default) and the
 * model, given the registers the old chip had. Notes sounding stop until their next key-on. 0, or
 * -1 when the build has no such core (nothing changed). The player's choice in the SDL2 menu. */
int mdrt_set_fm(mdrt *m, const char *core, int model);

/* the master clock in Hz, the frame's length in master clocks (896040 NTSC, 1070460 PAL) and
 * frames completed */
uint32_t mdrt_master_clock(const mdrt *m);
uint32_t mdrt_frame_mclks(const mdrt *m);
uint64_t mdrt_frame_count(const mdrt *m);

/* sound at master clock `at` of the frame (past its end: in the next frames): a YM2612 port
 * write (0 / 1 part I's address / data, 2 / 3 part II's), the YM2612's reset, a PSG byte */
void mdrt_ym_write(mdrt *m, uint32_t at, int port, uint8_t value);
void mdrt_ym_reset(mdrt *m, uint32_t at);
void mdrt_psg_port(mdrt *m, uint32_t at, uint8_t value);
/* the client's sound processor: run it up to master clock `until` of the frame (its writes then
 * come in through mdrt_ym_write) */
typedef void (*mdrt_sound_tick_fn)(void *user, uint32_t until);
void mdrt_set_sound_tick(mdrt *m, mdrt_sound_tick_fn fn, void *user);

/* ---- the 68k's bus by address --------------------------------------------------------------
 * For what has no function of its own (a cartridge's EEPROM, a bank-switching mapper, a
 * coprocessor) and for tools that work by address (a RAM watch, cheats, a script): the client
 * maps a region to its own functions, and mdrt_bus_* reach the map, then the console's own ports:
 * the VDP and PSG ($C00000-$C0001F, by the access's width, as the 68k's move.b / .w / .l), the I/O
 * chip ($A10000-$A1001F), save RAM ($200000 up when it is configured), and the configuration's
 * read68k for the rest (the client's ROM and work RAM; writes there go nowhere unless mapped).
 * DMA from the 68k bus reads through the map too. A client that calls the modules directly, as a
 * port of 68k code usually does, need not use it. Regions mapped later win where they overlap;
 * at most 16. Returns 0, or -1 when full. */
typedef uint8_t (*mdrt_bus_read_fn)(void *user, uint32_t addr);
typedef void (*mdrt_bus_write_fn)(void *user, uint32_t addr, uint8_t value);
int mdrt_bus_map(mdrt *m, uint32_t base, uint32_t size, mdrt_bus_read_fn read, mdrt_bus_write_fn write, void *user);
uint8_t mdrt_bus_read8(mdrt *m, uint32_t addr);
uint16_t mdrt_bus_read16(mdrt *m, uint32_t addr);
uint32_t mdrt_bus_read32(mdrt *m, uint32_t addr);
void mdrt_bus_write8(mdrt *m, uint32_t addr, uint8_t value);
void mdrt_bus_write16(mdrt *m, uint32_t addr, uint16_t value);
void mdrt_bus_write32(mdrt *m, uint32_t addr, uint32_t value);

/* ---- the runtime as a recorder -------------------------------------------------------------
 * The console writes the trace formats the oracle's exporter writes (docs/VECTORS.md) to the
 * client's sink, from now on: MDRT_TRACE_VDP a .vdptrace (version 2: every VDP port access with its
 * time, the DMA sources, the state and picture at each handshake once a whole frame is in it),
 * MDRT_TRACE_PADS a .padtrace (version 2: the adaptor, the I/O chip's accesses, the four pads'
 * buttons each frame), MDRT_TRACE_SOUND a
 * .sndtrace (every sound write at its master clock as the chips take it, and the console's own
 * audio; flags bit 2 says so, bit 1 that the FM core was Nuked-OPN2, bit 0 that the recording began
 * at the first frame). pulseman's tests replay them as they replay the oracle's, so a run of a
 * release is a test of the next. A NULL sink ends the trace (its END tag) and stops. */
enum { MDRT_TRACE_VDP, MDRT_TRACE_PADS, MDRT_TRACE_SOUND };
typedef void (*mdrt_sink_fn)(void *user, const void *bytes, size_t n);
void mdrt_record(mdrt *m, int trace, mdrt_sink_fn sink, void *user);

/* lockstep: what the game reads that depends on time or the player, from recorded values */
void mdrt_set_hv_source(mdrt *m, mdrt_hv_fn fn, void *user);
typedef void (*mdrt_pad_fn)(void *user, uint16_t buttons[MDRT_PADS]);
void mdrt_set_pad_source(mdrt *m, mdrt_pad_fn fn, void *user);

/* ---- time inside a frame -----------------------------------------------------------------
 * The composition keeps a position in the frame, in master clocks since it began (at the
 * handshake, in the vertical blank), that only the client moves. The picture is drawn a line at a
 * time as the position passes each line's start (mdrt_vdp_line_start), so the client's VDP writes
 * land on the lines after the beam: a write made after mdrt_wait_line(m, 100) changes lines 101 on.
 * A client that never moves it writes everything in the vertical blank and gets the frame drawn
 * whole at the handshake, as before. The H-interrupt handler (mdrt_set_hint) is called as the
 * position passes the lines the H-interrupt counter (register 10) picks, whether or not the client
 * moves the clock: with no moves, the handshake draws the frame and calls it on the way.
 *
 * mdrt_now: the position. mdrt_run_until: move it on to mclk (not back; at most the frame's end).
 * mdrt_charge: move it on by cycles of the 68k (7 master clocks each), for code that wrote the VDP
 * while the beam ran; nonzero when the frame's end is reached (the vertical blank is due: the
 * client takes it, mdrt_wait_vblank and its vblank code; the clock waits there until it does).
 * mdrt_wait_line: move it on to the start of line n (0-261 NTSC, 0-312 PAL, as the V counter
 * counts them before its jump), as the 68k's loops on the HV counter did; nonzero when line n has
 * gone by in this frame (the 68k would have waited through the vertical blank for it). */
uint32_t mdrt_now(const mdrt *m);
void mdrt_run_until(mdrt *m, uint32_t mclk);
int mdrt_charge(mdrt *m, uint32_t cycles);
int mdrt_wait_line(mdrt *m, int line);
/* the client's H-interrupt code (its handler, ported): called at the start of the line it comes on,
 * with mdrt_now there, when register 0 enables it (the counter as the oracle runs it: reloaded
 * from register 10 on the last line of the vertical blank and when it runs out, counted down on
 * each line of the display) */
typedef void (*mdrt_hint_fn)(void *user);
void mdrt_set_hint(mdrt *m, mdrt_hint_fn fn, void *user);

/* the handshake: draw the picture, make the frame's sound, hand both to the platform, wait for
 * its go, read the pads. On return the next frame has begun with the VDP in its vertical blank
 * (status bit 3) until mdrt_vblank_end, which the client calls when its vblank code is done.
 * Nonzero: the platform is closing and the game should stop. */
int mdrt_wait_vblank(mdrt *m);
void mdrt_vblank_end(mdrt *m);
/* the requests the last handshake brought (MDRT_REQUEST_*), and the file a SAVE or LOAD among them
 * names (NULL: the client's own); the SDL2 platform's are its save slots and the Autosave */
unsigned mdrt_requests(const mdrt *m);
const char *mdrt_request_path(const mdrt *m);

/* ---- the whole console's state, as a value ------------------------------------------------
 * Everything that decides what the game will see and hear from here on: the VDP (ports and
 * caches too), the PSG, the FM chip (its core's own state: a state loads only into a build with
 * the same FM core), the mixer, the sound written for times still to come, the I/O chip and the
 * pads, save RAM, the frame count and the HV beam, and the parts the client adds (its work RAM,
 * its sound processor, whatever it keeps outside them). The bytes are the same on every host for
 * a given build. Take a state between handshakes, at a point the client can resume from (the
 * client's position in its own code is not in it: see docs/API.md, "States").
 *
 * mdrt_get_state writes the state into buf and returns its size, or only the size when buf is
 * NULL or too small (size it with mdrt_get_state(m, NULL, 0)); mdrt_set_state loads one and
 * returns 0, or -1 when the bytes are not a state of this console (another version, model, FM
 * core, part list or size): then nothing has changed. A state with another audio rate loads,
 * with the mixer started afresh (it is presentation only). */
size_t mdrt_get_state(mdrt *m, void *buf, size_t size);
int mdrt_set_state(mdrt *m, const void *buf, size_t size);
/* a part of the client's in every state, under a tag of four characters: save writes it and
 * returns its size (out NULL: only the size, which may differ from one state to the next); load
 * reads it back and returns 0, or -1 when it cannot, and then the whole state is refused and
 * the console put back as it was. Returns 0, or -1 when the tag is taken or there are already
 * 16 parts. */
typedef size_t (*mdrt_part_save_fn)(void *user, uint8_t *out);
typedef int (*mdrt_part_load_fn)(void *user, const uint8_t *in, size_t size);
int mdrt_add_state_part(mdrt *m, const char *tag, mdrt_part_save_fn save, mdrt_part_load_fn load, void *user);
/* the same for a block of memory of a fixed size */
int mdrt_add_state_memory(mdrt *m, const char *tag, void *data, size_t size);

/* ---- frames played again, and the simulation's hash (rollback netplay: mdrt/netplay.h) ------
 * Quiet frames: mdrt_set_quiet(m, n) makes the next n handshakes quiet. The frames they end are not
 * drawn (from now: the lines are counted, the H-interrupts come and the HV counter reads as ever)
 * and the platform never sees them: no present, audio, wait or requests. Their sound is made as
 * ever (the client's sound tick runs, the chips are clocked), so the chips go on in step; only its
 * samples are dropped. Drawing starts again with the frame the n-th quiet handshake begins. A
 * rollback loads the state of frame s while at frame f, then mdrt_set_quiet(m, f - s): the frames
 * played again are quiet and frame f is drawn whole. 0 ends them now. mdrt_quiet: the quiet
 * handshakes still to come. Quiet is not part of a state. */
void mdrt_set_quiet(mdrt *m, int n);
int mdrt_quiet(const mdrt *m);
/* the platform's input now (the buttons its players hold), whether or not a pad source is set;
 * zeros when the platform has no input */
void mdrt_platform_input(mdrt *m, uint16_t buttons[MDRT_PADS]);
/* the client is waiting (mdrt_platform.idle): nonzero when the platform is closing */
int mdrt_idle(mdrt *m);
/* the netplay session this console plays in (mdrt/netplay.h), kept here for the client to find
 * whoever made it (the client from its command line, or the platform from its menu); NULL: none */
struct mdrt_net;
void mdrt_set_net(mdrt *m, struct mdrt_net *n);
struct mdrt_net *mdrt_get_net(const mdrt *m);
/* A hash of what the game can see, to compare two consoles (peers checking for a desync, tests):
 * the clock (the frame count, the position, the lines passed, the H-interrupt counter, the HV
 * counter's nudge), the VDP, the pads, save RAM, and the client's parts; not what only the player
 * sees and hears: the picture, the PSG, the FM chip, the mixer and the sound still to come. A part
 * is in it as its kind says: the client's are MDRT_PART_SIMULATION when added, the console's PSG,
 * FM, MIX and SND sections MDRT_PART_PRESENTATION (a game that reads the FM chip's status marks
 * "FM  " simulation). mdrt_set_state_part_kind: 0, or -1 when there is no such part (CONS's kind is
 * fixed). The same on every host for a given build when the client's parts are. */
enum { MDRT_PART_SIMULATION, MDRT_PART_PRESENTATION };
int mdrt_set_state_part_kind(mdrt *m, const char *tag, int kind);
uint64_t mdrt_sim_hash(mdrt *m);

#endif
