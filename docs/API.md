# Using pulseman from a client

A client is a game reimplemented in C whose hardware accesses were 68k bus accesses. It links
`build/release/libmdrt.a` (and `libmdrt_sdl2.a` for a window), includes `mdrt/mdrt.h` (and
`mdrt/platform_sdl2.h`), and maps its hardware calls onto the runtime: a hardware layer of a
hundred lines or so (VDP ports, the pads, save RAM, the sound chips) and a main that starts the
platform.

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
every setting the first time and whenever the player changes one: the window's size, place and full
screen, vsync, the scaling (sharp, integer, fit) and aspect (square pixels, 4:3), the volume, mute,
sound device and latency, pausing when the window loses focus, fast-forward's speed, frames a
second shown at the picture's top right (the Options page; off by default), the screenshots'
directory and every key of both pads and of the player's controls (on the function
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
must then start from scratch, as at power on, whatever it kept in its own statics (worth a
test: a restart against a fresh power on, frame by frame).

A client with options of its own (a game's rules) describes them in `options` (`mdrt_sdl_option`: a
key, a label, named choices or a number's range, a default) with `options_title`: the menu gets a
page of them, the settings file keeps them, and `options_use` hands the client their values before
every power on, on the window's thread with the game stopped. Read them there and keep them for the
game: a change made in play shows from the next power on. `options_changed` can answer a change by
changing other values (a preset); it hears of the values read from the settings file too (`which`
-1), so a preset can agree with values edited there. Named choices can stand for numbers
(`numbers`, with `min`, `max` and a `unit`): the client gets the number, and the settings file
takes a choice's name or any number in the range, which the page shows as one more choice, "custom
(115%)". The options can be split into `pages`, each an item of the menu of its own, and
`options_text` puts the client's lines under a page's options, made again after each change (what
the choice now chosen holds, say). Lines in sections (a `\f` before each, its first line its title)
are shown a section at a time: the lines before the first section, then a line ("2 of 6" at the
left, the section's title at the right) whose LEFT and RIGHT step through the sections, then the
lines of the one shown; the page keeps it while the menu is open, and after a change on the page. A choice can also be found on disk (`browse`, an
`mdrt_sdl_browse`): a line under it opens the menu's file browser, and `options_pick` makes what
the player chooses there one of its choices (the settings file keeps its path, given back to
`options_pick` at the next start). `sram_file`, asked after `options_use`, names the save RAM's
file for that game when it follows the options (another set of data that keeps records of its own);
NULL keeps `sram_path`.

The pieces with no SDL in them are there for another platform (a browser's) to use:
`mdrt/settings.h` (the file's text, read and written back with its comments and unknown keys),
`mdrt/view.h` (where the picture goes in a window for each scaling), `mdrt/png.h` (PNGs
in memory: a screenshot's; an indexed picture's, deflated; any PNG read back, every colour type,
bit depth and interlacing, checked so that a file from anywhere is safe to read; and deflate both
ways, `mdrt_deflate` and `mdrt_inflate`), `mdrt/zip.h` (zip archives in memory: read and checked
so that one from anywhere is safe to open, and written, the same files making the same bytes), `mdrt/menu.h` (the menu's pages, inputs and drawing, with
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
game polls (56 master clocks a Z80-bus access, for instance).

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

Sound times are the client's own (`mdrt_ym_write(m, at, ...)`); they need not follow the clock. A
client whose game writes the VDP only in the vertical blank can move no clock at all and keep a
sound model of its own (charging, say, only the Z80 bus accesses, which would put its VDP writes
on the wrong lines if it moved the clock with them).

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
on from with nothing on that stack (the game loop's wait for the next frame, typically). Take
it there, and on loading, restart the client there. A client run as a
coroutine on its own stack could keep that stack too; nothing here needs it yet.

The player's requests reach the client through the platform: `mdrt_platform.requests` returns
MDRT_REQUEST_SAVE and MDRT_REQUEST_LOAD once for each press and MDRT_REQUEST_REWIND while it is
held; `mdrt_requests(m)` gives the client what the last handshake brought (the SDL2 platform:
the menu's Save and Load pages, and F4 held). The client decides what to do and when. `mdrt_request_path(m)` is the
file a SAVE or LOAD names, when the platform names one (the SDL2 platform's save slots and quick
resume: `states/slotN.state.new` to write, `states/slotN.state` to read); NULL leaves the file to
the client. A client that can take a state only at some points (in play, say) does nothing
otherwise, and the platform keeps the slot as it was. Fast-forward, slow motion,
pause and the frame step are the platform's alone: they change when its `wait` lets the next frame
go (and fast-forward and slow motion queue no sound), so the client sees only ordinary frames.

## Frames played again, and the simulation's hash

Rollback netplay plays frames again when a peer's input turns out to differ from the guess: it
loads an earlier state and runs the frames since with the right input, all within one frame of the
player's time. `mdrt_set_quiet(m, n)` makes the next `n` handshakes quiet: the frames they end are
not drawn (the lines are still counted, so the H-interrupts come on their lines and the HV counter
reads as ever) and the platform never sees them (no present, audio, wait or requests). Their sound
is made as ever, the client's sound tick included, so the chips stay in step and only the samples
are dropped. Drawing starts again with the frame the last quiet handshake begins, so after loading
frame `s`'s state while at frame `f`, `mdrt_set_quiet(m, f - s)` plays the frames between quietly
and draws frame `f` whole. `mdrt_quiet(m)` is how many are left. Quiet is not part of a state.

`mdrt_sim_hash(m)` hashes what the game can see: the clock (the frame count, the position, the
lines passed, the H-interrupt counter, the HV counter's nudge), the VDP with its ports, the pads,
save RAM and the client's parts; not the picture, the PSG, the FM chip, the mixer or the sound
still to come, which only the player sees and hears. Two consoles that will go on alike have the
same hash, whatever FM core each plays its sound with. A client's part is in it unless marked
otherwise: `mdrt_set_state_part_kind(m, tag, MDRT_PART_PRESENTATION)` leaves one out, and a game
that reads the FM chip's status puts `"FM  "` in with `MDRT_PART_SIMULATION`. The hash is the
same on every host for a given build when the client's parts are written the same everywhere
(field by field, not a struct copied with its padding).

Netplay is the pad source, and the person playing is still at the platform:
`mdrt_platform_input(m, buttons)` reads the platform's buttons whatever the pad source.

## Netplay

`mdrt/netplay.h` plays a game between two to four machines with rollback: each console plays its
own pads at once and guesses the others' (the buttons last heard, held); when another's buttons for
a frame come and differ from the guess, it loads its state from before that frame and plays the
frames since again, quietly, within one frame of the player's time. The machines are a star: a
host and up to three guests, each guest talking to the host alone, the host passing every guest's
buttons on to the others as they come. The module knows no game and touches no sockets or clocks;
it never blocks.

The platform gives it a link (`send` and `recv` of datagrams to the other peer, without blocking,
and `ticks`, a clock in microseconds; a host's, `send_to` and `recv_from` too, for several guests
by an index the link gives each address) and the local players' buttons (`local_input`). A
session:

1. `mdrt_net_new(&config)`: the role (host, join, or either: two machines that settle it), the
   identity (a string of everything the simulation depends on: the build, the ROM, settings that
   must match; another one is refused), the player's name (the others are told it), the players
   at this machine (the pads it plays: one by default), and for the host the players in all it
   waits for (two by default; the pads go out in the order the guests came, its own first: two
   machines with one each, pads 1 and 2; four with one each, pads 1 to 4; two and one and one;
   one and three ...; a guest that would make more is refused), the input delay, the window
   (frames played on guesses before waiting), how far a rollback may go back, how often to check
   for a desync, and its settings for the guests' clients (rules, save RAM, a game's league with
   its art: a blob of up to 16 MB, sent to each 1 KB at a time with 128 in flight; the guest says
   which it has, and the lost ones go again a round trip on).
2. `mdrt_net_connect(n)` until it returns `MDRT_NET_READY` (or `_REFUSED`, with `mdrt_net_why`):
   every player is in; each guest's client then applies `mdrt_net_settings(n, &size)` and powers
   on. `mdrt_net_peers`, `mdrt_net_me`, `mdrt_net_name_of`, `mdrt_net_players_of` and
   `mdrt_net_pads_of` say who is in (a host's waiting room as it fills); `mdrt_net_all_pads`, the
   pads played (pads 3 or 4: the ports need EA's 4 Way Play). Protocol 4.
3. `mdrt_net_start(n, m)` on the console just powered on: the module becomes its pad source.
4. At every wait after its vertical blank code the client calls `mdrt_net_frame(n, resumable)`,
   `resumable` saying whether it could go on from here after a state is loaded (its game loop's
   wait). `MDRT_NET_GO`: play the next frame. `MDRT_NET_WAIT`: call again a little later without
   playing on (the other's buttons are too far behind, or this peer gives a frame back to stay in
   step). `MDRT_NET_RESTART`: a state was loaded; restart at the resumable point and play on
   (the frames played again are quiet: `mdrt_quiet`). `MDRT_NET_ENDED`: the session is over
   for every machine (a desync, a player who left, a connection lost: `mdrt_net_why`, the same
   words on each), and the console plays on alone.
5. `mdrt_net_quit(n)` says goodbye; `mdrt_net_free(n)`.

A state is taken at every resumable point. Where a game has none for a while (a menu, a loading
screen), the module guesses only as far back as it can roll back, then waits for each frame's
buttons. The client's part is small: report its waits, restart when told, keep everything it goes
on from in the state's parts, and make its identity. `mdrt_net_stats_get` gives the round trip,
rollbacks, waits and checks for the platform to show; `mdrt_net_set_log` writes every frame's
buttons as they become known, a replay of the session from power on.

`mdrt/net_udp.h` is the native link: UDP (POSIX sockets; Winsock 2 on Windows, linking `ws2_32`),
IPv4 and IPv6, names resolved. `mdrt_udp_open(port, peer, err, n)`: a host gives no peer and hears
whoever sends (each address a place for `send_to` and `recv_from`, up to eight) until
`mdrt_udp_lock` (call it once the session starts); a joiner names the host's address; two peers
that both connect each name the other's (a simultaneous open, which most home routers let through
without a forwarded port).
`mdrt_udp_link(u)` is the link for the configuration; `mdrt_udp_wait(u, ms)` sleeps until a datagram
comes, for a caller told to wait; `mdrt_udp_simulate` delays, reorders, loses and doubles what it
sends, for tests. In the SDL2 platform a client that plays online gives `mdrt_sdl_options.net_identity`, and the
menu has an Online play page: the player's name; host, join a host, or both connecting at once;
the other's address and the port (fields the player types into, kept in the settings file); this
machine's LAN address (`mdrt_udp_lan_address`); the input delay; for a game of more than two players
(`mdrt_sdl_options.net_pads`, up to 4), the players at this machine (the window's players 1, 2 ...
on its pads) and a host's players in all, with EA's 4 Way Play in the ports when pads 3 or 4 are
played; and Start,
which stops the game, connects, and powers on again in the session (or Leave, during one). What the
game plays by that is sent rather than compared (its options, its data, save RAM) comes from
`mdrt_sdl_options.net_settings`, asked when Start makes the session, once the game has stopped and
`options_use` has had the menu's values: the bytes this player sends if it hosts (the session
copies them). At power on the game reads the host's with `mdrt_net_settings` (the host its own) and
plays by them on both. A client
can also make a session from its command line and put it in `mdrt_sdl_options.net` (its UDP link
in `net_udp`). Either way the window connects before the game starts (a page that says so, a
host's with who has joined so far, Esc giving up), then powers on a new console with the session started on it: the game's function
finds it with `mdrt_get_net(m)`, calls `mdrt_net_frame` at its waits as above, and `mdrt_idle(m)`
while told to wait (the window waits a millisecond there; nonzero: the window is closing, so say
goodbye and stop). The window says who joined ("Sam has joined") and why a session ended ("Sam
left") on a band across the bottom row of the game's picture for a few seconds.
While the session lasts the window paces the frames by its clock and never makes up the time the
game spent waiting on the other player in `mdrt_idle` (a frame given back to keep in step stays
given; other lateness is caught up as ever), stretches the sound by up to half a per cent to
keep its queue, and keeps the session's rules: the menu does not stop the game, and pause, the
frame step, slow motion, fast-forward, rewind, the save slots, quick resume, reset, another ROM,
the FM core and chip and the Players page's adaptor (the session's pads say) are not to be had; save RAM is not read from the player's file, nor written back to it by that
console ever (after the session too, while the game plays on alone: its save RAM is the session's).
`mdrt_net_state(n)` says where a session is. Every client reads the same options: `mdrt_net_parse` takes `--host [PORT]`,
`--join ADDR[:PORT]`, `--peer ADDR[:PORT]`, `--port`, `--delay`, `--name`, `--here N` (the players
at this machine), `--players N` (a host's players in all) and `--net-sim` from its command line
(`MDRT_NET_USAGE`), and `mdrt_net_open_udp` opens the socket they ask for.

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
