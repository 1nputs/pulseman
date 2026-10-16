; vdptest: a Mega Drive program that exercises the VDP features the renderer must get right and
; NHL '94 does not use, for the WP2 "second program" test vector. Ten phases of 30 frames:
;   0 H40, 128x32 planes, full-screen scroll       5 window above / below a row
;   1 per-line horizontal scroll                    6 H32, 64x64 planes, DMA fill of plane B
;   2 per-cell horizontal scroll                    7 H40, 32x128 planes, VRAM copy of tiles
;   3 per-2-cell-column vertical scroll             8 H-interrupt every 16 lines: the backdrop,
;   4 window left / right of a column                 plane A's scroll and plane B's vertical scroll
;                                                    change down the screen
;                                                   9 the HV counter polled for lines 96 and 160:
;                                                     the backdrop, a scroll and a colour changed there
; Always: 80 linked sprites of every size, flip, palette and priority, moving; 22 sprites on
; line 60 (the 20 per line limit) and a sprite at X 0 on line 150 that masks the ones after it.
; Assemble: vasmm68k_mot -Fbin -o vdptest.bin vdptest.s   (tests/rom/build.sh)

VDP_DATA        equ     $C00000
VDP_CTRL        equ     $C00004
VDP_HV          equ     $C00008
PLANE_A         equ     $C000
PLANE_B         equ     $E000
WINDOW          equ     $B000
SPRITES         equ     $A800
HSCROLL         equ     $AC00

RAM             equ     $FF0000
frame           equ     RAM+0           ; word: frames since start
vbl             equ     RAM+2           ; word: set by the vblank handler
phase           equ     RAM+4           ; word
hband           equ     RAM+6           ; word: H-interrupts since the vblank
hstab           equ     RAM+$100        ; 240 lines x 2 words
sattab          equ     RAM+$1000       ; 80 x 8 bytes

; set the VDP to write VRAM at \1 (with DMA: _DMA)
VRAMW_D         macro
                move.l  #($40000000|((\1&$3FFF)<<16)|(\1>>14)),(a5)
                endm
VRAMW_DMA       macro
                move.l  #($40000080|((\1&$3FFF)<<16)|(\1>>14)),(a5)
                endm
; DMA from 68k address \1, \2 words
DMA_SRC         macro
                move.w  #$9300+((\2)&$FF),(a5)
                move.w  #$9400+((\2)>>8),(a5)
                move.w  #$9500+(((\1)>>1)&$FF),(a5)
                move.w  #$9600+(((\1)>>9)&$FF),(a5)
                move.w  #$9700+(((\1)>>17)&$7F),(a5)
                endm

                org     0
                dc.l    $00FFFE00, start
                dcb.l   22, trap_stop
                dc.l    trap_stop, trap_stop, trap_stop, trap_stop, hblank, trap_stop, vblank, trap_stop
                dcb.l   32, trap_stop

                org     $100
                dc.b    "SEGA MEGA DRIVE "
                dc.b    "(C)MDRT 2026.OCT"
                dc.b    "VDP TEST                                        "
                dc.b    "VDP TEST                                        "
                dc.b    "GM 00000000-00"
                dc.w    0
                dc.b    "J               "
                dc.l    0, $3FFFF, $FF0000, $FFFFFF
                dc.b    "            "
                dc.b    "            "
                dc.b    "                                        "
                dc.b    "JUE             "

                org     $200
trap_stop:      bra.s   trap_stop
rte_only:       rte

start:          move    #$2700,sr
                move.b  ($A10001),d0            ; TMSS on later models
                andi.b  #$0F,d0
                beq.s   .no_tmss
                move.l  #'SEGA',($A14000)
.no_tmss:       lea     VDP_CTRL,a5
                lea     VDP_DATA,a6
                tst.w   (a5)
                lea     regs(pc),a0
                moveq   #regs_end-regs-1,d0
                lsr.w   #1,d0
.regs:          move.w  (a0)+,(a5)
                dbf     d0,.regs
                ; clear RAM
                lea     RAM,a0
                move.w  #$3FFF,d0
.clr:           clr.l   (a0)+
                dbf     d0,.clr
                ; clear VRAM by DMA fill: 64 KB of 0
                move.w  #$8F01,(a5)
                move.l  #$93FF94FF,(a5)
                move.w  #$9780,(a5)
                move.l  #$40000080,(a5)
                move.w  #0,(a6)
.wfill:         move.w  (a5),d0
                btst    #1,d0
                bne.s   .wfill
                move.w  #$8F02,(a5)
                ; colours: colour i = (i*5 & 7) red, (i*3 & 7) green, (i & 7) blue
                move.l  #$C0000000,(a5)
                moveq   #0,d1
