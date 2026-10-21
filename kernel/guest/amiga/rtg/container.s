.include "p96offsets.inc"
.include "rtgabi.inc"
.equ DO_STATE_TEXT,1

.equ LIB_FLAGS, 14
.equ LIB_NEGSIZE, 16
.equ LIB_POSSIZE, 18
.equ LIB_OPENCNT, 32
.equ lib_exec, 36
.equ lib_segment, 44
.equ lib_name, 48
.equ lib_claimed, 52
.equ lib_alloc, 56
.equ lib_free, 60
| padded screens: address given out, bytes of black rows before it
.equ lib_pads, 64
.equ NPADS, 8
.equ lib_size, lib_pads+NPADS*8
.equ state_switch, gbi_CardData
.equ state_display, gbi_CardData+4
.equ state_format, gbi_CardData+8
.equ state_vsync, gbi_CardData+12
| the last SetPanning: memory, width, x, y
.equ state_panmem, gbi_CardData+16
.equ state_panw, gbi_CardData+20
.equ state_panx, gbi_CardData+24
.equ state_pany, gbi_CardData+28
.equ state_lib, gbi_CardData+32

.text
.globl _start
_start:
 moveq #-1,%d0
 rts
resident:
 .word 0x4afc
 .long resident, code_end
 .byte 0x80, 1, 9, 0
 .long library_name, id_string, init_table
library_name: .asciz "container.card"
board_name: .asciz "Amiga Container RTG"
id_string: .asciz "container.card 1.0 (03.10.2026)"
 .asciz "$VER: container.card 1.0 (03.10.2026)"
 .balign 2
init_table:
 .long lib_size, vectors, 0, library_init
vectors:
 .long library_open, library_close, library_expunge, return_zero
 .long find_card, init_card, -1
library_init:
 move.l %d0,%a1
 move.l %a6,lib_exec(%a1)
 move.l %a0,lib_segment(%a1)
 move.l #board_name,lib_name(%a1)
 move.b #9,8(%a1)
 move.l #library_name,10(%a1)
 move.b #6,LIB_FLAGS(%a1)
 move.w #1,20(%a1)
 clr.w 22(%a1)
 move.l #id_string,24(%a1)
 rts
library_open:
 addq.w #1,LIB_OPENCNT(%a6)
 bclr #3,LIB_FLAGS(%a6)
 move.l %a6,%d0
 rts
library_close:
 subq.w #1,LIB_OPENCNT(%a6)
 bne return_zero
 btst #3,LIB_FLAGS(%a6)
 bne library_expunge
return_zero:
 moveq #0,%d0
 rts
library_expunge:
 tst.w LIB_OPENCNT(%a6)
 bne defer_expunge
 tst.l lib_claimed(%a6)
 bne defer_expunge
 movem.l %d2/%a5-%a6,-(%sp)
 move.l %a6,%a5
 move.l lib_exec(%a5),%a6
 move.l lib_segment(%a5),%d2
 move.l %a5,%a1
 jsr -252(%a6)
 moveq #0,%d0
 move.w LIB_NEGSIZE(%a5),%d0
 move.l %a5,%a1
 sub.l %d0,%a1
 add.w LIB_POSSIZE(%a5),%d0
 jsr -210(%a6)
 move.l %d2,%d0
 movem.l (%sp)+,%d2/%a5-%a6
 rts
defer_expunge:
 bset #3,LIB_FLAGS(%a6)
 bra return_zero

find_card:
 tst.l lib_claimed(%a6)
 bne return_zero
 move.l #RTG_BASE,%a1
 cmp.l #RTG_MAGIC,rtg_magic(%a1)
 bne return_zero
 cmp.l #RTG_VERSION,rtg_version(%a1)
 bne return_zero
 cmp.l #RTG_HEADER_SIZE,rtg_header_size(%a1)
 bne return_zero
 move.l #1,rtg_card(%a1)
 btst #1,rtg_formats+3(%a1)
 beq 9f
 move.l #3,rtg_card(%a1)
 move.l #1,lib_claimed(%a6)
 move.l %a1,gbi_RegisterBase(%a0)
 | the display session's memory, mapped by the launcher
 move.l rtg_vram(%a1),gbi_MemoryBase(%a0)
 move.l rtg_memory_size(%a1),gbi_MemorySize(%a0)
 move.l rtg_vram(%a1),gbi_MemorySpaceBase(%a0)
 move.l rtg_memory_size(%a1),gbi_MemorySpaceSize(%a0)
 moveq #1,%d0
 rts
