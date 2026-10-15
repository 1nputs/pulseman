# Using md-runtime from a client

A client is a game reimplemented in C whose hardware accesses were 68k bus accesses. It links
`build/release/libmdrt.a` (and `libmdrt_sdl2.a` for a window), includes `mdrt/mdrt.h` (and
`mdrt/platform_sdl2.h`), and maps its hardware calls onto the runtime. The first client,
NHL '94 (`../nhl95`), does it in `native/hal_native.c` (about a hundred lines) and
`native/main.c`; read those as the reference.

## Starting

```c
mdrt_config c = { 0 };
c.audio_rate = 48000;                 /* output rate */
c.sram_size = 0x2000;                 /* cartridge save RAM bytes */
c.read68k = read68k;                  /* the byte at a 68k address, for DMA (ROM, work RAM) */
mdrt_sdl_options o = { "Title", 3, 0, "save.srm" };
return mdrt_sdl_run(&c, &o, game, NULL);   /* game(m, user) runs on the game thread */
```

Without a window, fill `c.platform` (present, audio, wait, input: any may be NULL), create the
console with `mdrt_new(&c)` and call the game yourself.

## The bus

| 68k access | runtime |
|---|---|
| move to / from $C00000 / $C00004 / $C00008 | `mdrt_vdp_data16/32`, `mdrt_vdp_ctrl16/32`, `mdrt_vdp_read16`, `mdrt_vdp_status`, `mdrt_vdp_hv8/16` on `mdrt_get_vdp(m)` |
| move.b to $C00011 (PSG) | `mdrt_psg_port(m, at, value)` |
| $A10003 / $A10005 (pads) | `mdrt_pad_read/write(mdrt_get_pads(m), addr, ...)` |
| $200000 save RAM | `mdrt_sram_read8/write8(mdrt_get_sram(m), off, ...)` (odd offsets) |
| waiting for the vertical blank | `mdrt_wait_vblank(m)`: nonzero means stop |

The VDP state can be set whole (`mdrt_vdp_set_state`) to start from a snapshot.

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
game polls. NHL '94: `src/z80.c` (the relay), 56 master clocks a Z80-bus access.

## The handshake

`mdrt_wait_vblank(m)` draws the picture, makes the frame's sound, hands both to the platform,
waits for its go (the SDL2 platform paces by the sound queue), and reads the pads. On return the
VDP is in its vertical blank (status bit 3): run the vblank interrupt's code, then call
`mdrt_vblank_end(m)`.

## Lockstep

`mdrt_set_hv_source(m, fn, user)` and `mdrt_set_pad_source(m, fn, user)` replace the HV counter
model and the platform's buttons with recorded values, so a run can follow the oracle's.
