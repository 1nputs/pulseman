# Test vectors

A vector is a recording of what a real program did to the hardware and what the hardware did
back, made by the oracle (Genesis Plus GX running the program, a development tool only). The
runtime replays it and must answer as the oracle did. This file is the specification of the
formats: the runtime owns them, and every exporter and reader follows this file. Vectors are
never committed (they come from copyrighted programs, apart from the test ROM's).

## Where they live

`make check` reads every vector in `$MDRT_VECTORS` (default `../nhl95/out/vectors`). With no
directory, or no vector of a kind, the vector tests say so and are skipped; set
`MDRT_REQUIRE_VECTORS=1` on a machine that has the vectors and a skip is a failure instead, so a
wrong path cannot pass. The first client's `make runtime-check` sets both.

| File | Read by | What |
|---|---|---|
| `*.vdptrace` | `tests/vdp_trace.c`, `examples/trace-player.c` | every VDP port access, DMA sources, and the state and picture at each frame |
| `*.padtrace` | `tests/pad_trace.c` | the I/O chip's accesses and the buttons held |
| `*.sndtrace` | `tests/psg_trace.c`, `fm_trace.c`, `mix_trace.c`, `examples/trace-player.c` | every sound chip write at its master clock, and the oracle's audio |

The exporters today are the first client's (`../nhl95/tools/oracle/vdptrace.py`, `padtrace.py`,
`sndtrace.py` over its oracle hook, `../nhl95/tools/hybrid/oracle_shim.c`); its
`tools/oracle/vectors.py` holds the recipe of each vector it records. The test ROM's own vector
(`vdptest.vdptrace`) is recorded from `build/vdptest.bin` (`tests/rom/build.sh`) the same way.

## Rules

- Integers are little-endian unless a field says otherwise.
- Each format starts with a four-byte magic and a version (u32). A reader accepts the versions
  it knows and refuses the rest with the version in its message. A change of format bumps the
  version, and the exporter, every reader and this file change in the same commit.
- Times are master clocks (53,693,175 Hz NTSC; 3,420 a line, 262 lines a frame).

## `.vdptrace` (MDVT, version 1)

Every VDP port access of the 68k in order, the bytes each DMA from the 68k bus reads, and at the
start of each vertical blank (the V counter reaching the active height) the VDP's state and the
frame just drawn.

```
"MDVT", version u32 = 1, region u8 (0 NTSC, 1 PAL), 3 reserved bytes, state
state:  regs[24], VRAM[65536] (VDP byte order), CRAM[64] u16 big-endian (bus words
        0000BBB0GGG0RRR0), VSRAM[40] u16 big-endian (11 bits)
events: tag u8, then
   1 CTRL16  u16          a move.w to the control port
   2 CTRL32  u32          a move.l to the control port
   3 DATA16  u16          a move.w to the data port
   4 DATA32  u32          a move.l to the data port
   5 READ16  u16          a move.w from the data port: the value the oracle returned
   6 STATUS  u16          a read of the status word: the value returned
   7 HV8     addr u8, value u8   a byte read of the HV counter ($C00008 or $C00009)
   8 HV16    u16          a word read of the HV counter
   9 DMASRC  n u32, n bytes      the 68k memory the DMA that the next CTRL starts reads, in order
  10 FRAME   state, width u16, height u16, width x height u16 colours (bus format)
  11 END
```

The replay applies the accesses in order, serves DMA reads from the recorded bytes, checks every
READ16 value, and at each FRAME checks the state byte for byte and the rendered picture pixel for
pixel. A trace begins after the program's set-up and only once a whole frame was drawn under
tracing (`HARDWARE.md`, 2026-10-15).

## `.padtrace` (MDPT, version 1)

```
"MDPT", version u32 = 1, the I/O chip's 16 registers at the start ($A10001 + 2 n: version, data
1-3, control 1-3, serial ...); then records: tag u8
   1 FRAME   buttons u16 port 1, buttons u16 port 2: held from here on (bits: 0 up, 1 down,
             2 left, 3 right, 4 B, 5 C, 6 A, 7 Start; mdrt/pad.h MDRT_PAD_*)
   2 WRITE   address u32, width u8, value u32   a write to $A10000-$A1001F
   3 READ    address u32, width u8, value u32   a read, and the value the oracle returned
   4 END
```

## `.sndtrace` (MDST, version 3)

```
"MDST", version u32 = 3, the oracle's audio rate u32 (Hz), its FM preamp u16 and PSG preamp
u16 (percent; Genesis Plus GX's defaults 100 and 150; 0 leaves that chip out of the audio),
flags u32 (bit 0: recorded from power on, so every write the chips had is in the trace;
otherwise they start in a state the trace does not hold; bit 1: the oracle's FM core was
Nuked-OPN2, not MAME's YM2612), Z80 RAM at the start [8192], the Z80 at the start: PC u16, HL
u16, C u8, bank u16 (the 68k address >> 15), state u8 (bit 0 running, bit 1 the bus taken by the
68k), time u32 (master clocks from the trace's start to where the Z80 is: the oracle runs it whole
instructions at a time, so it can be a little ahead);
then events: tag u8, time u32 (master clocks since the trace started, modulo 2^32: it wraps
after 4,793 frames; events come in time order give or take a few hundred clocks, so a reader
unwraps it), and
   1 YM       port u8 (0 / 1 address / data of part I, 2 / 3 of part II), value u8
   2 PSG      value u8                (a write to $C00011)
   3 Z80RAM   address u16, value u8   (a 68k byte write to $A00000-$A01FFF)
   4 BUSREQ   value u16   |   5 Z80RESET value u16
   6 AUDIO    n u32, n stereo frames of s16 (the oracle's output for the frame just run)
   7 END
```

The YM2612 writes are the Z80's and the 68k's together; the Z80 fields are for a client that
ports its Z80 program (the first client checks its relay against every write's time). The
runtime's chip tests take the writes and compare their audio with the oracle's by loudness and
spectrum in 50 ms windows, since the cores are different emulations.