.cram:          move.w  d1,d2
                mulu.w  #5,d2
                andi.w  #7,d2
                add.w   d2,d2
                move.w  d1,d3
                mulu.w  #3,d3
                andi.w  #7,d3
                lsl.w   #5,d3
                or.w    d3,d2
                move.w  d1,d3
                andi.w  #7,d3
                lsl.w   #8,d3
                add.w   d3,d3
                or.w    d3,d2
                move.w  d2,(a6)
                addq.w  #1,d1
                cmpi.w  #64,d1
                bne.s   .cram
                ; tiles 1-15: pixel x, y of tile n = (x + 2y + n) & 15
                VRAMW_D $20
                moveq   #1,d1                   ; tile
.tile:          moveq   #0,d2                   ; row
.row:           moveq   #0,d3                   ; column pair
                moveq   #0,d5
.pix:           move.w  d3,d4
                add.w   d2,d4
                add.w   d2,d4
                add.w   d1,d4
                andi.w  #15,d4
                lsl.l   #4,d5
                or.w    d4,d5
                addq.w  #1,d3
                cmpi.w  #8,d3
                bne.s   .pix
                move.l  d5,(a6)
                addq.w  #1,d2
                cmpi.w  #8,d2
                bne.s   .row
                addq.w  #1,d1
                cmpi.w  #16,d1
                bne.s   .tile
                ; plane A (128 x 32): tile 1 + (cx + cy) % 15, palette cx / 4, h flip odd cx, v flip odd cy, priority bit 2 of cx + cy
                VRAMW_D PLANE_A
                moveq   #0,d2                   ; cy
.pa_row:        moveq   #0,d1                   ; cx
.pa_cell:       move.w  d1,d3
                add.w   d2,d3
                move.w  d3,d4
                ext.l   d3
                divu.w  #15,d3
                swap    d3
                addq.w  #1,d3                   ; tile
                move.w  d1,d0
                lsr.w   #2,d0
                andi.w  #3,d0
                ror.w   #3,d0                   ; palette << 13
                or.w    d0,d3
                btst    #0,d1
                beq.s   .pa_nh
                bset    #11,d3
.pa_nh:         btst    #0,d2
                beq.s   .pa_nv
                bset    #12,d3
.pa_nv:         btst    #2,d4
                beq.s   .pa_np
                bset    #15,d3
.pa_np:         move.w  d3,(a6)
                addq.w  #1,d1
                cmpi.w  #128,d1
                bne.s   .pa_cell
                addq.w  #1,d2
                cmpi.w  #32,d2
                bne.s   .pa_row
                ; plane B (128 x 32): tile 1 + (3 cx + cy) % 15, palette (cy / 2) & 3
                VRAMW_D PLANE_B
                moveq   #0,d2
.pb_row:        moveq   #0,d1
.pb_cell:       move.w  d1,d3
                add.w   d1,d3
                add.w   d1,d3
                add.w   d2,d3
                ext.l   d3
                divu.w  #15,d3
                swap    d3
                addq.w  #1,d3
                move.w  d2,d0
                lsr.w   #1,d0
                andi.w  #3,d0
                ror.w   #3,d0
                or.w    d0,d3
                move.w  d3,(a6)
                addq.w  #1,d1
                cmpi.w  #128,d1
                bne.s   .pb_cell
                addq.w  #1,d2
                cmpi.w  #32,d2
                bne.s   .pb_row
                ; window (64 x 32): tile 7 palette 2, priority on the left half
                VRAMW_D WINDOW
                move.w  #64*32-1,d0
                moveq   #0,d1
.win:           move.w  #$4007,d3
                btst    #5,d1
                bne.s   .win_lo
                bset    #15,d3
.win_lo:        move.w  d3,(a6)
                addq.w  #1,d1
                dbf     d0,.win
                ; sprites: entry i: y 20 + 13 i mod 200, size i & 15, link i + 1, tile 1 + i mod 15 with palette i & 3,
                ; h flip bit 2, v flip bit 3, priority bit 4 of i; x 10 + 37 i mod 300
                lea     sattab,a0
                moveq   #0,d1
.spr:           move.w  d1,d2
                mulu.w  #13,d2
                divu.w  #200,d2
                swap    d2
                addi.w  #20+128,d2
                move.w  d2,(a0)+
                move.w  d1,d2
                andi.w  #15,d2
                lsl.w   #8,d2
                move.w  d1,d3
                addq.w  #1,d3
                cmpi.w  #80,d3
                bne.s   .spr_l
                moveq   #0,d3
.spr_l:         or.w    d3,d2
                move.w  d2,(a0)+
                move.w  d1,d2
                ext.l   d2
                divu.w  #15,d2
                swap    d2
                addq.w  #1,d2
                move.w  d1,d3
                andi.w  #3,d3
                ror.w   #3,d3
                or.w    d3,d2
                btst    #2,d1
                beq.s   .spr_h
                bset    #11,d2
