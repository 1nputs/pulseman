# Hardware notes

Behaviour the runtime implements, each with where it comes from: a document below, or a test
vector (the oracle, Genesis Plus GX running the real game; see WORKPLAN section 3). When a vector
and a document disagree the vector wins, and the disagreement is written down here.

## References

- Sega, *Genesis Software Manual* ("Sega2.doc") and the *Genesis Technical Bulletins*.
- Charles MacDonald, `genvdp.txt`.
- Plutiedev (plutiedev.com): VDP registers, DMA, sprites, PSG, YM2612, controllers.
- Sega Retro: Mega Drive VDP registers, memory map, I/O ports.
- SMS Power: SN76489 (the Mega Drive variant's noise taps and attenuation).
- Nemesis's YM2612 and VDP research (SpritesMind).

## Vectors

The formats are in `VECTORS.md`. Recorded by `../nhl95/tools/oracle/vdptrace.py` into
`$MDRT_VECTORS` (default `../nhl95/out/vectors`): `boot` (EA logo), `title`, `setup`,
`matchup`, `pregame` (front-end screens, from power on with Start presses), `faceoff`, `play`
(CPU against CPU), `pause` (the pause menu), and `vdptest` (the test ROM). Each is 90-250
frames.

Sound traces (`*.sndtrace`, `../nhl95/tools/oracle/sndtrace.py`, version 3): `game` (6000 frames from power on: EA logo,
title, menus, the faceoff, 40 s of play), `game_psg` and `game_fm` (the same with the FM chip,
or the PSG, left out of the oracle's audio: preamp 0), `game_fm_nuked` (`game_fm` with the oracle
on Nuked-OPN2 too), `boot` (3000 frames from power on),
`play` (3000 frames from a save state: its chips start in a state the trace does not hold, so
only the Z80 relay uses it). Times are master clocks modulo 2^32; readers unwrap them.

## Log

(dated entries: a quirk, the vector that showed it, what the runtime does)

- 2026-10-15, all vectors: CRAM and VSRAM reads return the colour / scroll bits with the bits the
  RAM does not drive taken from the oldest FIFO entry (the next one to be written); NHL '94's
  palette fade reads CRAM and the vectors' READ16 values match only with that.
- 2026-10-15, `faceoff`: a trace recorded straight after loading a save state carried a first
  picture partly drawn before the load; the exporter now emits a FRAME only after a whole frame
  was drawn under tracing.
- 2026-10-15, all vectors: a frame-granular renderer (the state at the start of the vertical
  blank, drawn whole) is pixel-exact for NHL '94: the game writes the VDP only in the vertical
  blank or with the display off.
- 2026-10-15, `vdptest` phase 3: vertical scroll per 2-cell column uses the plane's columns,
  which move with its fine horizontal scroll: column c covers x = (hscroll & 15) + 16c .. +15
  and takes VSRAM word 2c (A) or 2c + 1 (B). A partly shown column at the left edge takes, in
  H40, VSRAM word 38 AND word 39 for both planes; in H32 it is not scrolled vertically.
- 2026-10-15, `vdptest` phase 4: with the window on the left and plane A finely scrolled, the
  partly shown plane A column at the window's right edge (x = edge .. edge + fine - 1) shows the
  tiles of the plane A column after it (the "window bug").
- 2026-10-15, `vdptest`: a trace started while a DMA fill was still running in the oracle cannot
  be followed by an instant-DMA model; the vectors start after the program's set-up.
- 2026-10-15, PSG (SMS Power "SN76489", `game_psg`): the model follows the documentation: 16-bit
  noise register reset to $8000, white noise taps bits 0 and 3 (period 57337 shifts), the noise
  register shifting on its flip-flop's rising edge, 2 dB attenuation steps, tone periods 0 and 1
  a constant +1. Genesis Plus GX agrees except on periods 0 and 1, where it toggles every internal
  clock (an ultrasonic square that averages to half level); NHL '94 never sets them.
- 2026-10-15, `game_psg`: the oracle's PSG sound is band-limited by its resampler (blip_buf) and
  rolls off above about 5 kHz at 44100 Hz (-1.6 dB at 7.3 kHz, -2 dB at 11.6 kHz, -3.6 dB at 14.4
  kHz); the model's band-limited steps are flat to 17 kHz. The spectral comparison stops at
  5 kHz.
- 2026-10-15, `game`, `game_psg`, `game_fm`: the oracle's audio is exactly its FM output plus its
  PSG output (the three recordings agree sample for sample). Against ours in the 200 Hz - 5 kHz
  bands the oracle's PSG measures 0.3361 of the model's level and its FM 1.7731 of `mdrt_fm`'s,
  so the mixer takes the FM at 1.7708 and the PSG at 0.3357 (MDRT_MIX_FM_GAIN / PSG_GAIN): the
  full mix then measures 0.9998 of the oracle's, peaking at 25303 on `game`.
- 2026-10-15, FM: Nuked-OPN2 in its YM2612 mode models the discrete chip's DAC "ladder": idle
  slots output a step above zero, a DC offset (432 at `mdrt_fm`'s scale). The mixer's 10 Hz
  high-pass removes it, as the console's output capacitor does.
- 2026-10-15, `game_fm`: the oracle's FM core (MAME's YM2612 in GPGX) and Nuked-OPN2 agree within
  1 dB of loudness in 1299 of 1305 windows; ymfm agrees with the oracle in 1304. Genesis Plus GX
  resets the FM chip when the Z80's reset is asserted and again when it is released.
- 2026-10-16, the composition's default HV counter: the beam it models is shown through the VDP's
  counter mapping (`mdrt_vdp_hv_at`; the V counter jumps from $EA back to $E5 at line 235 in
  NTSC, the H counter from $B6 to $E4 in H40, $93 to $E9 in H32). Before, lines 235-261 read as
  $EB-$FF and 262 on as $00-$05, values the counter never shows there. The first client reads the
  counter as a random source, so its native random numbers change; its lockstep replays the
  oracle's values and is unaffected.
- 2026-10-16, PAL: the master clock is 53,203,424 Hz (the PAL colour subcarrier, 4.43361875 MHz, x 12),
  against NTSC's 53,693,175; the dividers (68k /7, Z80 and PSG /15, YM2612 /7/144) are the same,
  so a PAL console's chips run 0.9% slower and its 313-line frame lasts 1,070,460 master clocks.
  The mixer resamples from the configured clock and the SDL2 platform paces by it. No PAL vector
  exists yet.
