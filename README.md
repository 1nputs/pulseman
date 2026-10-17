# pulseman

*Named for the hero of Game Freak's 1994 Mega Drive game, who lives inside the circuitry.*

A small, permissively licensed, modular Mega Drive / Genesis hardware runtime in C11: the VDP
(state machine and renderer), PSG, YM2612 interface, controller ports and save RAM, with an
SDL2 platform layer for Linux. A WebAssembly build for the browser is planned (WP8, not started).
It is the runtime for games reimplemented in C, first NHL '94 in
[ninety-four](https://github.com/1nputs/ninety-four) (which has it as a git submodule), and is
meant to be reused by later ports and other projects. The C API is prefixed `mdrt_` (Mega Drive
runtime).

No 68k or Z80 emulation: the game logic is C, and the game's own sound relay drives the sound
chips directly.

Documents: `docs/API.md` (using the runtime from a client), `docs/VECTORS.md` (the test vectors
and their formats), `docs/HARDWARE.md` (the behaviour modelled and where each piece comes from).
This README carries the status.

## Status

- WP0 skeleton: done.
- WP1 VDP state machine: done. All eight NHL '94 traces (787 frames: boot, title, menus,
  faceoff, play, pause) replay with registers, VRAM, CRAM and VSRAM byte-exact at every frame and
  every data port read matching the oracle; 89 hand-written unit checks.
- WP2 renderer: done. Pixel-exact on every frame of the NHL '94 traces and of `vdptest`
  (tests/rom/vdptest.s, our own program: H40 / H32, 128-cell planes, line / cell / column
  scroll, the window both ways, 80 sprites, the per-line limit and X 0 masking, fill and copy
  DMA): 1035 frames. 37 unit checks; `build/framediff/` gets a PNG of the first bad frame.

- WP3 PSG: done. `src/psg/sn76489.c`: the documented SN76489 (Sega's noise taps, 2 dB steps),
  rendered at the YM2612's rate with band-limited steps (integer only). 46 unit checks
  (frequencies, attenuation, protocol, noise); against the oracle's PSG-only audio of 100 s of
  NHL '94 from power on (`game_psg.sndtrace`): loudness within 1 dB in all 866 windows of 50 ms
  with sound, spectrum within 3 dB (mean 0.35 dB), the whole trace within 0.68 dB band by band.
- WP4 FM and mixer: done. `src/fm/`: the YM2612 behind `mdrt/fm.h`, Nuked-OPN2 (vendored,
  LGPL-2.1) by default or ymfm (vendored, BSD-3, C++) with `make FM_CORE=ymfm`, both at the same
  level. `src/mix/`: FM + PSG at the oracle's balance and loudness, a 10 Hz DC blocker, a
  polyphase resampler to any rate (20 kHz low-pass; integer only). Unit checks: ports reach the
  core (a voice on each part, panned, at its pitch), the DAC, reset; the resampler's pitch,
  level, pass band and stop band (-75 dB at 26 kHz). Against the oracle (`game*.sndtrace`, the
  title song and play): FM alone within 1 dB loudness in 1299 / 1305 windows (the oracle's MAME
  core; 1304 / 1305 against the oracle on Nuked-OPN2 too), the full mix within 1 dB in 1614 /
  1621 at the same level (gain 0.9998), peak 25303, nothing clipped.
- WP6 composition and the frame handshake: done. `mdrt_new` puts the modules together;
  `mdrt_wait_vblank` draws the picture, makes the frame's sound (writes at their master clock in
  the frame, the client's sound processor run through the sound tick, writes past the frame's end
  carried over), hands both to the platform's callbacks, waits for its go, reads the pads; HV
  counter and pads injectable. A test game (moving picture, a scale on the PSG, DAC samples from
  a tick) runs 600 frames headless: 480605 samples at 48 kHz against 480604.4 (no drift),
  deterministic over two runs, every note at its pitch.