.spr_h:         btst    #3,d1
                beq.s   .spr_v
                bset    #12,d2
.spr_v:         btst    #4,d1
                beq.s   .spr_p
                bset    #15,d2
.spr_p:         move.w  d2,(a0)+
                move.w  d1,d2
                mulu.w  #37,d2
                divu.w  #300,d2
                swap    d2
                addi.w  #10+128,d2
                move.w  d2,(a0)+
                addq.w  #1,d1
                cmpi.w  #80,d1
                bne     .spr
                ; sprites 30-51: 1x1 on line 60, x = 14 (i - 30): 22 on one line
                lea     sattab+30*8,a0
                moveq   #0,d1
.line60:        move.w  #60+128,(a0)
                andi.w  #$00FF,2(a0)            ; size 1x1, keep the link
                move.w  d1,d2
                mulu.w  #14,d2
                addi.w  #128,d2
                move.w  d2,6(a0)
                addq.w  #8,a0
                addq.w  #1,d1
                cmpi.w  #22,d1
                bne.s   .line60
                ; sprites 60-62 on line 150: x 40, raw x 0 (masks), x 100
                lea     sattab+60*8,a0
                move.w  #150+128,(a0)
                move.w  #40+128,6(a0)
                move.w  #150+128,8(a0)
                move.w  #0,14(a0)
                move.w  #150+128,16(a0)
                move.w  #100+128,22(a0)
                ; go
                move.w  #$8174,(a5)             ; display, vint, DMA, mode 5
                move    #$2300,sr               ; (the H-interrupt is level 4)
main:           tst.w   (vbl)
                beq.s   main
                clr.w   (vbl)
                bsr     update
                cmpi.w  #9,(phase)
                bne.s   main
                ; phase 9: at line 96 the backdrop to colour $2A and plane A 40 lines further down; at
                ; line 160 colour 1 to white and the backdrop to colour 17
.w96:           cmpi.b  #96,(VDP_HV)
                bne.s   .w96
                move.w  #$872A,(a5)
                move.l  #$40000010,(a5)
                move.w  (frame),d0
                addi.w  #40,d0
                move.w  d0,(a6)
.w160:          cmpi.b  #160,(VDP_HV)
                bne.s   .w160
                move.l  #$C0020000,(a5)
                move.w  #$0EEE,(a6)
                move.w  #$8711,(a5)
                bra.s   main

; per frame: the scroll tables in RAM and the moving sprites, for the next vblank to send
update:         move.w  (frame),d7
                move.w  d7,d0
                ext.l   d0
                divu.w  #30,d0
                andi.l  #$FFFF,d0
                divu.w  #10,d0
                swap    d0
                move.w  d0,(phase)
                ; horizontal scroll table: line l: A = frame + l * phase, B = -frame / 2 + l / 4
                lea     hstab,a0
                moveq   #0,d1
.hs:            move.w  d1,d2
                mulu.w  (phase),d2
                add.w   d7,d2
                move.w  d2,(a0)+
                move.w  d7,d2
                asr.w   #1,d2
                neg.w   d2
                move.w  d1,d3
                lsr.w   #2,d3
                add.w   d3,d2
                move.w  d2,(a0)+
                addq.w  #1,d1
                cmpi.w  #240,d1
                bne.s   .hs
                ; sprites 0-29 and 63-79 move right one pixel a frame
                lea     sattab+6,a0
                moveq   #29,d0
.mv:            addq.w  #1,(a0)
                andi.w  #$1FF,(a0)
                addq.w  #8,a0
                dbf     d0,.mv
                lea     sattab+63*8+6,a0
                moveq   #79-63,d0
.mv2:           addq.w  #1,(a0)
                andi.w  #$1FF,(a0)
                addq.w  #8,a0
                dbf     d0,.mv2
                rts

; the vblank: registers for the phase, VSRAM, the scroll table and the sprites by DMA
vblank:         movem.l d0-d7/a0-a6,-(sp)
                lea     VDP_CTRL,a5
                lea     VDP_DATA,a6
                move.w  (phase),d0
                add.w   d0,d0
                move.w  .modes(pc,d0.w),d1      ; R11 high byte, R12 low byte
                move.w  d1,d2
                lsr.w   #8,d2
                ori.w   #$8B00,d2
                move.w  d2,(a5)
                andi.w  #$FF,d1
                ori.w   #$8C00,d1
                move.w  d1,(a5)
                move.w  .sizes(pc,d0.w),(a5)    ; R16
                move.w  .winx(pc,d0.w),(a5)     ; R17
                move.w  .winy(pc,d0.w),(a5)     ; R18
                move.w  .hint(pc,d0.w),(a5)     ; R0: the H-interrupt in phase 8
                move.w  #$8A0F,(a5)             ; R10: every 16 lines
                move.w  #$873F,(a5)             ; R7: the backdrop back (phases 8, 9 change it)
                move.l  #$C0020000,(a5)         ; colour 1 back (phase 9 changes it)
                move.w  #$026A,(a6)
                clr.w   (hband)
                bra.s   .vs
