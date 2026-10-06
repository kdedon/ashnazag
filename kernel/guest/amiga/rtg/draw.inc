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

| d4: the low byte of d4 in all four
spread:
 and.l #255,%d4
 move.l %d4,%d5
 lsl.l #8,%d5
 or.l %d5,%d4
 move.l %d4,%d5
 swap %d5
 or.l %d5,%d4
 rts

| the start of rectangle d0,d1 in the bitmap at a2, rows d6 apart, to a2
locate:
 move.l %d1,-(%sp)
 mulu.l %d6,%d1
 add.l %d0,%d1
 add.l %d1,%a2
 move.l (%sp)+,%d1
 rts

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
 bsr locate
 bsr spread
 sub.l %d2,%d6
 subq.l #1,%d3
1: move.l %d2,%d1
2: move.l %a2,%d0
 and.l #3,%d0
 beq 3f
 move.b %d4,(%a2)+
 subq.l #1,%d1
 bne 2b
 bra 6f
3: move.l %d1,%d0
 lsr.l #4,%d0
 beq 5f
 subq.l #1,%d0
4: move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 dbra %d0,4b
5: and.l #15,%d1
 beq 6f
 subq.l #1,%d1
7: move.b %d4,(%a2)+
 dbra %d1,7b
6: add.l %d6,%a2
 dbra %d3,1b
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
 bsr locate
 bsr spread
 sub.l %d2,%d6
 subq.l #1,%d3
1: move.l %d2,%d1
2: move.l %a2,%d0
 and.l #3,%d0
 beq 3f
 eor.b %d4,(%a2)+
 subq.l #1,%d1
 bne 2b
 bra 6f
3: move.l %d1,%d0
 lsr.l #2,%d0
 beq 5f
 subq.l #1,%d0
4: eor.l %d4,(%a2)+
 dbra %d0,4b
5: and.l #3,%d1
 beq 6f
 subq.l #1,%d1
7: eor.b %d4,(%a2)+
 dbra %d1,7b
6: add.l %d6,%a2
 dbra %d3,1b
 bsr show
8: movem.l (%sp)+,%d2-%d7/%a2-%a3
 rts
9: fallback gbi_InvertRectDefault

| Copies d4 bytes a row, d5 rows, from a2 to a3; rows d6 (source) and
| d7 (destination) apart.  Backwards from the ends when a3 is after a2
| in the same row; rows bottom up when the destination is lower.
| d0: 0 forward, 1 rows bottom up, 2 each row backwards.
copy:
 tst.l %d0
 beq 1f
 cmp.l #2,%d0
 beq 20f
 | bottom up: start at the last rows, step back
 move.l %d5,%d1
 subq.l #1,%d1
 move.l %d1,%d0
 mulu.l %d6,%d0
 add.l %d0,%a2
 mulu.l %d7,%d1
 add.l %d1,%a3
 neg.l %d6
 neg.l %d7
1: sub.l %d4,%d6
 sub.l %d4,%d7
 subq.l #1,%d5
2: move.l %d4,%d1
3: move.l %a3,%d0
 and.l #3,%d0
 beq 4f
 move.b (%a2)+,(%a3)+
 subq.l #1,%d1
 bne 3b
 bra 7f
4: move.l %d1,%d0
 lsr.l #4,%d0
 beq 6f
 subq.l #1,%d0
5: move.l (%a2)+,(%a3)+
 move.l (%a2)+,(%a3)+
 move.l (%a2)+,(%a3)+
 move.l (%a2)+,(%a3)+
 dbra %d0,5b
6: and.l #15,%d1
 beq 7f
 subq.l #1,%d1
8: move.b (%a2)+,(%a3)+
 dbra %d1,8b
7: add.l %d6,%a2
 add.l %d7,%a3
 dbra %d5,2b
 rts
 | each row from its end
20: add.l %d4,%a2
 add.l %d4,%a3
 add.l %d4,%d6
 add.l %d4,%d7
 subq.l #1,%d5