- WP7 native platform (SDL2): done. `mdrt_sdl_run` (`include/mdrt/platform_sdl2.h`,
  `src/platform/sdl2/`): the game on its own thread, the window on the caller's, frames paced by
  the sound queue (3 frames' worth), the keyboard and two game controllers, save RAM in a file,
  pause, full screen, a clean quit (`mdrt_wait_vblank` returns nonzero). `make sdl` builds it and
  the examples: `testgame` (the WP6 game in a window: 300 frames in 4.88 s with SDL's real-time
  disk audio, 5.00 s of sound) and `trace-player` (a client's .vdptrace pictures and .sndtrace
  sound live; NHL '94's 100 s game trace plays in real time at 42% of a core, release build).
  Under the sanitizers SDL2 reports a 768-byte leak of its own (`ASAN_OPTIONS=detect_leaks=0`).
- WP9 client integration: NHL '94 runs on the runtime with sound (`../ninety-four`, `make nhl94`):
  its `native/hal_native.c` maps the game's hardware calls, its Z80 relay port feeds the FM chip
  through the sound tick, and the first two seconds of its sound match the oracle's within about
  1 dB a 100 ms window. `docs/API.md` describes the contract.
- WP5 pads and save RAM: done. The game's pad port reads replay exactly (3600 / 3600 over 900
  frames of random input on both pads); 6-button sequence and save RAM checked by hand.
- 2026-10-16, four players: EA's 4 Way Play in both ports (`mdrt_pads_set_adaptor`): port 2's
  output lines choose which of four 3-button pads port 1 reads, or the adaptor's mark the game
  looks for. Pads 1-4 through the platform's input and the pad source; `.padtrace` version 2
  carries the device and four pads (version 1 still replays); a state from before loads.
  NHL '94 from power on with the adaptor in the reference emulator, four pads pressing their own
  buttons through the title and the setup screen, replays 17878 / 17878 reads (its
  `four_way.padtrace`), the detection included.
