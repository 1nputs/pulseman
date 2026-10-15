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