21: move.l %d4,%d1
22: move.l %a3,%d0
 and.l #3,%d0
 beq 23f
 move.b -(%a2),-(%a3)
 subq.l #1,%d1
 bne 22b
 bra 26f
23: move.l %d1,%d0
 lsr.l #2,%d0
 beq 25f
 subq.l #1,%d0
24: move.l -(%a2),-(%a3)
 dbra %d0,24b
25: and.l #3,%d1
 beq 26f
 subq.l #1,%d1
27: move.b -(%a2),-(%a3)
 dbra %d1,27b
26: add.l %d6,%a2
 add.l %d7,%a3
 dbra %d5,21b
 rts

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
 | direction, when both are one bitmap
 sub.l %a1,%a1
 cmp.l %a2,%a3
 bne 1f
 cmp.l %d1,%d3
 blo 1f
 bhi 2f
 cmp.l %d0,%d2
 bls 1f
 move.w #2,%a1
 bra 1f
2: move.w #1,%a1
1: bsr locate
 exg %a2,%a3
 exg %d6,%d7
 move.l %d2,%d0
 move.l %d3,%d1
 bsr locate
 exg %a2,%a3
 exg %d6,%d7
 move.l %a1,%d0
 bsr copy
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
 bsr locate
 sub.l %d2,%d6
 move.l %d6,-(%sp)
 move.l %d2,%a1
 | a5: the template row; d7: the first pixel's bit in its byte
 move.l (%a4),%a5
 moveq #0,%d7
 move.b 6(%a4),%d7
 move.l %d7,%d0
 lsr.l #3,%d0
 add.l %d0,%a5
 and.l #7,%d7
 | d5 foreground, d4 high word background, d1 inversion
 move.b 11(%a4),%d5
 move.w 14(%a4),%d1
 swap %d4
 move.w %d1,%d4
 swap %d4
 move.b 7(%a4),%d0
 moveq #0,%d1
 btst #2,%d0
 beq 2f
 moveq #-1,%d1
2: and.l #3,%d0
 move.l %d0,-(%sp)
 subq.l #1,%d3
3: move.l %a5,%a3
 move.b (%a3)+,%d0
 eor.b %d1,%d0
 lsl.b %d7,%d0
 moveq #8,%d6
 sub.l %d7,%d6
 move.l %a1,%d2
 subq.l #1,%d2
 tst.l (%sp)
 beq 10f
 cmp.l #1,(%sp)
 beq 20f
 | complement
30: add.b %d0,%d0
 bcc 31f
 eor.b %d4,(%a2)
31: addq.l #1,%a2
 subq.b #1,%d6
 bne 32f
 move.b (%a3)+,%d0
 eor.b %d1,%d0
 moveq #8,%d6
32: dbra %d2,30b
 bra 40f
 | JAM1
10: add.b %d0,%d0
 bcc 11f
 move.b %d5,(%a2)
11: addq.l #1,%a2
 subq.b #1,%d6
 bne 12f
 move.b (%a3)+,%d0
 eor.b %d1,%d0
 moveq #8,%d6
12: dbra %d2,10b
 bra 40f
 | JAM2
20: swap %d4
21: add.b %d0,%d0
 bcc 22f
 move.b %d5,(%a2)+
 bra 23f
22: move.b %d4,(%a2)+
23: subq.b #1,%d6
 bne 24f
 move.b (%a3)+,%d0
 eor.b %d1,%d0
 moveq #8,%d6
24: dbra %d2,21b
 swap %d4
40: add.l 4(%sp),%a2
 move.w 4(%a4),%d0
 ext.l %d0
 add.l %d0,%a5
 dbra %d3,3b
 addq.l #8,%sp
 bsr show
8: movem.l (%sp)+,%d2-%d7/%a2-%a5
 rts
9: fallback gbi_BlitTemplateDefault