9: move.l #2,rtg_card(%a1)
 bra return_zero

.macro callback field, target
 move.l #\target,\field(%a0)
.endm
| our function, with P96's own kept as the fallback for formats and masks
.macro drawfn field, fallback, target
 tst.l \fallback(%a0)
 bne 1f
 move.l \field(%a0),\fallback(%a0)
1: move.l #\target,\field(%a0)
.endm
init_card:
 move.l #4,RTG_BASE+rtg_card
 | MOVE16 rows on a 68040 or 68060 (AttnFlags)
 move.l 4,%a1
 moveq #30,%d1
 move.w 296(%a1),%d0
 and.w #0x88,%d0
 beq 1f
 moveq #40,%d1
1: move.l %d1,-(%sp)
 bsr do_init
 addq.l #4,%sp
 move.l #board_name,gbi_BoardName(%a0)
 clr.l gbi_BoardType(%a0)
 clr.l gbi_PaletteChipType(%a0)
 clr.l gbi_GraphicsControllerType(%a0)
 move.w #8,gbi_BitsPerCannon(%a0)
 | In the display chain, direct access, own drawing functions without masks
 or.l #0x04508000,gbi_Flags(%a0)
 and.l #~0x01000000,gbi_Flags(%a0)
 move.l #RTG_BASE,%a1
 move.w rtg_formats+2(%a1),gbi_RGBFormats(%a0)
 move.w rtg_formats+2(%a1),gbi_SoftSpriteFlags(%a0)
 | The host draws the pointer on CLUT screens when it offers to: a hardware sprite.
 tst.l rtg_cursor(%a1)
 beq 3f
 or.l #1,gbi_Flags(%a0)
 and.w #~2,gbi_SoftSpriteFlags(%a0)
3:
 move.l rtg_max_width(%a1),%d0
 cmp.l #4096,%d0
 bls 1f
 move.l #4096,%d0
1: move.l rtg_max_height(%a1),%d1
 cmp.l #4096,%d1
 bls 2f
 move.l #4096,%d1
 | the same limits for CLUT, hi-colour, true-colour and true-alpha modes
2: move.l %d2,-(%sp)
 moveq #2,%d2
4: move.w #8192,gbi_MaxHorValue(%a0,%d2.l)
 move.w #8192,gbi_MaxVerValue(%a0,%d2.l)
 move.w %d0,gbi_MaxHorResolution(%a0,%d2.l)
 move.w %d1,gbi_MaxVerResolution(%a0,%d2.l)
 move.l #4,gbi_PixelClockCount(%a0,%d2.l*2)
 addq.l #2,%d2
 cmp.l #10,%d2
 blo 4b
 move.l (%sp)+,%d2
 clr.l state_switch(%a0)
 move.l #1,state_display(%a0)
 move.l #1,state_format(%a0)
 clr.l state_panmem(%a0)
 callback gbi_SetSwitch, set_switch
 callback gbi_SetColorArray, set_palette
 callback gbi_SetDAC, set_dac
 callback gbi_SetGC, set_gc
 callback gbi_SetPanning, set_panning
 callback gbi_CalculateBytesPerRow, bytes_per_row
 callback gbi_CalculateMemory, calculate_memory
 callback gbi_GetCompatibleFormats, compatible_formats
 callback gbi_SetDisplay, set_display
 callback gbi_ResolvePixelClock, resolve_clock
 callback gbi_GetPixelClock, get_clock
 callback gbi_SetClock, noop
 callback gbi_SetMemoryMode, noop
 callback gbi_SetWriteMask, noop
 callback gbi_SetClearMask, noop
 callback gbi_SetReadPlane, noop
 callback gbi_WaitVerticalSync, wait_vsync
 callback gbi_GetVSyncState, vsync_state
 callback gbi_SetInterrupt, return_zero
 callback gbi_WaitBlitter, noop
 callback gbi_SetSprite, set_sprite
 callback gbi_SetSpritePosition, set_sprite_position
 callback gbi_SetSpriteImage, set_sprite_image
 callback gbi_SetSpriteColor, set_sprite_color
 | P96's allocator under ours, kept once
 move.l %a6,state_lib(%a0)
 cmp.l #alloc_mem,gbi_AllocCardMem(%a0)
 beq 1f
 move.l gbi_AllocCardMem(%a0),lib_alloc(%a6)
 move.l gbi_FreeCardMem(%a0),lib_free(%a6)
 callback gbi_AllocCardMem, alloc_mem
 callback gbi_FreeCardMem, free_mem