- 2026-10-16, players in the SDL2 platform: a Players page (two pads or EA's 4 Way Play, from
  the next power on; each player's controller, controllers keeping the slot they were plugged
  into; the controllers' buttons and turbo A / B / C, each set by pressing the button; turbo's
  speed; the stick's dead zone), keys for pads 3 and 4 and turbo keys. `make sdl-check` plugs in
  two virtual controllers and checks players 1 and 3 get them, a moved button, the d-pad, the
  dead zone, turbo, a controller pulled out, and a button set from the menu.
- 2026-10-16, save slots and quick resume in the SDL2 platform: requests can name a file
  (`mdrt_platform.request_path`, `mdrt_request_path`); eight slots in `states/` with a picture each
  (`mdrt_png_decode` reads it back; the menu shows it beside the list at half size); a save goes to
  `slotN.state.new` and becomes the slot only when the client writes it; F9 / F10 use the slot
  chosen last; quick resume saves at quitting and loads at the next start, or forgets when no state
  could be taken. `make sdl-check`: F9, the Save and Load pages, F10, a save the game refuses, a
  state at quitting and the next start back at it, and quitting out of play leaving none.
- 2026-10-16, the looks in the SDL2 platform's Video page: the colours' levels (`mdrt_vdp_rgb`'s
  curves: Genesis Plus GX's as before, full range, or the console's own, measured at its video
  output), dithering blended as a television blended it (only where a column of one colour stands
  between two of another with its colour again beyond, so lines and edges stay sharp; or every
  pixel softened across), and a picture tube's scanlines (light, medium, heavy: at the picture's own
  lines at any size) and mask (aperture grille, slot mask, shadow mask: at the window's pixels).
  `mdrt/look.h` has no SDL: the platform blends the frame before any upscaler, then multiplies the
  scaled picture by an overlay the size of the picture in the window and adds the picture again at
  the lift, which gives back on average the light the tube takes, so the brightest colours still
  reach white. The game's frames only, never the menu; screenshots stay as the console made them.
  `platform_unit` checks the levels, the blend on hand-made rows, the scanlines' rows at 2x, 3x
  and an uneven size, each mask's pattern, and the light kept on average; `make sdl-check` turns
  them on from the menu and plays with them.
- 2026-10-16, captures in the SDL2 platform: the replay (the last 20 seconds always kept, saved
  by F10 or the menu's Capture page) and recordings (F11 to start and to stop), saved in `videos/`
  as GIFs made here (lossless, 30 frames a second: GIF's times are in hundredths and players show
  a hundredth as a tenth, so 60 cannot be had; WebP or APNG could, with a library, left for now).
  `mdrt/gif.h` (each frame's own exact colours, GIF's LZW) and `mdrt/capture.h` (frames kept
  compressed, about 8 KB a frame; a clip read into a GIF) have no SDL. The game thread hands each
  frame over as it is made (0.4 ms); clips are saved on a thread of their own while the game
  plays on. The keys moved with them: F9 is free, F10 the replay, F11 recording, F12 a
  screenshot; states are saved and loaded from the menu, full screen is the menu's and
  Alt+Enter's, and an older build's keys leave the settings (`mdrt_settings_remove`). A control's
  key may have Shift, Ctrl or Alt. `platform_unit` checks the LZW back and forth, the frames kept
  and taken, a recording's file, the GIF's frames and times; Pillow read real NHL '94 frames and
  49 hard cases back exactly; `make sdl-check` saves the replay, records a second, saves from the
  menu, reads each GIF back, saves and loads states from the menu, and binds a key with Ctrl by
  pressing it.
- 2026-10-16, the review's small findings: PAL's master clock (`MDRT_MASTER_CLOCK_PAL`,
  `mdrt_master_clock`) in the mixer and the platform; the default HV counter through the VDP's
  counter mapping; `mdrt_vdp_reg` and a rule for what in the structs is API; the mixer's gains in
  `mdrt_config`; sound events kept in time order as they arrive (no sort per chunk);
  `MDRT_REQUIRE_VECTORS`; the vector formats in `docs/VECTORS.md`; Clang checked. The first
  client's golden runs (its picture, sound and state, frame by frame) are unchanged.
- 2026-10-16, the whole console's state as a value (`mdrt_get_state`, `mdrt_set_state`, parts
  the client adds; `docs/API.md`, "States"): every module saves and loads itself as bytes, ymfm
  through its own save format; Nuked-OPN2's chip type is now per chip. Loads are all or nothing.
  `tests/state_test.c`: a state taken mid-note gives the same 60 frames of pictures, VDP state
  and sound again, in the same console and a new one. The player's requests (save, load, rewind)
  come through the platform (`mdrt_requests`; F9, F10, F4 in SDL2). The first client saves,
  loads and rewinds with it; its states go on bit for bit for thousands of frames.
- 2026-10-16, time inside a frame: a position in the frame only the client moves
  (`mdrt_now`, `mdrt_run_until`, `mdrt_charge`, `mdrt_wait_line`), the picture drawn a line at
  a time as it passes them with the oracle's rules for writes in a line's horizontal blank
  (`mdrt_vdp_beam`), the H-interrupt handler called on the lines register 10 picks
  (`mdrt_set_hint`), the HV counter at the position. Vectors: `.vdptrace` version 2 carries each
  access's time; `tests/rom/vdptest.s` phases 8 (H-interrupts) and 9 (HV polls) and all nine
  traces replay pixel-exact; `tests/clock_test.c` checks the hooks. A client that moves no clock
  gets the frame drawn whole at the handshake, as before (the first client's golden runs are
  unchanged).
- 2026-10-16, the bus map, run-time FM cores, the recorder. `mdrt_bus_map` and `mdrt_bus_*`: the
  68k's bus by address, the client's regions (a mapper, an EEPROM) before the console's ports, DMA
  through it (`tests/bus_test.c`: a mapper's banked window read by DMA). Both FM cores in one
  build, chosen by `mdrt_config.fm_core`, and the chip by `fm_model`: the YM2612 or the YM3438
  (no DAC ladder); a Japanese console by `domestic` (`fm_unit` runs every core and model,
  `fm_trace` and `mix_trace` every core against the oracle). `mdrt_record`: the console writes
  `.vdptrace`, `.padtrace` and `.sndtrace` itself to the client's sink; `tests/record_test.c`
  records a test game and `make check` replays its traces exactly (pictures, VDP state, pad
  reads, the sound sample for sample); the first client's own 600-frame recording replays the same
  way (its `make runtime-selfcheck`). (RUNTIME-DIRECTION 5, 9, 8.)
- 2026-10-16, the player's controls in the SDL2 platform: a settings file the player can edit
  (`mdrt/settings.h`: comments, order and unknown keys kept; `settings.ini` where the game runs)
  with every key rebindable (both pads' twelve buttons, the controls, on F1-F12 by default), the
  window's size, place and full screen remembered;
  volume and mute, the sound device and latency; fast-forward held (4 times by default), slow
  motion, a frame at a time while paused, pausing when the window loses focus (the keys let go);
  screenshots as PNG at the console's resolution (`mdrt/png.h`); sharp (whole multiple, then
  smooth), integer and fit scaling, square or 4:3 (`mdrt/view.h`), hi-DPI. Changes show for a
  moment in the window's title. `tests/platform_unit.c` checks the settings, the placements and
  the PNG decoded back; `make sdl-check` plays the platform from a thread pressing keys under SDL's
  dummy video and disk sound (frame rates, pause and step, focus, the screenshot, the volume and
  mute in the sound written, the file, keys rebound by hand).
- 2026-10-16, the menu (Esc, or a controller's Back or Guide): `mdrt/menu.h` draws pages over the
  picture at the console's resolution in pulseman's own 5 x 8 font and works them with six inputs.
  The SDL2 platform's pages: the game's ROM (a file browser; the client checks each file through
  `rom_check` and gets it through `rom_use`; the one chosen is remembered, and with none the game
  starts on that page), a reset (a new console, the game function called again), states, video,
  sound with the FM core and chip changed as the game plays (`mdrt_set_fm`: a new chip given the
  old one's registers between frames), options, and every key changed by pressing it. The game
  waits while it is open; the key that closes it does not reach the game. `fm_unit` switches a
  console's chip to every core and model and hears the voice go on at its pitch; `platform_unit`
  works a page and draws it; `make sdl-check` works the menu as a player (a ROM turned down, one
  taken, the video and FM chip changed, a reset, another ROM in play, a key changed, the ROM found
  again on the next start).
- 2026-10-16, pixel-art upscalers (`mdrt/upscale.h`, no SDL): Scale2x and Scale3x (EPX's rules as
  scale2x.it publishes them) and MMPX 2x and 4x (McGuire and Gagiu, JCGT 2021, following the
  authors' MIT-licensed C++). Each source pixel becomes a block of copies of its neighbours, so no
  new colours appear. The SDL2 platform runs the game's frames through the one chosen in the
  Video menu (`video.upscaler`) before its scaling, on the window's thread (a 320 x 224 frame:
  0.3, 0.6, 2.1 and 8 ms), never the menu; screenshots stay at the console's resolution.
  `platform_unit` checks hand-worked cases, Scale3x against a second formulation, and the outputs
  on generated pixel art (noise, blocks and lines at every slope, text, a dither) against hashes
  of the authors' own code; `make sdl-check` plays with MMPX 4x on.
- 2026-10-16, xBR 2x, 3x and 4x (Hyllian's xBR-lv2, following his MIT-licensed shader at its
  defaults): it blends along the edges it finds, so curves and text come out smooth (new colours
  between). Its decisions are made in whole numbers: a console's colour levels are evenly spaced,
  so the shader's comparisons often come out exactly equal, where float arithmetic lets the last
  bit decide. `platform_unit` checks it against hashes of a line-by-line transcription of the
  shader in exact fractions (identical, pixel for pixel, at every size, on the generated pictures
  and real NHL '94 frames); Mesa's software GPU running the shader itself agrees but for a level
  here and there and the exact ties, where a GPU's rounding goes its own way. 3.4 ms at 2x and
  4.8 ms at 4x for a 256 x 224 frame; `make sdl-check` plays with xBR 3x for a run.

`make check` (GCC, sanitizers; both FM cores, each against the vectors); `make sdl-check` (the SDL2
platform, about 15 s in real time; `MODE=release` to time fast-forward too); `CC=clang CXX=clang++
make MODE=release check` for Clang (checked on 2026-10-16 with Zig's Clang 22: `pip install
ziglang`, `CC="python -m ziglang cc" CXX="python -m ziglang c++" LDLIBS=-lc++`, ymfm then linking
Zig's libc++). For Windows it cross-builds with MinGW-w64 GCC (`make CC=x86_64-w64-mingw32-gcc
CXX=x86_64-w64-mingw32-g++ AR=x86_64-w64-mingw32-ar MODE=release B=build/win64 sdl`, with SDL2's
MinGW package on `pkg-config`; checked on 2026-10-16) or builds in MSYS2; `WERROR=` turns warnings
back into warnings for a compiler that is newer than these. `FM_CORES` picks the FM cores a build has, chosen at run time
(`mdrt_config.fm_core`): "nuked ymfm" by default; `make FM_CORES=ymfm` builds without Nuked-OPN2,
so with no LGPL code (for a release or browser build: review finding 1); a build with ymfm links
`-lstdc++`. Vectors come from `$MDRT_VECTORS` (default
`../ninety-four/out/vectors`; `docs/VECTORS.md`); `MDRT_REQUIRE_VECTORS=1` makes a missing vector a
failure rather than a skip. The test ROM is assembled with `tests/rom/build.sh` (vasm; the
default is the one built in `../ninety-four/ref/`).

Third-party code: Nuked-OPN2 (LGPL-2.1) and ymfm (BSD-3) in `third_party/`; MMPX (MIT, Copyright
2020 Morgan McGuire & Mara Gagiu) and xBR (MIT, Copyright 2011-2016 Hyllian) in
`src/platform/upscale.c`, which follows their reference code and carries their notices. A binary
that includes them ships their licences.
