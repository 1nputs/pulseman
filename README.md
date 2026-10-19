# pulseman

*Named for the hero of Game Freak's 1994 Mega Drive game, who lives inside the circuitry.*

A small, permissively licensed, modular Mega Drive / Genesis hardware runtime in C11, for games
reimplemented in C: the game logic is the client's C code, and pulseman is the console around it.
There is no 68k or Z80 emulation; a client calls the runtime where its game touched the
hardware, and its own sound driver (in C) feeds the sound chips. The C API is prefixed `mdrt_`
(Mega Drive runtime).

Documents: `docs/API.md` (using the runtime from a client), `docs/VECTORS.md` (the test vectors
and their formats), `docs/HARDWARE.md` (the behaviour modelled, and where each piece comes from).

## What it has

The console, in `libmdrt` (no SDL):

- **VDP**: the state machine (ports, the FIFO's read-back, DMA from the bus, fill and copy) and
  the renderer (H40 and H32, every plane size and scroll mode, the window, sprites with the
  per-line limits), drawn a frame at a time at the handshake, or a line at a time as a
  client-owned clock passes each line (`mdrt_vdp_beam`), with the H-interrupt on register 10's
  lines and the HV counter at the beam.
- **Sound**: the SN76489 PSG (Sega's noise taps, band-limited steps), the YM2612 behind
  `mdrt/fm.h` with two cores chosen at run time (Nuked-OPN2, LGPL-2.1, and ymfm, BSD-3), the
  YM3438 too, and a mixer (the chips at a reference emulator's balance, a DC blocker, a polyphase
  resampler to any rate). Sound writes carry their master clock; a client's sound processor runs
  through the sound tick.
- **I/O**: 3- and 6-button pads, EA's 4 Way Play adaptor in both ports, save RAM, and a bus map
  for a cartridge's own regions (a mapper, an EEPROM) that DMA reads through.
- **The console as a value**: `mdrt_get_state` / `mdrt_set_state` (every module, and parts the
  client adds), all or nothing; NTSC and PAL clocks; a Japanese console.
- **Recording**: `mdrt_record` writes the console's own `.vdptrace`, `.padtrace` and `.sndtrace`.
- **Rollback netplay**: `mdrt/netplay.h`, two consoles on two machines playing one game (each
  player's buttons at once, the other's guessed, frames played again quietly when a guess was
  wrong, desync checks), knowing no game and never blocking; `mdrt/net_udp.h` its UDP link (IPv4
  and IPv6, host and join or both connecting at once) and the options every client reads; quiet
  frames (`mdrt_set_quiet`) and the simulation's hash (`mdrt_sim_hash`) beneath them.
- **Libraries a platform uses**: `mdrt/png.h` (screenshots, indexed pictures, any PNG read back,
  deflate both ways, no zlib), `mdrt/zip.h` (zip archives read and checked, and written the same
  every time), `mdrt/gif.h` and `mdrt/capture.h` (lossless animated GIFs, frames kept
  compressed), `mdrt/upscale.h` (Scale2x, Scale3x, MMPX, xBR), `mdrt/look.h` (colour levels,
  dithering blended, scanlines and masks), `mdrt/menu.h` (menu pages in pulseman's own font),
  `mdrt/settings.h` (a hand-editable settings file kept as written), `mdrt/view.h` (where the
  picture goes in a window).

The SDL2 platform, in `libmdrt_sdl2` (`mdrt_sdl_run`, `include/mdrt/platform_sdl2.h`): the game on
its own thread, frames paced by the sound queue, keyboard and controllers for up to four players
(each button rebindable, turbo, dead zone), save RAM in a file per game, pause, fast-forward, slow
motion and frame step, full screen and hi-DPI, vsync (no tearing; the game keeps its own time),
scaling (sharp, integer, fit; square or 4:3) with the upscalers and looks above, screenshots,
replays and recordings as GIFs, save slots with a picture each, quick resume, and a menu (Esc) with
the ROM browser, reset, states, video, capture, sound (the FM core and chip changed live), players,
keys, and pages of the client's own options handed to it before every power on. And netplay's
window: it connects before the game starts, then paces by the clock and keeps the session's
rules (no pause, rewind or states while two play).

## Building and checking

```
make                # libmdrt, the tests and examples (MODE=debug: -O1 with sanitizers)
make check          # the tests
make sdl            # libmdrt_sdl2 and the examples (testgame, trace-player)
make sdl-check      # the SDL2 platform, played from a thread under SDL's dummy drivers (about 15 s)
make MODE=release   # optimised, no sanitizers (MODE=release sdl-check also times fast-forward)
```

`FM_CORES` picks the FM cores a build has: "nuked ymfm" by default; `make FM_CORES=ymfm` leaves
out Nuked-OPN2, so the build has no LGPL code. A build with ymfm links `-lstdc++`.

Clang: `CC=clang CXX=clang++ make MODE=release check` (Zig's Clang works: `pip install ziglang`,
then `CC="python -m ziglang cc" CXX="python -m ziglang c++" LDLIBS=-lc++`). Windows: a MinGW-w64
cross build (`make CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ AR=x86_64-w64-mingw32-ar
MODE=release B=build/win64 sdl`, with SDL2's MinGW package on `pkg-config`) or MSYS2. `WERROR=`
turns warnings back into warnings for a compiler newer than these. Under the sanitizers SDL2
reports a 768-byte leak of its own (`ASAN_OPTIONS=detect_leaks=0`).

### What the checks cover

Unit tests check each module by hand-worked cases: the VDP's registers, DMA and rendering, the
PSG's frequencies, noise and attenuation, the FM ports and DAC, the resampler's pass and stop
bands, the pads and adaptor, states that replay the same pictures and sound, the clock hooks, the
bus map, the recorder (a test game's own traces replayed exactly), PNGs of every kind and damaged
ones, zip archives (Python's, hostile and damaged ones), the GIF encoder, the upscalers against
their reference code, and the menu and settings.

The test vectors are recordings of real programs on a reference emulator (Genesis Plus GX, a
development tool only, never part of pulseman): every VDP access with the state and picture at
each frame, every pad port access, every sound chip write with the emulator's audio. The VDP
replays them byte-exact (registers, VRAM, CRAM, VSRAM) and pixel-exact at every frame, the pads
read for read, and the sound chips within measured tolerances of loudness and spectrum (the cores
are different emulations). Vectors come from copyrighted programs, so none is committed: a client
records them with its oracle tools and points `make check` at them with `MDRT_VECTORS=DIR`
(`docs/VECTORS.md`). Without it the vector tests say so and are skipped; `MDRT_REQUIRE_VECTORS=1`
makes a skip a failure, so a wrong path cannot pass. pulseman's own test ROM
(`tests/rom/vdptest.s`: both modes, plane sizes, every scroll mode, the window both ways, 80
sprites and the per-line limits, fill and copy DMA, H-interrupts, HV polls) is assembled with
`tests/rom/build.sh` (vasm: `VASM=path`, or `vasmm68k_mot` on the `PATH`) and recorded the same
way.

## Planned

A WebAssembly build for the browser.

## Licences

pulseman is MIT. Third-party code: Nuked-OPN2 (LGPL-2.1) and ymfm (BSD-3) in `third_party/`;
MMPX (MIT, Copyright 2020 Morgan McGuire & Mara Gagiu) and xBR (MIT, Copyright 2011-2016 Hyllian)
in `src/platform/upscale.c`, which follows their reference code and carries their notices. A
binary that includes them ships their licences.
