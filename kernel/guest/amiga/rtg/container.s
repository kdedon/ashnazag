.include "p96offsets.inc"
.include "rtgabi.inc"

.equ LIB_FLAGS, 14
.equ LIB_NEGSIZE, 16
.equ LIB_POSSIZE, 18
.equ LIB_OPENCNT, 32
.equ lib_exec, 36
.equ lib_segment, 44
.equ lib_name, 48
.equ lib_claimed, 52
.equ lib_size, 56
.equ state_switch, gbi_CardData
.equ state_display, gbi_CardData+4
.equ state_format, gbi_CardData+8

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
 cmp.l #1,rtg_version(%a1)
 bne return_zero
 cmp.l #4096,rtg_header_size(%a1)
 bne return_zero
 cmp.l #RTG_SIZE,rtg_memory_size(%a1)
 bne return_zero
 cmp.l #1,rtg_format(%a1)
 bne return_zero
 move.l #1,lib_claimed(%a6)
 move.l %a1,gbi_RegisterBase(%a0)
 move.l #RTG_PIXELS,gbi_MemoryBase(%a0)
 move.l #RTG_SIZE,gbi_MemorySize(%a0)
 move.l #RTG_PIXELS,gbi_MemorySpaceBase(%a0)
 move.l #RTG_SIZE,gbi_MemorySpaceSize(%a0)
 moveq #1,%d0
 rts

.macro callback field, target
 move.l #\target,\field(%a0)
.endm
init_card:
 move.l #board_name,gbi_BoardName(%a0)
 clr.l gbi_BoardType(%a0)
 clr.l gbi_PaletteChipType(%a0)
 clr.l gbi_GraphicsControllerType(%a0)
 move.w #8,gbi_BitsPerCannon(%a0)
 | CPU rendering and a software pointer share the linear indexed buffer.
 or.l #0x05100000,gbi_Flags(%a0)
 move.w #2,gbi_RGBFormats(%a0)
 move.w #2,gbi_SoftSpriteFlags(%a0)
 move.w #8192,gbi_MaxHorValue+2(%a0)
 move.w #8192,gbi_MaxVerValue+2(%a0)
 move.l #RTG_BASE,%a1
 move.l rtg_max_width(%a1),%d0
 cmp.l #4096,%d0
 bls 1f
 move.l #4096,%d0
1: move.w %d0,gbi_MaxHorResolution+2(%a0)
 move.l rtg_max_height(%a1),%d0
 cmp.l #4096,%d0
 bls 2f
 move.l #4096,%d0
2: move.w %d0,gbi_MaxVerResolution+2(%a0)
 move.l #4,gbi_PixelClockCount+4(%a0)
 clr.l state_switch(%a0)
 move.l #1,state_display(%a0)
 move.l #1,state_format(%a0)
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
 callback gbi_GetVSyncState, noop
 callback gbi_SetInterrupt, return_zero
 callback gbi_WaitBlitter, noop
 callback gbi_SetSprite, return_zero
 callback gbi_SetSpritePosition, noop
 callback gbi_SetSpriteImage, noop
 callback gbi_SetSpriteColor, noop
 moveq #1,%d0
 rts
noop:
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
set_dac:
 move.l %d7,state_format(%a0)
 move.l %d7,gbi_RGBFormat(%a0)
 bra publish_on
publish_on:
 move.l #RTG_BASE,%a1
 addq.l #1,rtg_seq(%a1)
 move.l state_switch(%a0),%d0
 beq 1f
 moveq #2,%d0
 tst.l state_display(%a0)
 beq 1f
 cmp.l #1,state_format(%a0)
 bne 1f
 moveq #1,%d0
1: move.l %d0,rtg_on(%a1)
 addq.l #1,rtg_seq(%a1)
 rts
set_gc:
 move.w %d0,gbi_Border(%a0)
 move.l %a1,gbi_ModeInfo(%a0)
 move.b gmi_Depth(%a1),gbi_Depth(%a0)
 move.l %a2,-(%sp)
 move.l #RTG_BASE,%a2
 addq.l #1,rtg_seq(%a2)
 moveq #0,%d0
 move.w gmi_Width(%a1),%d0
 move.l %d0,rtg_width(%a2)
 move.w gmi_Height(%a1),%d0
 move.l %d0,rtg_height(%a2)
 addq.l #1,rtg_seq(%a2)
 move.l (%sp)+,%a2
 rts
set_panning:
 movem.l %d2-%d3/%a2,-(%sp)
 move.w %d1,gbi_XOffset(%a0)
 move.w %d2,gbi_YOffset(%a0)
 move.l #RTG_BASE,%a2
 and.l #65535,%d0
 addq.l #3,%d0
 and.l #-4,%d0
 and.l #65535,%d1
 and.l #65535,%d2
 mulu.l %d0,%d2
 add.l %d1,%d2
 move.l %a1,%d3
 sub.l #RTG_PIXELS,%d3
 add.l %d3,%d2
 addq.l #1,rtg_seq(%a2)
 move.l %d0,rtg_stride(%a2)
 move.l %d2,rtg_offset(%a2)
 addq.l #1,rtg_seq(%a2)
 movem.l (%sp)+,%d2-%d3/%a2
 rts
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
palette_done:
 movem.l (%sp)+,%d2-%d3/%a2
 rts
bytes_per_row:
 cmp.l #1,%d7
 bne return_zero
 and.l #65535,%d0
 beq return_zero
 cmp.l #4096,%d0
 bhi return_zero
 addq.l #3,%d0
 and.l #-4,%d0
 rts
calculate_memory:
 move.l %a1,%d0
 rts
compatible_formats:
 moveq #2,%d0
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
wait_vsync:
 tst.w %d0
 bne noop
 | Bounded fallback while the host display has no retrace signal.
 move.w #255,%d0
1: dbra %d0,1b
 rts
 .balign 4
clocks: .long 25175000,40000000,65000000,108000000
code_end:
