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

Recorded by `../nhl95/tools/oracle/vdptrace.py` into `$MDRT_VECTORS` (default
`../nhl95/out/vectors`): `boot` (EA logo), `title`, `setup`, `matchup`, `pregame` (front-end
screens, from power on with Start presses), `faceoff`, `play` (CPU against CPU), `pause` (the
pause menu). Each is 90-150 frames.

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

