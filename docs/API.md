# Using pulseman from a client

A client is a game reimplemented in C whose hardware accesses were 68k bus accesses. It links
`build/release/libmdrt.a` (and `libmdrt_sdl2.a` for a window), includes `mdrt/mdrt.h` (and
`mdrt/platform_sdl2.h`), and maps its hardware calls onto the runtime. The first client,
NHL '94 (`../ninety-four`), does it in `native/hal_native.c` (about a hundred lines) and
`native/main.c`; read those as the reference.

## Starting

```c
mdrt_config c = { 0 };
c.audio_rate = 48000;                 /* output rate */
c.sram_size = 0x2000;                 /* cartridge save RAM bytes */
c.read68k = read68k;                  /* the byte at a 68k address, for DMA (ROM, work RAM) */
mdrt_sdl_options o = { .title = "Title", .app = "title", .sram_path = "save.srm" };
return mdrt_sdl_run(&c, &o, game, NULL);   /* game(m, user) runs on the game thread */
```

The platform keeps the player's settings in `settings.ini` in the directory the game runs in
(`settings_path` puts it elsewhere; `app` begins the screenshots' names). It writes the file with
every setting the first time and whenever the player changes one: the window's size, place and
full screen, the scaling (sharp, integer, fit) and aspect (square pixels, 4:3), the volume, mute,
sound device and latency, pausing when the window loses focus, fast-forward's speed, the
screenshots' directory and every key of both pads and of the player's controls (on the function
keys by default: `include/mdrt/platform_sdl2.h` lists them). `scale` (a window of N times the
picture) and `aspect` (4:3) set this run's window over the settings'.

Esc opens the platform's menu (a controller's Back or Guide button too), drawn over the picture
at the console's resolution while the game waits in `mdrt_wait_vblank`: the game's ROM, a reset,
a state saved or loaded, the video, the sound (with the FM core and chip, changed as the game
plays: `mdrt_set_fm`), options and every key. A client that runs a ROM gives the platform
`rom_check` (are these bytes a ROM the game can run, or why not, in a few words) and `rom_use`
(the ROM, before the game starts), and `rom_path` (the command line's) and `rom_default`: the
platform reads the file, keeps the bytes, remembers the one the player chose in the settings and,
when none will do, opens the menu on a file browser. The menu's reset and a new ROM stop the game
(`mdrt_wait_vblank` returns nonzero), make a new console and call the game function again: it
must then start from scratch, as at power on, whatever it kept in its own statics (NHL '94's
`make golden-check` runs a restart against a fresh power on, frame by frame).

The pieces with no SDL in them are there for another platform (a browser's) to use:
`mdrt/settings.h` (the file's text, read and written back with its comments and unknown keys),
`mdrt/view.h` (where the picture goes in a window for each scaling), `mdrt/png.h` (a
screenshot's PNG, in memory), `mdrt/menu.h` (the menu's pages, inputs and drawing, with
pulseman's own font), `mdrt/upscale.h` (Scale2x, Scale3x and MMPX: the picture magnified as
pixel art, before the window's scaling), `mdrt/look.h` (dithering blended, and a picture
tube's scanlines and mask as an overlay to multiply the picture by), `mdrt/gif.h` (animated GIFs
with each frame's exact colours) and `mdrt/capture.h` (frames kept as they come, the last
seconds in memory or every one in a file, and read out as a GIF).

Without a window, fill `c.platform` (present, audio, wait, input: any may be NULL), create the
console with `mdrt_new(&c)` and call the game yourself.

Other settings: `c.pal` makes a PAL console (its master clock, 53,203,424 Hz, 313 lines a frame,
the version register's PAL bit; V30 when register 1 asks for it); `c.fm_gain` and `c.psg_gain`
set the mixer's balance in 1/32768ths (0 keeps the defaults, fitted to the oracle; consoles
differ by model).

## Threads

Everything runs on the game's thread: the bus functions, `mdrt_wait_vblank`, the sound tick, and
the platform callbacks it makes (present, audio, wait, input). The SDL2 platform keeps its window
on the thread that called `mdrt_sdl_run` and hands pictures and buttons across under its own
lock. Nothing in the runtime blocks, sleeps or reads a clock except the platform's `wait`.

## What is API

Use the functions. The module structs (`mdrt_vdp`, `mdrt_pads`, `mdrt_psg`, `mdrt_fm`) are
public so they can be embedded, but their fields are not API and may change: read registers with
`mdrt_vdp_reg`, state with `mdrt_vdp_get_state`. The exception is save RAM: `mdrt_sram`'s
`bytes`, `size` and `dirty` are the platform's interface for keeping it.

## The bus

| 68k access | runtime |
|---|---|
| move to / from $C00000 / $C00004 / $C00008 | `mdrt_vdp_data16/32`, `mdrt_vdp_ctrl16/32`, `mdrt_vdp_read16`, `mdrt_vdp_status`, `mdrt_vdp_hv8/16` on `mdrt_get_vdp(m)` |
| move.b to $C00011 (PSG) | `mdrt_psg_port(m, at, value)` |
| $A10003 / $A10005 (pads) | `mdrt_pad_read/write(mdrt_get_pads(m), addr, ...)` |
| $200000 save RAM | `mdrt_sram_read8/write8(mdrt_get_sram(m), off, ...)` (odd offsets) |
| waiting for the vertical blank | `mdrt_wait_vblank(m)`: nonzero means stop |

The VDP state can be set whole (`mdrt_vdp_set_state`) to start from another emulator's
snapshot; the sound chips cannot be (another emulator's FM core has other internals), so a client
starting from such a recording writes their registers back. The runtime's own states (below)
hold everything.

The status word shows the vertical blank (bit 3, also while the display is off), the vertical
interrupt pending (bit 7), FIFO empty (bit 9, always) and PAL (bit 0). DMA busy (bit 1) is never
set and the FIFO is never full: DMA completes at once and the FIFO drains at once, which is right
for a client that waits cooperatively. A program that polls until DMA busy is *set* would spin
for ever; port such a loop as a no-op.

The HV counter's default model is the beam at the clock's position (below), moved on a little
more at each read so that a client that moves no clock still reads changing values, shown as the
hardware's counters show it (the V counter's jump back to $E5 in NTSC's vertical blank, the H
counter's in the horizontal blank). Games mostly read it as a random source; anything that needs
the oracle's exact timing replays recorded values (`mdrt_set_hv_source`).

## Sound and time

Sound writes carry a time: master clocks since the current frame began (`mdrt_frame_mclks(m)`
long, 896040 NTSC; the frame begins at the handshake, in the vertical blank). The frame's audio is
made at the next handshake with every write at its time; a time past the frame's end lands in
the frames after. The YM2612 is written by port (`mdrt_ym_write(m, at, port, value)`: 0 / 1
part I's address / data, 2 / 3 part II's), as the chip needs address and data apart.

A game whose music runs on the Z80 ports the Z80 program to C (no Z80 is emulated) and runs it
on the runtime's clock: `mdrt_set_sound_tick(m, fn, user)` calls `fn(user, until)` while the
frame's audio is made, and the port runs its program up to master clock `until` of the frame,
writing the YM2612 as it goes. Between handshakes the client can run it further (when the game
waits on it); give the game's own code a cost per access so the port's time moves while the
game polls. NHL '94: `src/z80.c` (the relay), 56 master clocks a Z80-bus access.

## Time inside a frame

The composition keeps a position in the frame (`mdrt_now`, master clocks since the handshake,
which is the start of the vertical blank) that only the client moves, and draws the picture a line
at a time as the position passes each line's start, so VDP writes land on the lines after the
beam. A client that never moves it makes all its writes in the vertical blank, and the frame is
drawn whole at the handshake. Three ways to move it, for what the 68k code did:

| The 68k code | The port |
|---|---|
| ran code in the H-interrupt handler | `mdrt_set_hint(m, fn, user)`: `fn` runs at the start of each line the counter (register 10) picks, while register 0 enables it, whether or not the client moves the clock (the handshake draws the rest of the frame and calls it on the way) |
| polled the HV counter for line n, then wrote | `mdrt_wait_line(m, n)`, then the writes (nonzero: line n is past for this frame) |
| wrote the VDP while the beam ran, without waiting | `mdrt_charge(m, cycles)` as it runs: 68k cycles, 7 master clocks each (nonzero: the frame's end is reached, the vertical blank is due) |

`mdrt_run_until(m, mclk)` moves it to a time. The default HV counter shows the beam at the
position. The rules are the oracle's (`HARDWARE.md`, 2026-10-16): a line is drawn at the start of
its period, which begins with its horizontal blank, so a write there (the first 860 master clocks)
to the backdrop, CRAM, a name table or the display bit still changes that line; anything else shows
from the next. The H-interrupt handler is called at its line's start: a handler that does work
before it writes should charge those cycles first, or its writes count as made in the blank.

Sound times are the client's own (`mdrt_ym_write(m, at, ...)`); they need not follow the clock. The
first client, whose game writes the VDP in the vertical blank, moves no clock: its sound model
charges the Z80 bus accesses only, which would put its VDP writes on the wrong lines.

## The handshake

`mdrt_wait_vblank(m)` draws the picture, makes the frame's sound, hands both to the platform,
waits for its go (the SDL2 platform paces by the sound queue), and reads the pads. On return the
VDP is in its vertical blank (status bit 3): run the vblank interrupt's code, then call
`mdrt_vblank_end(m)`.

## States

`mdrt_get_state(m, buf, size)` writes the whole console as bytes and `mdrt_set_state(m, buf,
size)` loads them: the VDP with its ports and caches, the PSG, the FM chip and its core's own
state, the mixer, the sound written for times still to come, the I/O chip and the pads, save RAM,
the frame count and the HV beam. Size a buffer with `mdrt_get_state(m, NULL, 0)`. The format is
in `mdrt.h` (sections under four-character tags); the bytes are the same on every host for a
given build, so a state is a value: save states, rewind, rollback, golden tests.

The client adds what it keeps itself: `mdrt_add_state_memory(m, "RAM ", ram, 0x10000)` for a
block, `mdrt_add_state_part(m, tag, save, load, user)` for anything else (its sound processor,
registers it keeps in C globals). Every state then carries them, in the order added.

A load is all or nothing: a state from another version, another model (PAL), another FM core or
with another list of parts is refused (-1) and the console is as it was. A state from a console
with another audio rate loads, with the mixer started afresh, since the mixer only shapes the
sound the player hears.

What a state does not hold is where the client's code is. A blocking client is somewhere deep
in its own C stack when it waits; a state is only useful at a point the client knows how to go
on from with nothing on that stack (NHL '94: the game loop's wait, the point its lockstep already
restarts at). Take it there, and on loading, restart the client there. A client run as a
coroutine on its own stack could keep that stack too; nothing here needs it yet.

The player's requests reach the client through the platform: `mdrt_platform.requests` returns
MDRT_REQUEST_SAVE and MDRT_REQUEST_LOAD once for each press and MDRT_REQUEST_REWIND while it is
held; `mdrt_requests(m)` gives the client what the last handshake brought (the SDL2 platform:
the menu's Save and Load pages, and F4 held). The client decides what to do and when. `mdrt_request_path(m)` is the
file a SAVE or LOAD names, when the platform names one (the SDL2 platform's save slots and quick
resume: `states/slotN.state.new` to write, `states/slotN.state` to read); NULL leaves the file to
the client. A client that can take a state only at some points (NHL '94: in play) does nothing
otherwise, and the platform keeps the slot as it was. Fast-forward, slow motion,
pause and the frame step are the platform's alone: they change when its `wait` lets the next frame
go (and fast-forward and slow motion queue no sound), so the client sees only ordinary frames.

## The bus by address

`mdrt_bus_read8/16/32` and `mdrt_bus_write8/16/32` reach the 68k's bus by address, with the
access's width: the client's regions first (`mdrt_bus_map(m, base, size, read, write, user)`,
for a cartridge's mapper, EEPROM or coprocessor), then the VDP and PSG ports, the I/O chip, save
RAM, and the configuration's `read68k` for the rest. DMA from the 68k bus reads through it, so a
banked ROM window is seen by DMA too. A port of 68k code calls the modules directly and need not
use it; tools that work by address (a RAM watch, cheats, a script) do.

## FM cores, models, region

`mdrt_config.fm_core` picks the FM core by name (`mdrt_fm_cores(i)` lists the build's: "nuked"
and "ymfm" by default) and `fm_model` the chip: `MDRT_FM_YM2612`, the discrete chip of the first
consoles with its DAC's "ladder" distortion, or `MDRT_FM_YM3438`, the integrated one of later
consoles. `mdrt_new` returns NULL for a core the build lacks. `domestic` makes a Japanese
console (the version register's overseas bit clear), `pal` a PAL one. A state names its core and
model and loads only into a console with the same.

## Recording

`mdrt_record(m, MDRT_TRACE_VDP / _PADS / _SOUND, sink, user)` has the console write the trace
formats of `docs/VECTORS.md` to `sink(user, bytes, n)` from then on (the core touches no files:
the client's sink does); a NULL sink ends one. pulseman's tests replay them as they replay the
oracle's: the pictures, VDP states and pad reads must come out the same and the sound sample for
sample, so a run recorded with one build is a test of the next. Start the VDP trace after the
client has set any state outside the ports (a snapshot), since the trace begins with the VDP's
state as it is then.

## Lockstep

`mdrt_set_hv_source(m, fn, user)` and `mdrt_set_pad_source(m, fn, user)` replace the HV counter
model and the platform's buttons with recorded values, so a run can follow the oracle's.
