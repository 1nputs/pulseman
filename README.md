# md-runtime

A small, permissively licensed, modular Mega Drive / Genesis hardware runtime in C11: the VDP
(state machine and renderer), PSG, YM2612 interface, controller ports and save RAM, with an
SDL2 platform layer for Linux and a WebAssembly build for the browser. It is the runtime for
games reimplemented in C, first the NHL '94 port in the sibling `nhl95` repository, and is
meant to be reused by later ports and other projects.

No 68k or Z80 emulation: the game logic is C, and the game's own sound relay drives the sound
chips directly.

Start with `docs/WORKPLAN.md`: the decisions, the interfaces the first client needs, the work
packages in order, and how each one is verified against a real emulator.

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
- WP5 pads and save RAM: done. The game's pad port reads replay exactly (3600 / 3600 over 900
  frames of random input on both pads); 6-button sequence and save RAM checked by hand.

`make check` (GCC, sanitizers); `CC=clang make MODE=release check` for Clang; `make FM_CORE=ymfm
check` for the other FM core. Vectors come from
`$MDRT_VECTORS` (default `../nhl95/out/vectors`, recorded with
`../nhl95/tools/oracle/vdptrace.py`, `padtrace.py` and `sndtrace.py`; the test ROM with `tests/rom/build.sh`, which uses the vasm
built in `../nhl95/ref/`).
