/* md-runtime: the composition of the hardware modules and the timing constants.
 * See docs/WORKPLAN.md. */
#ifndef MDRT_MDRT_H
#define MDRT_MDRT_H
#include <stdint.h>

/* NTSC clocks (Sega Genesis Software Manual; the YM2612 divides the 68k clock by 144) */
#define MDRT_MASTER_CLOCK_NTSC 53693175u
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
    /* the buttons held on pads 1 and 2 (MDRT_PAD_*) for the frame starting (NULL: as they are) */
    void (*input)(void *user, uint16_t buttons[2]);
} mdrt_platform;

typedef struct mdrt_config {
    int pal;
    uint32_t audio_rate;        /* output samples a second (0: 48000) */
    size_t sram_size;           /* save RAM bytes (0: none) */
    mdrt_read8_fn read68k;      /* the 68k's memory, for DMA */
    void *read68k_user;
    mdrt_platform platform;
} mdrt_config;

mdrt *mdrt_new(const mdrt_config *c);
void mdrt_free(mdrt *m);
mdrt_vdp *mdrt_get_vdp(mdrt *m);
mdrt_pads *mdrt_get_pads(mdrt *m);
mdrt_sram *mdrt_get_sram(mdrt *m);

/* the frame's length in master clocks (896040 NTSC, 1070460 PAL) and frames completed */
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

/* lockstep: what the game reads that depends on time or the player, from recorded values */
void mdrt_set_hv_source(mdrt *m, mdrt_hv_fn fn, void *user);
typedef void (*mdrt_pad_fn)(void *user, uint16_t buttons[2]);
void mdrt_set_pad_source(mdrt *m, mdrt_pad_fn fn, void *user);

/* the handshake: draw the picture, make the frame's sound, hand both to the platform, wait for
 * its go, read the pads. On return the next frame has begun with the VDP in its vertical blank
 * (status bit 3) until mdrt_vblank_end, which the client calls when its vblank code is done.
 * Nonzero: the platform is closing and the game should stop. */
int mdrt_wait_vblank(mdrt *m);
void mdrt_vblank_end(mdrt *m);

#endif