1:
 drawfn gbi_FillRect, gbi_FillRectDefault, fill_rect
 drawfn gbi_InvertRect, gbi_InvertRectDefault, invert_rect
 drawfn gbi_BlitRect, gbi_BlitRectDefault, blit_rect
 drawfn gbi_BlitTemplate, gbi_BlitTemplateDefault, blit_template
 drawfn gbi_BlitRectNoMaskComplete, gbi_BlitRectNoMaskCompleteDefault, blit_complete
 move.l #5,RTG_BASE+rtg_card
 moveq #1,%d0
 rts
| d0 pixels wide: the rows of the smaller mode that wide in a1, else 0 (flags set)
small_rows:
 move.l %a0,-(%sp)
 lea RTG_BASE+rtg_small,%a0
 suba.l %a1,%a1
1: tst.l (%a0)
 beq 3f
 cmp.l (%a0),%d0
 beq 2f
 addq.l #8,%a0
 cmp.l #RTG_BASE+rtg_small+32,%a0
 blo 1b
 bra 3f
2: move.l 4(%a0),%a1
3: move.l (%sp)+,%a0
 cmpa.w #0,%a1
 rts
noop:
 rts
| d0 bytes.  A screen of a smaller mode, its rows the display's, gets
| black rows, half the display's spare height, above and below, so the
| display shows it centred with nothing else around it.
alloc_mem:
 movem.l %d1-%d7/%a1-%a3,-(%sp)
 move.l %d0,%d4
 move.l %d0,%d5
 and.l #-16,%d5
 moveq #0,%d6
 lea RTG_BASE+rtg_small,%a2
1: tst.l (%a2)
 beq 5f
 | its height in rows of the display's width, 1 to 4 bytes a pixel
 moveq #1,%d1
2: move.l RTG_BASE+rtg_max_width,%d2
 mulu.l %d1,%d2
 move.l %d2,%d3
 mulu.l 4(%a2),%d3
 cmp.l %d3,%d5
 beq 3f
 addq.l #1,%d1
 cmp.l #4,%d1
 bls 2b
 addq.l #8,%a2
 cmp.l #RTG_BASE+rtg_small+32,%a2
 blo 1b
 bra 5f
3: move.l RTG_BASE+rtg_max_height,%d6
 sub.l 4(%a2),%d6
 bls 4f
 lsr.l #1,%d6
 mulu.l %d2,%d6
 | a free slot, else no border
 move.l state_lib(%a0),%a3
 lea lib_pads(%a3),%a2
 moveq #NPADS-1,%d0
6: tst.l (%a2)
 beq 5f
 addq.l #8,%a2
 dbra %d0,6b
4: moveq #0,%d6
5: move.l %d4,%d0
 add.l %d6,%d0
 add.l %d6,%d0
 move.l %d0,%d5
 move.l state_lib(%a0),%a3
 move.l lib_alloc(%a3),%a3
 movem.l (%sp),%d1-%d3
 move.l 24(%sp),%d7
 move.l 28(%sp),%a1
 jsr (%a3)
 tst.l %d6
 beq 9f
 tst.l %d0
 beq 9f
 | all of it black, the screen's own row ends too
 move.l %d0,%a3
 lsr.l #2,%d5
7: clr.l (%a3)+
 subq.l #1,%d5
 bne 7b
 add.l %d6,%d0
 move.l %d0,(%a2)
 move.l %d6,4(%a2)
9: movem.l (%sp)+,%d1-%d7/%a1-%a3
 rts
| a1 the memory: a padded screen's own block goes back
free_mem:
 move.l %a2,-(%sp)
 move.l state_lib(%a0),%a2
 move.l lib_free(%a2),-(%sp)
 lea lib_pads(%a2),%a2
 moveq #NPADS-1,%d0