.modes:         dc.w    $0081, $0381, $0281, $0481, $0081, $0081, $0000, $0081, $0081, $0081
.sizes:         dc.w    $9003, $9003, $9003, $9003, $9003, $9003, $9011, $9030, $9003, $9003
.winx:          dc.w    $9100, $9100, $9100, $9100, $9108, $9188, $9100, $9100, $9100, $9100
.winy:          dc.w    $9200, $9200, $9200, $9200, $9200, $9200, $9205, $9285, $9200, $9200
.hint:          dc.w    $8004, $8004, $8004, $8004, $8004, $8004, $8004, $8004, $8014, $8004
.vs:            ; VSRAM: A = frame, B = 0; in phase 3 column c: c * frame / 8
                move.l  #$40000010,(a5)
                moveq   #0,d1
.vsl:           move.w  d1,d2
                lsr.w   #1,d2
                mulu.w  (frame),d2
                lsr.w   #3,d2
                cmpi.w  #3,(phase)
                beq.s   .vsw
                moveq   #0,d2
                btst    #0,d1
                bne.s   .vsw
                move.w  (frame),d2
.vsw:           move.w  d2,(a6)
                addq.w  #1,d1
                cmpi.w  #40,d1
                bne.s   .vsl
                ; scroll table: 240 x 2 words to HSCROLL
                DMA_SRC hstab,480
                VRAMW_DMA HSCROLL
                ; sprite table: 320 words to SPRITES
                DMA_SRC sattab,320
                VRAMW_DMA SPRITES
                ; phase 6, first frame: fill plane B's first 2 KB with $11; phase 7 first frame: copy tiles 1-15 over 17-31
                move.w  (frame),d0
                ext.l   d0
                divu.w  #30,d0
                swap    d0
                tst.w   d0
                bne.s   .done
                cmpi.w  #6,(phase)
                bne.s   .copy
                move.w  #$8F01,(a5)
                move.l  #$93FF9407,(a5)         ; $800 bytes
                move.w  #$9780,(a5)
                VRAMW_DMA PLANE_B
                move.w  #$1111,(a6)
.wf:            move.w  (a5),d0                 ; wait for the fill (DMA busy, status bit 1)
                btst    #1,d0
                bne.s   .wf
                move.w  #$8F02,(a5)
                bra.s   .done
.copy:          cmpi.w  #7,(phase)
                bne.s   .done
                move.w  #$8F01,(a5)
                move.l  #$93E09401,(a5)         ; $1E0 bytes
                move.l  #$95209600,(a5)         ; from $0020
                move.w  #$97C0,(a5)
                move.l  #$022000C0,(a5)         ; to $0220, VRAM copy
.wc:            move.w  (a5),d0
                btst    #1,d0
                bne.s   .wc
                move.w  #$8F02,(a5)
.done:          addq.w  #1,(frame)
                move.w  #1,(vbl)
                movem.l (sp)+,d0-d7/a0-a6
                rte

; the H-interrupt (phase 8): band n (from 1) sets the backdrop to colour 5n mod 64, plane A's
; full-screen horizontal scroll to 12n + frame and plane B's vertical scroll to 6n
hblank:         movem.l d0/a5-a6,-(sp)
                lea     VDP_CTRL,a5
                lea     VDP_DATA,a6
                addq.w  #1,(hband)
                move.w  (hband),d0
                mulu.w  #5,d0
                andi.w  #$3F,d0
                ori.w   #$8700,d0
                move.w  d0,(a5)
                move.l  #$6C000002,(a5)         ; VRAM $AC00: the scroll table's first line
                move.w  (hband),d0
                mulu.w  #12,d0
                add.w   (frame),d0
                move.w  d0,(a6)
                move.l  #$40020010,(a5)         ; VSRAM word 1: plane B
                move.w  (hband),d0
                mulu.w  #6,d0
                move.w  d0,(a6)
                movem.l (sp)+,d0/a5-a6
                rte

regs:           dc.w    $8004, $8134, $8200|(PLANE_A>>10), $8300|(WINDOW>>10), $8400|(PLANE_B>>13)
                dc.w    $8500|(SPRITES>>9), $8600, $8700|$3F, $8800, $8900, $8AFF, $8B00, $8C81
                dc.w    $8D00|(HSCROLL>>10), $8E00, $8F02, $9003, $9100, $9200
regs_end:
