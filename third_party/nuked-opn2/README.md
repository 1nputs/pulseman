# Nuked-OPN2

Yamaha YM3438 / YM2612 emulator by Alexey Khokholov (Nuke.YKT), version 1.0.9, from
https://github.com/nukeykt/Nuked-OPN2 at commit 335747d78cb0abbc3b55b004e62dad9763140115
(2023-08-11). Licensed under the GNU Lesser General Public License 2.1 or later (LICENSE).
md-runtime uses them through `src/fm/nuked.c` only, so the core can be swapped (`make
FM_CORE=ymfm`) or relinked as the LGPL requires.

Changed from the published files (2026-10-16, marked "md-runtime" in them): the chip type is a
field of `ym3438_t` (`chip_type`, kept across `OPN2_Reset`) instead of a static shared by every
chip, and `OPN2_SetChipType` takes the chip. Two consoles in one process can then be different
models, and a chip's whole state is its struct.
