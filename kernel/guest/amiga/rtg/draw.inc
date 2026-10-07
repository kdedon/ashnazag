| Drawing into 8-bit CLUT bitmaps, as P96 asks of a board's blitter.
| Other formats and masks go to P96's own CPU functions.

| The host's pointer over the screen rectangle d0,d1 w d2 h d3 of the
| bitmap at a2, rows d6 apart: put the pixels it covers back first.
| The host draws it again when the card rings after drawing.
hide:
 movem.l %d0-%d7/%a0-%a3,-(%sp)
 lea RTG_BASE,%a1
 tst.l rtg_pshown(%a1)
 beq 9f
 cmp.l rtg_porigin(%a1),%a2
 bne 9f
 cmp.l rtg_pstride(%a1),%d6
 bne 9f
 cmp.l rtg_px1(%a1),%d0
 bge 9f
 add.l %d0,%d2
 cmp.l rtg_px0(%a1),%d2
 ble 9f
 cmp.l rtg_py1(%a1),%d1
 bge 9f
 add.l %d1,%d3
 cmp.l rtg_py0(%a1),%d3
 ble 9f
1: tas rtg_plock(%a1)
 bmi 1b
 tst.l rtg_pshown(%a1)
 beq 8f
 move.l rtg_py1(%a1),%d6
 sub.l rtg_py0(%a1),%d6
 move.l rtg_px1(%a1),%d7
 sub.l rtg_px0(%a1),%d7
 move.l rtg_pstride(%a1),%d4
 move.l rtg_py0(%a1),%d5
 mulu.l %d4,%d5
 add.l rtg_porigin(%a1),%d5
 add.l rtg_px0(%a1),%d5
 move.l %d5,%a0
 lea rtg_psave(%a1),%a2
 lea rtg_pdrawn(%a1),%a3
 moveq #0,%d3
2: moveq #0,%d2
3: move.b (%a0,%d2.l),%d0
 cmp.b (%a3,%d2.l),%d0
 bne 4f
 move.b (%a2,%d2.l),(%a0,%d2.l)
4: addq.l #1,%d2
 cmp.l %d7,%d2
 blo 3b
 add.l %d4,%a0
 lea 16(%a2),%a2
 lea 16(%a3),%a3
 addq.l #1,%d3
 cmp.l %d6,%d3
 blo 2b
 clr.l rtg_pshown(%a1)
 st rtg_pad(%a1)
8: clr.b rtg_plock(%a1)
 | the host found the lock taken: it draws the pointer after this drawing
 tst.b rtg_pwant(%a1)
 beq 9f
 clr.b rtg_pwant(%a1)
 st rtg_pad(%a1)
9: movem.l (%sp)+,%d0-%d7/%a0-%a3
 rts

| the pointer back on the screen, if a drawing took it off
show:
 bsr drew
 tst.b RTG_BASE+rtg_pad
 beq 1f
 clr.b RTG_BASE+rtg_pad
 bsr ring
1: rts

| the host copies the shown screen: one ring until it copies again
drew:
 tst.l RTG_BASE+rtg_track
 beq 1f
 tas RTG_BASE+rtg_drawn
 bmi 1f
 bra ring
1: rts

| P96's own function for what the card does not draw, then drew
.macro fallback field
 pea drew(%pc)
 move.l \field(%a0),-(%sp)
 rts
.endm

| an operation block on the stack for the drawing library, a0 at it
.macro opinit kind
 lea -OP_SIZE(%sp),%sp
 move.l %sp,%a0
 .rept OP_SIZE/4
 clr.l (%a0)+
 .endr
 move.l %sp,%a0
 move.l #\kind,OP_KIND(%a0)
 move.l #8,OP_DEPTH(%a0)
 move.l #255,OP_PMASK(%a0)
.endm
| the rectangle d0-d3 of the bitmap at a2, rows d6 apart
.macro opdst
 move.l %a2,OP_DST(%a0)
 move.l %d6,OP_DSTRIDE(%a0)
 movem.l %d0-%d3,OP_X(%a0)
.endm
.macro oprun
 bsr do_run
 lea OP_SIZE(%sp),%sp
.endm

| d0-d3 from P96's words
words:
 and.l #65535,%d0
 and.l #65535,%d1
 and.l #65535,%d2
 and.l #65535,%d3
 rts

| FillRect: a1 RenderInfo, d0,d1 x,y, d2,d3 w,h, d4 pen, d5 mask, d7 format
fill_rect:
 cmp.l #1,%d7
 bne 9f
 cmp.b #255,%d5
 bne 9f
 movem.l %d2-%d7/%a2-%a3,-(%sp)
 bsr words
 tst.l %d2
 beq 8f
 tst.l %d3
 beq 8f
 move.l (%a1),%a2
 moveq #0,%d6
 move.w 4(%a1),%d6
 bsr hide
 opinit DO_FILL
 opdst
 move.l #1,OP_MODE(%a0)
 move.l %d4,OP_FG(%a0)
 oprun
 bsr show
8: movem.l (%sp)+,%d2-%d7/%a2-%a3
 rts
9: fallback gbi_FillRectDefault

