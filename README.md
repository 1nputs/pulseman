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

WP0: `make check` builds the (empty) library and the test harness, warning-clean with
GCC and Clang. `CC=clang make MODE=release check` for the Clang build.