1: cmpa.l (%a2),%a1
 beq 2f
 addq.l #8,%a2
 dbra %d0,1b
 bra 3f
2: suba.l 4(%a2),%a1
 clr.l (%a2)
3: move.l 4(%sp),%a2
 move.l (%sp),4(%sp)
 addq.l #4,%sp
 rts

| P96 serializes board callbacks; each publishes one complete control update.
set_switch:
 move.l state_switch(%a0),%d1
 tst.w %d0
 sne %d0
 and.l #1,%d0
 move.l %d0,state_switch(%a0)
 move.w %d0,gbi_MoniSwitch(%a0)
 bsr publish_on
 move.l %d1,%d0
 rts
set_display:
 move.l state_display(%a0),%d1
 tst.w %d0
 sne %d0
 and.l #1,%d0
 move.l %d0,state_display(%a0)
 bsr publish_on
 move.l %d1,%d0
 rts
| the screen's rows follow its format, whichever of SetDAC and SetPanning comes last
set_dac:
 move.l %d7,state_format(%a0)
 move.l %d7,gbi_RGBFormat(%a0)
 bsr publish_pan
 bra publish_on
| shown, blank, or native when switched off; a format the host lacks stays blank
publish_on:
 move.l %d1,-(%sp)
 move.l #RTG_BASE,%a1
 addq.l #1,rtg_seq(%a1)
 move.l state_format(%a0),%d1
 bsr format_bpp
 move.l state_switch(%a0),%d0
 beq 1f
 moveq #2,%d0
 tst.l state_display(%a0)
 beq 1f
 tst.l %d1
 beq 1f
 moveq #1,%d0
 move.l state_format(%a0),rtg_format(%a1)
1: move.l %d0,rtg_on(%a1)
 addq.l #1,rtg_seq(%a1)
 move.l (%sp)+,%d1
 bra ring
| d1: a format; out d1 its bytes a pixel if the host shows it, else 0
format_bpp:
 move.l %d2,-(%sp)
 move.l %d1,%d2
 moveq #0,%d1
 cmp.l #31,%d2
 bhi 9f
 move.l RTG_BASE+rtg_formats,%d1
 btst %d2,%d1
 beq 8f
 moveq #1,%d1
 cmp.l #1,%d2
 beq 9f
 moveq #2,%d1
 cmp.l #4,%d2
 beq 9f
 cmp.l #10,%d2
 beq 9f
 moveq #4,%d1
 cmp.l #6,%d2
 beq 9f
 cmp.l #9,%d2
 beq 9f
8: moveq #0,%d1
9: move.l (%sp)+,%d2
 rts
set_gc:
 move.w %d0,gbi_Border(%a0)
 move.l %a1,gbi_ModeInfo(%a0)
 move.b gmi_Depth(%a1),gbi_Depth(%a0)
 move.l %a2,-(%sp)
 move.l #RTG_BASE,%a2
 addq.l #1,rtg_seq(%a2)
 moveq #0,%d0
 move.w gmi_Height(%a1),%d0
 move.l %d0,rtg_height(%a2)
 move.w gmi_Width(%a1),%d0
 move.l %d0,rtg_width(%a2)
 | rows until SetPanning places the screen: one at the start of card memory
 tst.l rtg_stride(%a2)
 bne 1f
 bsr pixel_bytes
 bsr row_bytes
 move.l %d0,rtg_stride(%a2)
1: addq.l #1,rtg_seq(%a2)
 move.l (%sp)+,%a2
 bra ring
set_panning:
 move.w %d1,gbi_XOffset(%a0)
 move.w %d2,gbi_YOffset(%a0)
 move.l %a1,state_panmem(%a0)
 and.l #65535,%d0
 move.l %d0,state_panw(%a0)
 and.l #65535,%d1
 move.l %d1,state_panx(%a0)
 and.l #65535,%d2
 move.l %d2,state_pany(%a0)
 bsr publish_pan
 bra ring