| InvertRect: a1 RenderInfo, d0,d1 x,y, d2,d3 w,h, d4 mask, d7 format
invert_rect:
 cmp.l #1,%d7
 bne 9f
 movem.l %d2-%d7/%a2-%a3,-(%sp)
 bsr words
 tst.l %d2
 beq 8f
 tst.l %d3
 beq 8f
 move.l (%a1),%a2
 moveq #0,%d6
 move.w 4(%a1),%d6
 bsr hide
 opinit DO_FILL
 opdst
 move.l #3,OP_MODE(%a0)
 move.l %d4,OP_PMASK(%a0)
 oprun
 bsr show
8: movem.l (%sp)+,%d2-%d7/%a2-%a3
 rts
9: fallback gbi_InvertRectDefault

| BlitRect: a1 RenderInfo, d0,d1 source, d2,d3 destination, d4,d5 w,h,
| d6 mask, d7 format
blit_rect:
 cmp.l #1,%d7
 bne 9f
 cmp.b #255,%d6
 bne 9f
 movem.l %d2-%d7/%a2-%a3,-(%sp)
 move.l (%a1),%a2
 moveq #0,%d6
 move.w 4(%a1),%d6
 move.l %d6,%d7
 move.l %a2,%a3
 bra blit_common
9: fallback gbi_BlitRectDefault

| BlitRectNoMaskComplete: a1 source, a2 destination RenderInfo, d0,d1
| source, d2,d3 destination, d4,d5 w,h, d6 minterm, d7 format; copies only
blit_complete:
 cmp.l #1,%d7
 bne 9f
 cmp.b #0xc0,%d6
 bne 9f
 movem.l %d2-%d7/%a2-%a3,-(%sp)
 moveq #0,%d7
 move.w 4(%a2),%d7
 move.l (%a2),%a3
 move.l (%a1),%a2
 moveq #0,%d6
 move.w 4(%a1),%d6
 bra blit_common
9: fallback gbi_BlitRectNoMaskCompleteDefault

| a2 source bitmap, rows d6 apart; a3 destination, rows d7 apart
blit_common:
 bsr words
 and.l #65535,%d4
 and.l #65535,%d5
 tst.l %d4
 beq 8f
 tst.l %d5
 beq 8f
 movem.l %d0-%d3,-(%sp)
 move.l %d4,%d2
 move.l %d5,%d3
 bsr hide
 move.l 8(%sp),%d0
 move.l 12(%sp),%d1
 exg %a2,%a3
 exg %d6,%d7
 bsr hide
 exg %a2,%a3
 exg %d6,%d7
 movem.l (%sp)+,%d0-%d3
 opinit DO_BLIT
 move.l %a3,OP_DST(%a0)
 move.l %d7,OP_DSTRIDE(%a0)
 move.l %d2,OP_X(%a0)
 move.l %d3,OP_Y(%a0)
 move.l %d4,OP_W(%a0)
 move.l %d5,OP_H(%a0)
 move.l #3,OP_MODE(%a0)
 move.l %a2,OP_SRC(%a0)
 move.l %d6,OP_SSTRIDE(%a0)
 move.l %d0,OP_SX(%a0)
 move.l %d1,OP_SY(%a0)
 oprun
 bsr show
8: movem.l (%sp)+,%d2-%d7/%a2-%a3
 rts

| BlitTemplate: a1 RenderInfo, a2 Template, d0,d1 x,y, d2,d3 w,h, d4 mask,
| d7 format.  Template: memory, row bytes (word), x offset 0-15, draw mode,
| foreground and background pens.
blit_template:
 cmp.l #1,%d7
 bne 9f
 cmp.b #255,%d4
 beq 1f
 | a mask only in complement mode, where it is what gets inverted
 btst #1,7(%a2)
 beq 9f
1: movem.l %d2-%d7/%a2-%a5,-(%sp)
 move.l %a2,%a4
 bsr words
 tst.l %d2
 beq 8f
 tst.l %d3
 beq 8f
 move.l (%a1),%a2
 moveq #0,%d6
 move.w 4(%a1),%d6
 bsr hide
 opinit DO_EXPAND
 opdst
 move.l #DOF_TEXT,OP_FLAGS(%a0)
 move.l (%a4),OP_SRC(%a0)
 move.w 4(%a4),%d0
 ext.l %d0
 move.l %d0,OP_SSTRIDE(%a0)
 moveq #0,%d0
 move.b 6(%a4),%d0
 move.l %d0,OP_SX(%a0)
 move.l 8(%a4),OP_FG(%a0)
 move.l 12(%a4),OP_BG(%a0)
 | JAM1 transparent, JAM2 replace, COMPLEMENT xor under the mask
 moveq #2,%d1
 move.b 7(%a4),%d0
 btst #2,%d0
 beq 1f
 move.l #DOF_TEXT+DOF_INVERT,OP_FLAGS(%a0)
1: and.l #3,%d0
 beq 2f
 moveq #1,%d1
 subq.l #1,%d0
 beq 2f
 moveq #3,%d1
 move.l %d4,OP_PMASK(%a0)
2: move.l %d1,OP_MODE(%a0)
 oprun
 bsr show
8: movem.l (%sp)+,%d2-%d7/%a2-%a5
 rts
9: fallback gbi_BlitTemplateDefault