| the panned screen's rows and first pixel in the format the board was last given
publish_pan:
 movem.l %d0-%d3/%a2,-(%sp)
 move.l #RTG_BASE,%a2
 move.l state_panmem(%a0),%d3
 beq 9f
 bsr pixel_bytes
 move.l state_panw(%a0),%d0
 bsr row_bytes
 move.l state_pany(%a0),%d2
 mulu.l %d0,%d2
 sub.l rtg_vram(%a2),%d3
 add.l %d3,%d2
 move.l state_panx(%a0),%d3
 mulu.l %d1,%d3
 add.l %d3,%d2
 addq.l #1,rtg_seq(%a2)
 move.l %d0,rtg_stride(%a2)
 move.l %d2,rtg_offset(%a2)
 addq.l #1,rtg_seq(%a2)
9: movem.l (%sp)+,%d0-%d3/%a2
 rts
| d1: bytes a pixel of the board's format, 1 for one the host lacks
pixel_bytes:
 move.l state_format(%a0),%d1
 bsr format_bpp
 tst.l %d1
 bne 1f
 moveq #1,%d1
1: rts
set_palette:
 movem.l %d2-%d3/%a2,-(%sp)
 and.l #65535,%d0
 and.l #65535,%d1
 cmp.l #256,%d0
 bhs palette_done
 move.l #256,%d2
 sub.l %d0,%d2
 cmp.l %d2,%d1
 bls 1f
 move.l %d2,%d1
1: tst.l %d1
 beq palette_done
 mulu.w #3,%d0
 lea gbi_CLUT(%a0,%d0.l),%a1
 add.l %d0,%d0
 lea RTG_BASE+rtg_palette,%a2
 add.l %d0,%a2
 mulu.w #3,%d1
 subq.l #1,%d1
 addq.l #1,RTG_BASE+rtg_seq
2: moveq #0,%d2
 move.b (%a1)+,%d2
 move.l %d2,%d3
 lsl.w #8,%d2
 or.w %d3,%d2
 move.w %d2,(%a2)+
 dbra %d1,2b
 addq.l #1,RTG_BASE+rtg_seq
 bsr ring
palette_done:
 movem.l (%sp)+,%d2-%d3/%a2
 rts
| wakes the host display for the new state: the write faults; 1 is the SYS: handler's
ring:
 jmp 0xf7f088
| rows on four-byte boundaries; as the display's when the card's memory
| is the display and an 8-bit bitmap is as wide as it
bytes_per_row:
 move.l %d7,%d1
 bsr format_bpp
 tst.l %d1
 beq return_zero
| d0 pixels of d1 bytes (kept): the row's bytes
row_bytes:
 and.l #65535,%d0
 beq return_zero
 cmp.l #4096,%d0
 bhi return_zero
 cmp.l #1,%d1
 bne 5f
 tst.l RTG_BASE+rtg_copy
 bne 1f
 cmp.l RTG_BASE+rtg_max_width,%d0
 bne 1f
 cmp.l RTG_BASE+rtg_vstride,%d0
 bhi 1f
 move.l RTG_BASE+rtg_vstride,%d0
 rts
1: mulu.l %d1,%d0
 addq.l #3,%d0
 and.l #-4,%d0
 rts
 | a smaller mode's width: the display's rows
5: move.l %a1,-(%sp)
 bsr small_rows
 move.l (%sp)+,%a1
 beq 1b
 move.l RTG_BASE+rtg_max_width,%d0
 bra 1b
calculate_memory:
 move.l %a1,%d0
 rts
compatible_formats:
 move.l RTG_BASE+rtg_formats,%d0
 rts
resolve_clock:
 movem.l %d2-%d4/%a2,-(%sp)
 lea clocks(%pc),%a2
 move.l #-1,%d2
 moveq #0,%d3
 moveq #0,%d4
1: move.l (%a2,%d3.l*4),%d1
 sub.l %d0,%d1
 bpl 2f
 neg.l %d1
2: cmp.l %d2,%d1
 bhs 3f
 move.l %d1,%d2
 move.l %d3,%d4
3: addq.l #1,%d3
 cmp.l #4,%d3
 blo 1b
 move.l (%a2,%d4.l*4),gmi_PixelClock(%a1)
 move.b %d4,gmi_Numerator(%a1)
 move.b #1,gmi_Denominator(%a1)
 and.b #0xb8,gmi_Flags(%a1)
 move.l %d4,%d0
 movem.l (%sp)+,%d2-%d4/%a2
 rts
get_clock:
 cmp.l #4,%d0
 bhs return_zero
 lea clocks(%pc),%a1
 move.l (%a1,%d0.l*4),%d0
 rts
| Sprite updates publish an odd cseq, the state, then an even one.
set_sprite:
 move.l #RTG_BASE,%a1
 tst.l rtg_cursor(%a1)
 beq return_zero
 addq.l #1,rtg_cseq(%a1)
 tst.w %d0
 sne %d0
 and.l #1,%d0
 move.l %d0,rtg_con(%a1)
 addq.l #1,rtg_cseq(%a1)
 bsr ring
 moveq #1,%d0
 rts
set_sprite_position:
 move.l #RTG_BASE,%a1
 addq.l #1,rtg_cseq(%a1)
 bsr sprite_xy
 addq.l #1,rtg_cseq(%a1)
 bra ring
| top left: the pointer position on the visible screen plus the hot spot
sprite_xy:
 move.w gbi_MouseX(%a0),%d0
 sub.w gbi_XOffset(%a0),%d0
 move.b gbi_MouseXOffset(%a0),%d1
 ext.w %d1
 add.w %d1,%d0
 ext.l %d0
 move.l %d0,rtg_cx(%a1)
 move.w gbi_MouseY(%a0),%d0
 sub.w gbi_YOffset(%a0),%d0
 move.b gbi_MouseYOffset(%a0),%d1
 ext.w %d1
 add.w %d1,%d0
 ext.l %d0
 move.l %d0,rtg_cy(%a1)
 rts
| MouseImage: two header words, then per row plane 0 and plane 1 words
set_sprite_image:
 movem.l %d2-%d5/%a2-%a3,-(%sp)
 move.l #RTG_BASE,%a1
 addq.l #1,rtg_cseq(%a1)
 bsr sprite_xy
 moveq #0,%d2
 move.b gbi_MouseWidth(%a0),%d2
 cmp.l #16,%d2
 bls 1f
 moveq #16,%d2
1: moveq #0,%d3
 move.b gbi_MouseHeight(%a0),%d3
 cmp.l #48,%d3
 bls 2f
 moveq #48,%d3
2: move.l gbi_MouseImage(%a0),%d0
 bne 3f
 moveq #0,%d3
3: move.l %d2,rtg_cw(%a1)
 move.l %d3,rtg_ch(%a1)
 move.l %d0,%a2
 addq.l #4,%a2
 lea rtg_cimg(%a1),%a3
 bra 6f
4: move.w (%a2)+,%d4
 move.w (%a2)+,%d5
 moveq #15,%d1
5: moveq #0,%d0
 add.w %d5,%d5
 addx.b %d0,%d0
 add.w %d4,%d4
 addx.b %d0,%d0
 move.b %d0,(%a3)+
 dbra %d1,5b
6: dbra %d3,4b
 addq.l #1,rtg_cseq(%a1)
 bsr ring
 movem.l (%sp)+,%d2-%d5/%a2-%a3
 rts
| colours 1 to 3 as index 0 to 2, each component repeated in a word
set_sprite_color:
 move.l #RTG_BASE,%a1
 and.l #255,%d0
 cmp.l #3,%d0
 bhs noop
 addq.l #1,rtg_cseq(%a1)
 mulu.w #6,%d0
 lea rtg_crgb+6(%a1,%d0.l),%a1
 move.b %d1,(%a1)+
 move.b %d1,(%a1)+
 move.b %d2,(%a1)+
 move.b %d2,(%a1)+
 move.b %d3,(%a1)+
 move.b %d3,(%a1)+
 addq.l #1,RTG_BASE+rtg_cseq
 bra ring
| No beam: in and out of the blank on alternate calls, so polls end.
vsync_state:
 eor.l #1,state_vsync(%a0)
 move.l state_vsync(%a0),%d0
 rts
wait_vsync:
 tst.w %d0
 bne noop
 | Bounded fallback while the host display has no retrace signal.
 move.w #255,%d0
1: dbra %d0,1b
 rts
 .include "drawops.s"
 .include "draw.inc"
 .balign 4
clocks: .long 25175000,40000000,65000000,108000000
code_end:
