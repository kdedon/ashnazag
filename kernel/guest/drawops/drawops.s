| drawops: drawing into packed 1- and 8-bit bitmaps.
|
| An operation is a block of longs (struct do_op in drawops.h), passed
| in a0 to do_run or on the stack to do_draw.  Coordinates are clipped
| by the caller.  Rows are drawn a longword at a time: a row's bits from
| its first to its last longword, with edge masks for the partial ends.
| A longword becomes (d & A) ^ X, where A keeps and X sets bits.
|
|   fill    pattern of 16 words (or solid), modes 1 replace, 2 transparent,
|           3 xor, 4 reverse transparent; pmask limits the bits changed
|   expand  1-bit source onto the bitmap, the same modes; flag 1 inverts
|           the source
|   blit    logic operations 0-15 (VDI numbering)
|   line    16-bit style, the fill modes; horizontal and vertical as fills
|
| do_hook, when set, is offered fills, expands and blits of at least
| do_limit[kind] pixels (do_limit[0] for text) first; it returns nonzero
| when it drew them.

.equ OP_KIND,0
.equ OP_DEPTH,4
.equ OP_DST,8
.equ OP_DSTRIDE,12
.equ OP_X,16
.equ OP_Y,20
.equ OP_W,24
.equ OP_H,28
.equ OP_MODE,32
.equ OP_FG,36
.equ OP_BG,40
.equ OP_PMASK,44
.equ OP_SRC,48
.equ OP_SSTRIDE,52
.equ OP_SX,56
.equ OP_SY,60
.equ OP_PAT,64
.equ OP_STYLE,68
.equ OP_FLAGS,72
.equ OP_SIZE,76

.equ DO_FILL,1
.equ DO_EXPAND,2
.equ DO_BLIT,3
.equ DO_LINE,4
.equ DOF_INVERT,1
.equ DOF_TEXT,2

| MOVE16 (Ax)+,(Ay)+, for assemblers set to the 68020
.macro move16pp ax, ay
 .word 0xf620+\ax, 0x8000+(\ay<<12)
.endm

.macro cfunc name
 .globl \name, _\name
\name:
_\name:
.endm

.ifdef DO_STATE_TEXT
 .text
.else
 .data
.endif
 .balign 4
 .globl do_m16, _do_m16, do_hook, _do_hook, do_limit, _do_limit
do_m16:
_do_m16: .long 0
do_hook:
_do_hook: .long 0
do_limit:
_do_limit: .long 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff

 .text
| do_init(cpu): 30 for a 68030, 40 or 60 for MOVE16 rows
cfunc do_init
 moveq #0,%d0
 cmp.l #40,4(%sp)
 blt 1f
 moveq #1,%d0
1: move.l %d0,do_m16
 rts

| do_draw(op)
cfunc do_draw
 move.l 4(%sp),%a0
| do_run: a0 the operation; keeps all but d0-d1/a0-a1
cfunc do_run
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l OP_KIND(%a0),%d2
 move.l do_hook,%d0
 beq 2f
 cmp.l #DO_BLIT,%d2
 bhi 2f
 move.l %d2,%d1
 btst #1,OP_FLAGS+3(%a0)
 beq 1f
 moveq #0,%d1
1: move.l OP_W(%a0),%d3
 muls.l OP_H(%a0),%d3
 lea do_limit,%a1
 cmp.l (%a1,%d1.l*4),%d3
 blt 2f
 move.l %a0,-(%sp)
 move.l %d0,%a1
 move.l %a0,-(%sp)
 jsr (%a1)
 addq.l #4,%sp
 move.l (%sp)+,%a0
 tst.l %d0
 bne 9f
2: cmp.l #DO_FILL,%d2
 bne 3f
 bsr fill
 bra 9f
3: cmp.l #DO_EXPAND,%d2
 bne 4f
 bsr expand
 bra 9f
4: cmp.l #DO_BLIT,%d2
 bne 5f
 bsr blit
 bra 9f
5: cmp.l #DO_LINE,%d2
 bne 9f
 bsr line
9: movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts

| d0 as a pixel value repeated through a longword at the depth of a0
spread:
 cmp.l #1,OP_DEPTH(%a0)
 bne 1f
 and.l #1,%d0
 neg.l %d0
 rts
1: and.l #255,%d0
 move.l %d0,%d1
 lsl.l #8,%d1
 or.l %d1,%d0
 move.l %d0,%d1
 swap %d1
 or.l %d1,%d0
 rts

| 4 source bits, the first highest, as 4 pixel masks
 .balign 4
xtab:
 .long 0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff
 .long 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff
 .long 0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff
 .long 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff

| 8 source bits as 8 pixel masks
xtab8:
 .set xt_n,0
 .rept 256
 .long ((xt_n>>7)&1)*0xff000000+((xt_n>>6)&1)*0xff0000+((xt_n>>5)&1)*0xff00+((xt_n>>4)&1)*0xff
 .long ((xt_n>>3)&1)*0xff000000+((xt_n>>2)&1)*0xff0000+((xt_n>>1)&1)*0xff00+(xt_n&1)*0xff
 .set xt_n,xt_n+1
 .endr

| Row setup for x at the depth of a0, row address in d0: a2 the first
| longword, d1 the bit offset of x in it.
rowat:
 moveq #3,%d1
 and.l %d0,%d1
 lsl.l #3,%d1
 and.l #-4,%d0
 move.l OP_X(%a0),%d3
 cmp.l #1,OP_DEPTH(%a0)
 beq 1f
 lsl.l #3,%d3
1: add.l %d3,%d1
 move.l %d1,%d3
 lsr.l #5,%d3
 lsl.l #2,%d3
 add.l %d3,%d0
 move.l %d0,%a2
 and.l #31,%d1
 rts

| Edge masks for a row d1 bits into its first longword, d4 bits long:
| d4 first, d6 last, d3 the number of longwords.
edges:
 move.l %d1,%d3
 add.l %d4,%d3
 moveq #-1,%d4
 lsr.l %d1,%d4
 moveq #31,%d5
 and.l %d3,%d5
 moveq #-1,%d6
 tst.l %d5
 beq 1f
 lsr.l %d5,%d6
 not.l %d6
1: add.l #31,%d3
 lsr.l #5,%d3
 rts

| ---- fill ----
.equ F_FG,-4
.equ F_BG,-8
.equ F_PM,-12
.equ F_ROW,-16
.equ F_NB,-20
.equ F_PW,-24
.equ F_HB,-28
.equ F_Y,-32
.equ F_CLASS,-36
.equ F_ALK,-40
.equ F_AL,-44
.equ F_EL,-48
.equ F_AB,-80
.equ F_XB,-112
.equ F_BUF,-192
.equ F_E0,-196
.equ F_NL,-200
.equ F_A2,-204
.equ F_FA0,-208
.equ F_FX0,-212
.equ F_FAL,-216
.equ F_FXL,-220
.equ F_VAR,-224
.equ F_UNI,-228
.equ F_SIZE,228

| Rows keep their place in a longword when the stride is a multiple of
| 4: the edges and pattern cycle are then worked out once per pattern
| word.
fill:
 cmp.l #32,OP_W(%a0)
 ble fsmall
fill1:
 link %a6,#-F_SIZE
 tst.l OP_H(%a0)
 ble 9f
 move.l OP_FG(%a0),%d0
 bsr spread
 move.l %d0,F_FG(%a6)
 move.l OP_BG(%a0),%d0
 bsr spread
 move.l %d0,F_BG(%a6)
 move.l OP_H(%a0),%d2
 subq.l #1,%d2
 move.l OP_Y(%a0),%d0
 move.l %d0,F_Y(%a6)
 muls.l OP_DSTRIDE(%a0),%d0
 add.l OP_DST(%a0),%d0
 move.l %d0,F_ROW(%a6)
 move.l #0x10000,F_PW(%a6)
 bsr fsolid
 beq 1f
 cmp.l #1,OP_MODE(%a0)
 bne ffast
 move.l OP_PAT(%a0),%d0
 beq fsame
 move.l %d0,%a1
 moveq #14,%d1
 move.w (%a1)+,%d0
2: cmp.w (%a1)+,%d0
 dbne %d1,2b
 bne ffast
 bra fsame
1:
 move.l OP_PMASK(%a0),%d0
 bsr spread
 move.l %d0,F_PM(%a6)
 move.l OP_W(%a0),%d0
 cmp.l #1,OP_DEPTH(%a0)
 beq 1f
 lsl.l #3,%d0
1: move.l %d0,F_NB(%a6)
 lea F_BUF+15(%a6),%a1
 move.l %a1,%d0
 and.w #-16,%d0
 move.l %d0,F_AL(%a6)
 moveq #-1,%d0
 move.l %d0,F_HB(%a6)
 moveq #3,%d0
 and.l OP_DSTRIDE(%a0),%d0
 move.l %d0,F_VAR(%a6)
 | F_UNI: every row the same (fixed place in a longword, one pattern word)
 clr.l F_UNI(%a6)
 tst.l %d0
 bne 3f
 move.l OP_PAT(%a0),%d0
 beq 2f
 move.l %d0,%a1
 moveq #14,%d1
 move.w (%a1)+,%d0
1: cmp.w (%a1)+,%d0
 dbne %d1,1b
 bne 3f
2: st F_UNI(%a6)
3:
frow:
 tst.l F_HB(%a6)
 bmi 1f
 tst.l F_VAR(%a6)
 beq 2f
1: move.l F_ROW(%a6),%d0
 bsr rowat
 move.l %a2,F_A2(%a6)
 move.l F_NB(%a6),%d4
 bsr edges
 move.l %d4,F_E0(%a6)
 move.l %d6,F_EL(%a6)
 move.l %d3,F_NL(%a6)
 cmp.l F_HB(%a6),%d1
 beq 2f
 move.l %d1,F_HB(%a6)
 move.l #0x10000,F_PW(%a6)
2: move.l #0xffff,%d0
 move.l OP_PAT(%a0),%d1
 beq 3f
 move.l %d1,%a1
 moveq #15,%d1
 and.l F_Y(%a6),%d1
 moveq #0,%d0
 move.w (%a1,%d1.l*2),%d0
3: cmp.l F_PW(%a6),%d0
 beq 4f
 move.l F_HB(%a6),%d1
 bsr fpat
 bsr fconst
4: tst.b F_UNI(%a6)
 bne funi
41: move.l F_A2(%a6),%a2
 move.l (%a2),%d0
 and.l F_FA0(%a6),%d0
 move.l F_FX0(%a6),%d1
 eor.l %d1,%d0
 move.l %d0,(%a2)+
 move.l F_NL(%a6),%d3
 subq.l #2,%d3
 bmi 5f
 | stores inline, unless MOVE16 lines or another class
 cmp.l #1,F_CLASS(%a6)
 bne 6f
 tst.l do_m16
 beq 7f
 cmp.l #32,%d3
 bhs 6f
7: movem.l F_XB+4(%a6),%d4-%d7
 move.l %d3,%d0
 lsr.l #3,%d0
 beq 71f
 subq.l #1,%d0
70: move.l %d4,(%a2)+
 move.l %d5,(%a2)+
 move.l %d6,(%a2)+
 move.l %d7,(%a2)+
 move.l %d4,(%a2)+
 move.l %d5,(%a2)+
 move.l %d6,(%a2)+
 move.l %d7,(%a2)+
 dbra %d0,70b
71: btst #2,%d3
 beq 72f
 move.l %d4,(%a2)+
 move.l %d5,(%a2)+
 move.l %d6,(%a2)+
 move.l %d7,(%a2)+
72: and.w #3,%d3
 beq 8f
 move.l %d4,(%a2)+
 subq.w #1,%d3
 beq 8f
 move.l %d5,(%a2)+
 subq.w #1,%d3
 beq 8f
 move.l %d6,(%a2)+
 bra 8f
6: bsr fmid
8: move.l (%a2),%d0
 and.l F_FAL(%a6),%d0
 move.l F_FXL(%a6),%d1
 eor.l %d1,%d0
 move.l %d0,(%a2)
5: move.l OP_DSTRIDE(%a0),%d0
 add.l %d0,F_ROW(%a6)
 add.l %d0,F_A2(%a6)
 addq.l #1,F_Y(%a6)
 subq.l #1,%d2
 bpl frow
9: unlk %a6
 rts

| Small 8-bit solid fills, replace or xor, all of pmask, stride a
| multiple of 4: every row the same bytes, longwords, bytes, entered
| part way down unrolled stores.  w at most 32.
fsmall:
 move.l OP_W(%a0),%d3
 ble 7f
 tst.l OP_PAT(%a0)
 bne 8f
 move.l OP_MODE(%a0),%d7
 cmp.l #1,%d7
 beq 1f
 cmp.l #3,%d7
 bne 8f
1: cmp.l #1,OP_DEPTH(%a0)
 beq fsmall1
 cmp.l #8,OP_DEPTH(%a0)
 bne 8f
 moveq #3,%d0
 and.l OP_DSTRIDE(%a0),%d0
 bne 8f
 cmp.b #255,OP_PMASK+3(%a0)
 bne 8f
 move.l OP_H(%a0),%d5
 subq.l #1,%d5
 bmi 7f
 move.l OP_FG(%a0),%d0
 bsr spread
 move.l %d0,%d4
 move.l OP_DSTRIDE(%a0),%d6
 move.l OP_Y(%a0),%d0
 muls.l %d6,%d0
 add.l OP_DST(%a0),%d0
 add.l OP_X(%a0),%d0
 move.l %d0,%a2
 | head bytes, longwords - 1, tail bytes
 neg.l %d0
 and.l #3,%d0
 cmp.l %d3,%d0
 bls 2f
 move.l %d3,%d0
2: sub.l %d0,%d3
 add.l %d0,%d0
 cmp.l #1,%d7
 bne fsmallx
 lea 3f(%pc),%a3
 sub.l %d0,%a3
 moveq #3,%d0
 and.l %d3,%d0
 add.l %d0,%d0
 lea 5f(%pc),%a4
 sub.l %d0,%a4
 lsr.l #2,%d3
 subq.l #1,%d3
1: move.l %a2,%a1
 jmp (%a3)
 move.b %d4,(%a1)+
 move.b %d4,(%a1)+
 move.b %d4,(%a1)+
3: move.l %d3,%d0
 bmi 4f
6: move.l %d4,(%a1)+
 dbra %d0,6b
4: jmp (%a4)
 move.b %d4,(%a1)+
 move.b %d4,(%a1)+
 move.b %d4,(%a1)+
5: add.l %d6,%a2
 subq.l #1,%d5
 bpl 1b
7: rts
8: bra fill1
| 1 bit: one bit field instruction a row
fsmall1:
 btst #0,OP_PMASK+3(%a0)
 beq 8b
 move.l OP_H(%a0),%d5
 subq.l #1,%d5
 bmi 7f
 move.l OP_DSTRIDE(%a0),%d6
 move.l OP_Y(%a0),%d0
 muls.l %d6,%d0
 add.l OP_DST(%a0),%d0
 move.l %d0,%a2
 move.l OP_X(%a0),%d1
 cmp.l #3,%d7
 beq 3f
 btst #0,OP_FG+3(%a0)
 beq 2f
1: bfset (%a2){%d1:%d3}
 add.l %d6,%a2
 subq.l #1,%d5
 bpl 1b
 bra 7f
2: bfclr (%a2){%d1:%d3}
 add.l %d6,%a2
 subq.l #1,%d5
 bpl 2b
 bra 7f
3: bfchg (%a2){%d1:%d3}
 add.l %d6,%a2
 subq.l #1,%d5
 bpl 3b
7: rts

fsmallx:
 moveq #-1,%d4
 lea 3f(%pc),%a3
 sub.l %d0,%a3
 moveq #3,%d0
 and.l %d3,%d0
 add.l %d0,%d0
 lea 5f(%pc),%a4
 sub.l %d0,%a4
 lsr.l #2,%d3
 subq.l #1,%d3
1: move.l %a2,%a1
 jmp (%a3)
 eor.b %d4,(%a1)+
 eor.b %d4,(%a1)+
 eor.b %d4,(%a1)+
3: move.l %d3,%d0
 bmi 4f
6: eor.l %d4,(%a1)+
 dbra %d0,6b
4: jmp (%a4)
 eor.b %d4,(%a1)+
 eor.b %d4,(%a1)+
 eor.b %d4,(%a1)+
5: add.l %d6,%a2
 subq.l #1,%d5
 bpl 1b
 rts

| 8 bits, replace or xor, all of pmask, pattern words all 0 or ffff:
| each row one value, written as bytes to a longword boundary,
| longwords, bytes.  Z clear when so.
fsolid:
 cmp.l #8,OP_DEPTH(%a0)
 bne 8f
 cmp.b #255,OP_PMASK+3(%a0)
 bne 8f
 move.l OP_MODE(%a0),%d0
 cmp.l #1,%d0
 beq 1f
 cmp.l #3,%d0
 bne 8f
1: move.l OP_PAT(%a0),%d0
 beq 7f
 move.l %d0,%a1
 moveq #15,%d1
2: move.w (%a1)+,%d0
 beq 3f
 cmp.w #-1,%d0
 bne 8f
3: dbra %d1,2b
7: moveq #1,%d0
 rts
8: moveq #0,%d0
 rts

| one value for every row: replace with a solid or uniform pattern
fsame:
 move.l F_FG(%a6),%d4
 move.l OP_PAT(%a0),%d0
 beq 1f
 move.l %d0,%a1
 tst.w (%a1)
 bne 1f
 move.l F_BG(%a6),%d4
1: move.l F_ROW(%a6),%a2
 add.l OP_X(%a0),%a2
 move.l OP_DSTRIDE(%a0),%d6
 move.l OP_W(%a0),%d7
 move.l %d4,%d5
2: move.l %a2,%a3
 move.l %d7,%d1
3: move.l %a2,%d0
 and.w #3,%d0
 beq 4f
 move.b %d4,(%a2)+
 subq.l #1,%d1
 bne 3b
 bra 9f
4: move.l %d1,%d3
 lsr.l #2,%d3
 beq 7f
 tst.l do_m16
 beq 5f
 cmp.l #32,%d3
 blo 5f
 movem.l %d1/%d6-%d7/%a3,-(%sp)
 bsr 20f
 movem.l (%sp)+,%d1/%d6-%d7/%a3
 move.l %d4,%d5
 bra 7f
5: lsr.l #3,%d3
 beq 61f
 subq.l #1,%d3
6: move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 dbra %d3,6b
61: btst #4,%d1
 beq 62f
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
62: btst #3,%d1
 beq 63f
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
63: btst #2,%d1
 beq 7f
 move.l %d4,(%a2)+
7: btst #1,%d1
 beq 8f
 move.w %d4,(%a2)+
8: btst #0,%d1
 beq 9f
 move.b %d4,(%a2)+
9: lea (%a3,%d6.l),%a2
 dbra %d2,2b
 unlk %a6
 rts
 | MOVE16 lines: the value in the cycle buffers, once
20: cmp.l F_PW(%a6),%d4
 beq 21f
 move.l %d4,F_PW(%a6)
 lea F_XB(%a6),%a1
 moveq #7,%d0
22: move.l %d4,(%a1)+
 dbra %d0,22b
 moveq #1,%d0
 move.l %d0,F_CLASS(%a6)
 moveq #-1,%d0
 move.l %d0,F_ALK(%a6)
 lea F_BUF+15(%a6),%a1
 move.l %a1,%d0
 and.w #-16,%d0
 move.l %d0,F_AL(%a6)
21: moveq #1,%d1
 bra fmid

ffast:
 move.l F_ROW(%a6),%a2
 add.l OP_X(%a0),%a2
 | d4 the row's value: fg or bg (replace), ff or 0 (xor)
 moveq #-1,%d4
 move.l OP_PAT(%a0),%d1
 beq 1f
 move.l %d1,%a1
 moveq #15,%d1
 and.l F_Y(%a6),%d1
 move.w (%a1,%d1.l*2),%d4
 ext.l %d4
1: cmp.l #3,OP_MODE(%a0)
 beq 20f
 tst.l %d4
 beq 2f
 move.l F_FG(%a6),%d4
 bra 3f
2: move.l F_BG(%a6),%d4
3: move.l OP_W(%a0),%d1
4: move.l %a2,%d0
 and.w #3,%d0
 beq 5f
 move.b %d4,(%a2)+
 subq.l #1,%d1
 bne 4b
 bra 9f
5: move.l %d1,%d3
 lsr.l #2,%d3
 beq 7f
 cmp.l #16,%d3
 blo 6f
 cmp.l F_PW(%a6),%d4
 beq 10f
 move.l %d4,F_PW(%a6)
 move.l %d4,F_XB(%a6)
 move.l %d4,F_XB+4(%a6)
 move.l %d4,F_XB+8(%a6)
 move.l %d4,F_XB+12(%a6)
 move.l %d4,F_XB+16(%a6)
 move.l %d4,F_XB+20(%a6)
 move.l %d4,F_XB+24(%a6)
 move.l %d4,F_XB+28(%a6)
 moveq #1,%d0
 move.l %d0,F_CLASS(%a6)
 moveq #-1,%d0
 move.l %d0,F_ALK(%a6)
 lea F_BUF+15(%a6),%a1
 move.l %a1,%d0
 and.w #-16,%d0
 move.l %d0,F_AL(%a6)
10: move.l %d1,-(%sp)
 moveq #1,%d1
 tst.l do_m16
 bne 11f
 bsr fstore
 bra 12f
11: bsr fmid
12: move.l (%sp)+,%d1
 bra 7f
6: move.l %d4,(%a2)+
 subq.l #1,%d3
 bne 6b
7: and.w #3,%d1
 beq 9f
8: move.b %d4,(%a2)+
 subq.w #1,%d1
 bne 8b
 bra 9f
 | xor: rows of 0 are left alone
20: tst.l %d4
 beq 9f
 move.l OP_W(%a0),%d1
21: move.l %a2,%d0
 and.w #3,%d0
 beq 22f
 not.b (%a2)+
 subq.l #1,%d1
 bne 21b
 bra 9f
22: move.l %d1,%d3
 lsr.l #2,%d3
 beq 24f
23: not.l (%a2)+
 subq.l #1,%d3
 bne 23b
24: and.w #3,%d1
 beq 9f
25: not.b (%a2)+
 subq.w #1,%d1
 bne 25b
9: move.l OP_DSTRIDE(%a0),%d0
 add.l %d0,F_ROW(%a6)
 addq.l #1,F_Y(%a6)
 subq.l #1,%d2
 bpl ffast
 unlk %a6
 rts

| Every row alike, all stores, one value: edges and value in registers.
funi:
 clr.b F_UNI(%a6)
 cmp.l #1,F_CLASS(%a6)
 bne 41b
 lea F_XB(%a6),%a1
 move.l (%a1)+,%d4
 cmp.l (%a1)+,%d4
 bne 41b
 cmp.l (%a1)+,%d4
 bne 41b
 cmp.l (%a1)+,%d4
 bne 41b
 move.l F_NL(%a6),%d0
 subq.l #2,%d0
 tst.l do_m16
 beq 1f
 cmp.l #32,%d0
 bge 41b
 | a3 groups of 8 middle longwords, a5 the entry for the rest
1: move.l %d0,%a3
 tst.l %d0
 bmi 1f
 moveq #7,%d1
 and.l %d0,%d1
 add.l %d1,%d1
 lea 4f(%pc),%a5
 sub.l %d1,%a5
 lsr.l #3,%d0
 move.l %d0,%a3
1: move.l F_FA0(%a6),%d5
 move.l F_FX0(%a6),%d6
 move.l F_FAL(%a6),%d7
 move.l F_FXL(%a6),%d3
 move.l F_A2(%a6),%a4
2: move.l %a4,%a2
 move.l (%a2),%d0
 and.l %d5,%d0
 eor.l %d6,%d0
 move.l %d0,(%a2)+
 move.l %a3,%d0
 bmi 8f
 jmp (%a5)
3: move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
 move.l %d4,(%a2)+
4: dbra %d0,3b
6: move.l (%a2),%d0
 and.l %d7,%d0
 eor.l %d3,%d0
 move.l %d0,(%a2)
8: add.l OP_DSTRIDE(%a0),%a4
 dbra %d2,2b
 unlk %a6
 rts

| A and X of the first and last longwords, edges included
fconst:
 move.l F_E0(%a6),%d0
 move.l F_NL(%a6),%d3
 subq.l #1,%d3
 bne 1f
 and.l F_EL(%a6),%d0
1: move.l F_AB(%a6),%d4
 move.l %d0,%d5
 not.l %d5
 or.l %d5,%d4
 move.l %d4,F_FA0(%a6)
 and.l F_XB(%a6),%d0
 move.l %d0,F_FX0(%a6)
 and.l #3,%d3
 move.l F_EL(%a6),%d0
 move.l F_AB(%a6,%d3.l*4),%d4
 move.l %d0,%d5
 not.l %d5
 or.l %d5,%d4
 move.l %d4,F_FAL(%a6)
 and.l F_XB(%a6,%d3.l*4),%d0
 move.l %d0,F_FXL(%a6)
 rts

| d3 whole longwords at a2, from the pattern cycle's second
fmid:
 tst.l %d3
 beq 9f
 moveq #1,%d1
 move.l F_CLASS(%a6),%d0
 beq fgen
 cmp.l #2,%d0
 beq fxor
 tst.l do_m16
 beq 3f
 cmp.l #32,%d3
 blo 3f
 | longwords up to a line, then lines of 16 bytes from an aligned copy
1: move.l %a2,%d0
 and.w #15,%d0
 beq 2f
 move.l F_XB(%a6,%d1.l*4),(%a2)+
 addq.l #1,%d1
 and.w #3,%d1
 subq.l #1,%d3
 bra 1b
2: move.l F_AL(%a6),%a4
 cmp.l F_ALK(%a6),%d1
 beq 4f
 lea F_XB(%a6,%d1.l*4),%a3
 movem.l (%a3),%d4-%d7
 movem.l %d4-%d7,(%a4)
 movem.l %d4-%d7,16(%a4)
 movem.l %d4-%d7,32(%a4)
 movem.l %d4-%d7,48(%a4)
 move.l %d1,F_ALK(%a6)
4: move.l %a4,%a3
 move.l %d3,%d0
 lsr.l #4,%d0
 beq 6f
 subq.l #1,%d0
5: move16pp 3,2
 move16pp 3,2
 move16pp 3,2
 move16pp 3,2
 move.l %a4,%a3
 dbra %d0,5b
6: move.l %d3,%d0
 lsr.l #2,%d0
 and.l #3,%d0
 bra 8f
7: move16pp 3,2
8: dbra %d0,7b
 and.l #3,%d3
 beq 9f
 movem.l (%a4),%d4-%d6
 bra 33f
3:
fstore:
 lea F_XB(%a6,%d1.l*4),%a3
 movem.l (%a3),%d4-%d7
 move.l %d3,%d0
 lsr.l #3,%d0
 beq 30f
 subq.l #1,%d0
31: move.l %d4,(%a2)+
 move.l %d5,(%a2)+
 move.l %d6,(%a2)+
 move.l %d7,(%a2)+
 move.l %d4,(%a2)+
 move.l %d5,(%a2)+
 move.l %d6,(%a2)+
 move.l %d7,(%a2)+
 dbra %d0,31b
30: btst #2,%d3
 beq 32f
 move.l %d4,(%a2)+
 move.l %d5,(%a2)+
 move.l %d6,(%a2)+
 move.l %d7,(%a2)+
32: and.l #3,%d3
 beq 9f
33: move.l %d4,(%a2)+
 subq.l #1,%d3
 beq 9f
 move.l %d5,(%a2)+
 subq.l #1,%d3
 beq 9f
 move.l %d6,(%a2)+
9: rts
fxor:
 lea F_XB(%a6,%d1.l*4),%a3
 movem.l (%a3),%d4-%d7
 move.l %d3,%d0
 lsr.l #2,%d0
 beq 2f
 subq.l #1,%d0
1: eor.l %d4,(%a2)+
 eor.l %d5,(%a2)+
 eor.l %d6,(%a2)+
 eor.l %d7,(%a2)+
 dbra %d0,1b
2: and.l #3,%d3
 beq 9f
 eor.l %d4,(%a2)+
 subq.l #1,%d3
 beq 9f
 eor.l %d5,(%a2)+
 subq.l #1,%d3
 beq 9f
 eor.l %d6,(%a2)+
9: rts
fgen:
 lea F_AB(%a6,%d1.l*4),%a3
 lea F_XB(%a6,%d1.l*4),%a4
 move.l %d3,%d0
 lsr.l #2,%d0
 beq 2f
 subq.l #1,%d0
1:
 .irp o,0,4,8,12
 move.l (%a2),%d4
 and.l \o(%a3),%d4
 move.l \o(%a4),%d5
 eor.l %d5,%d4
 move.l %d4,(%a2)+
 .endr
 dbra %d0,1b
2: and.l #3,%d3
 beq 9f
3: move.l (%a2),%d4
 and.l (%a3)+,%d4
 move.l (%a4)+,%d5
 eor.l %d5,%d4
 move.l %d4,(%a2)+
 subq.l #1,%d3
 bne 3b
9: rts

| The pattern cycle for pattern word d0, x d1 bits into the first
| longword: A and X for four longwords, twice over.
fpat:
 movem.l %d1-%d2/%a2,-(%sp)
 move.l %d0,F_PW(%a6)
 move.l %d1,F_HB(%a6)
 moveq #-1,%d3
 move.l %d3,F_ALK(%a6)
 cmp.l #1,OP_DEPTH(%a0)
 beq 1f
 lsr.l #3,%d1
1: move.l OP_X(%a0),%d3
 sub.l %d1,%d3
 lea xtab(%pc),%a1
 moveq #0,%d2
2: move.l %d0,%d5
 rol.w %d3,%d5
 cmp.l #1,OP_DEPTH(%a0)
 bne 3f
 move.w %d5,%d6
 swap %d5
 move.w %d6,%d5
 bra 4f
3: and.l #0xffff,%d5
 lsr.l #8,%d5
 lsr.l #4,%d5
 move.l (%a1,%d5.l*4),%d5
 addq.l #4,%d3
4: move.l %d5,%d1
 bsr modeax
 move.l %d6,F_AB(%a6,%d2.l*4)
 move.l %d6,F_AB+16(%a6,%d2.l*4)
 move.l %d7,F_XB(%a6,%d2.l*4)
 move.l %d7,F_XB+16(%a6,%d2.l*4)
 addq.l #1,%d2
 cmp.l #4,%d2
 blo 2b
 | 1 all stores, 2 all xors, 0 otherwise
 lea F_AB(%a6),%a1
 moveq #0,%d1
 bsr 5f
 bne 1f
 moveq #1,%d0
 bra 7f
1: moveq #-1,%d1
 bsr 5f
 bne 6f
 moveq #2,%d0
 bra 7f
6: moveq #0,%d0
7: move.l %d0,F_CLASS(%a6)
 movem.l (%sp)+,%d1-%d2/%a2
 rts
5: cmp.l (%a1),%d1
 bne 8f
 cmp.l 4(%a1),%d1
 bne 8f
 cmp.l 8(%a1),%d1
 bne 8f
 cmp.l 12(%a1),%d1
8: rts

| A in d6 and X in d7 for pixel mask d1 by mode, from F_FG/F_BG/F_PM
| (the expand frame keeps them at the same places)
modeax:
 move.l OP_MODE(%a0),%d4
 cmp.l #1,%d4
 bne 1f
 move.l F_FG(%a6),%d7
 move.l F_BG(%a6),%d4
 eor.l %d4,%d7
 and.l %d1,%d7
 eor.l %d4,%d7
 moveq #0,%d6
 bra 5f
1: cmp.l #2,%d4
 bne 2f
 move.l %d1,%d6
 not.l %d6
 move.l F_FG(%a6),%d7
 and.l %d1,%d7
 bra 5f
2: cmp.l #3,%d4
 bne 3f
 moveq #-1,%d6
 move.l %d1,%d7
 bra 5f
3: move.l %d1,%d6
 move.l %d1,%d7
 not.l %d7
 and.l F_FG(%a6),%d7
5: move.l F_PM(%a6),%d4
 and.l %d4,%d7
 not.l %d4
 or.l %d4,%d6
 rts

| ---- expand ----
| one op on (a2)+ of size s with the pixel mask in d0
.macro xrep s
 and.\s %d2,%d0
 eor.\s %d3,%d0
 move.\s %d0,(%a2)+
.endm
.macro xtr s
 and.\s %d3,%d0
 move.\s (%a2),%d1
 eor.\s %d2,%d1
 and.\s %d0,%d1
 eor.\s %d1,(%a2)+
.endm
.macro xrv s
 not.\s %d0
 xtr \s
.endm
.macro xxo s
 and.\s %d3,%d0
 eor.\s %d0,(%a2)+
.endm
| All rows with op: the source 32 bits at a time, two longwords for
| each 8 pixels, then a longword, a word and a byte for the rest.
.macro xrows op
 move.l X_ROW(%a6),%a4
 add.l OP_X(%a0),%a4
 move.l X_SROW(%a6),%a1
 lea xtab8(%pc),%a5
 moveq #7,%d0
 and.l OP_SX(%a0),%d0
 beq 20f
1: move.l %a4,%a2
 move.l OP_SX(%a0),%d6
 move.l OP_W(%a0),%d7
2: moveq #32,%d5
 cmp.l %d7,%d5
 ble 3f
 move.l %d7,%d5
3: bfextu (%a1){%d6:%d5},%d4
 add.l %d5,%d6
 sub.l %d5,%d7
 moveq #32,%d0
 sub.l %d5,%d0
 lsl.l %d0,%d4
 btst #0,OP_FLAGS+3(%a0)
 beq 4f
 not.l %d4
4: move.l %d5,-(%sp)
 lsr.l #3,%d5
 beq 6f
 subq.l #1,%d5
5: rol.l #8,%d4
 moveq #0,%d0
 move.b %d4,%d0
 lea (%a5,%d0.l*8),%a3
 move.l (%a3)+,%d0
 \op l
 move.l (%a3),%d0
 \op l
 dbra %d5,5b
6: move.l (%sp)+,%d5
 and.w #7,%d5
 beq 9f
 rol.l #8,%d4
 moveq #0,%d0
 move.b %d4,%d0
 lea (%a5,%d0.l*8),%a3
 move.l (%a3)+,%d4
 cmp.w #4,%d5
 blo 7f
 move.l %d4,%d0
 \op l
 move.l (%a3),%d4
 subq.w #4,%d5
 beq 9f
7: cmp.w #2,%d5
 blo 8f
 move.l %d4,%d0
 swap %d0
 \op w
 cmp.w #3,%d5
 bne 9f
 move.l %d4,%d0
 lsr.l #8,%d0
 \op b
 bra 9f
8: move.l %d4,%d0
 rol.l #8,%d0
 \op b
9: tst.l %d7
 bne 2b
 add.l OP_DSTRIDE(%a0),%a4
 add.l OP_SSTRIDE(%a0),%a1
 subq.l #1,X_ROWS(%a6)
 bne 1b
 unlk %a6
 rts
 | a source starting on a byte: read it a byte at a time
20: move.l OP_SX(%a0),%d6
 lsr.l #3,%d6
 moveq #0,%d7
 btst #0,OP_FLAGS+3(%a0)
 beq 21f
 moveq #-1,%d7
21: move.l OP_W(%a0),%d0
 lsr.l #3,%d0
 subq.l #1,%d0
 move.l %d0,X_NB(%a6)
22: move.l %a4,%a2
 lea (%a1,%d6.l),%a3
 moveq #0,%d4
 move.l X_NB(%a6),%d5
 bmi 24f
23: move.b (%a3)+,%d4
 eor.b %d7,%d4
 move.l (%a5,%d4.w*8),%d0
 \op l
 move.l 4(%a5,%d4.w*8),%d0
 \op l
 dbra %d5,23b
24: moveq #7,%d5
 and.l OP_W(%a0),%d5
 beq 29f
 move.b (%a3),%d4
 eor.b %d7,%d4
 lea (%a5,%d4.w*8),%a3
 move.l (%a3)+,%d4
 cmp.w #4,%d5
 blo 27f
 move.l %d4,%d0
 \op l
 move.l (%a3),%d4
 subq.w #4,%d5
 beq 29f
27: cmp.w #2,%d5
 blo 28f
 move.l %d4,%d0
 swap %d0
 \op w
 cmp.w #3,%d5
 bne 29f
 move.l %d4,%d0
 lsr.l #8,%d0
 \op b
 bra 29f
28: move.l %d4,%d0
 rol.l #8,%d0
 \op b
29: add.l OP_DSTRIDE(%a0),%a4
 add.l OP_SSTRIDE(%a0),%a1
 subq.l #1,X_ROWS(%a6)
 bne 22b
 unlk %a6
 rts
.endm

.equ X_ROWS,-16
.equ X_ROW,-20
.equ X_SROW,-24
.equ X_NB,-28
.equ X_E0,-32
.equ X_EL,-36
.equ X_NL,-40
.equ X_INV,-44
.equ X_FAST,-48
.equ X_P,-52
.equ X_FBX,-56
.equ X_N0,-60
.equ X_SH0,-64
.equ X_M0,-68
.equ X_IM0,-72
.equ X_NM,-76
.equ X_LW,-80
.equ X_LSH,-84
.equ X_ML,-88
.equ X_IML,-92
.equ X_SIZE,92

| 1-bit ops on (a2)+, the source bits in d0 already within the edge
| mask d5: replace, masked or whole
.macro x1rep m
 and.l %d2,%d0
 eor.l %d3,%d0
.if \m
 move.l (%a2),%d1
 eor.l %d1,%d0
 and.l %d5,%d0
 eor.l %d0,(%a2)+
.else
 move.l %d0,(%a2)+
.endif
.endm
.macro x1tr m
 move.l (%a2),%d1
 eor.l %d2,%d1
 and.l %d0,%d1
 eor.l %d1,(%a2)+
.endm
.macro x1xo m
 eor.l %d0,(%a2)+
.endm
| 1-bit rows, all of pmask: a partial first longword, whole ones, a
| partial last, each from one bit field of the source.
.macro x1rows op
1: move.l %a4,%a2
 move.l OP_SX(%a0),%d4
 move.l X_N0(%a6),%d6
 bfextu (%a3){%d4:%d6},%d0
 add.l %d6,%d4
 move.l X_SH0(%a6),%d6
 lsl.l %d6,%d0
 move.l X_IM0(%a6),%d6
 eor.l %d6,%d0
 move.l X_M0(%a6),%d5
 \op 1
 move.l X_NM(%a6),%d7
 beq 3f
 subq.l #1,%d7
 move.l X_INV(%a6),%d6
2: bfextu (%a3){%d4:0},%d0
 add.l %a1,%d4
 eor.l %d6,%d0
 \op 0
 dbra %d7,2b
3: move.l X_LW(%a6),%d6
 beq 4f
 bfextu (%a3){%d4:%d6},%d0
 move.l X_LSH(%a6),%d6
 lsl.l %d6,%d0
 move.l X_IML(%a6),%d6
 eor.l %d6,%d0
 move.l X_ML(%a6),%d5
 \op 1
4: add.l OP_DSTRIDE(%a0),%a4
 add.l OP_SSTRIDE(%a0),%a3
 subq.l #1,X_ROWS(%a6)
 bne 1b
 unlk %a6
 rts
.endm

expand:
 link %a6,#-X_SIZE
 tst.l OP_W(%a0)
 ble 9f
 tst.l OP_H(%a0)
 ble 9f
 move.l OP_FG(%a0),%d0
 bsr spread
 move.l %d0,F_FG(%a6)
 move.l OP_BG(%a0),%d0
 bsr spread
 move.l %d0,F_BG(%a6)
 eor.l %d0,F_FG(%a6)
 move.l F_FG(%a6),X_FBX(%a6)
 eor.l %d0,F_FG(%a6)
 move.l OP_PMASK(%a0),%d0
 bsr spread
 move.l %d0,F_PM(%a6)
 move.l OP_Y(%a0),%d0
 muls.l OP_DSTRIDE(%a0),%d0
 add.l OP_DST(%a0),%d0
 move.l %d0,X_ROW(%a6)
 move.l OP_SY(%a0),%d0
 muls.l OP_SSTRIDE(%a0),%d0
 add.l OP_SRC(%a0),%d0
 move.l %d0,X_SROW(%a6)
 lea xtab(%pc),%a5
 move.l OP_H(%a0),X_ROWS(%a6)
 cmp.l #8,OP_DEPTH(%a0)
 bne xl
 | 8 bits: eight pixels a byte, longwords at any address
 move.l F_PM(%a6),%d3
 move.l %d3,%d2
 move.l OP_MODE(%a0),%d0
 cmp.l #1,%d0
 bne x8b
 addq.l #1,%d2
 bne xl
x8b: cmp.l #1,%d0
 bne x8c
 move.l X_FBX(%a6),%d2
 move.l F_BG(%a6),%d3
 xrows xrep
x8c: cmp.l #3,%d0
 bne x8d
 xrows xxo
x8d: move.l F_FG(%a6),%d2
 cmp.l #2,%d0
 bne x8e
 xrows xtr
x8e: xrows xrv
xl:
 cmp.l #1,OP_DEPTH(%a0)
 bne xl0
 moveq #3,%d0
 and.l OP_DSTRIDE(%a0),%d0
 bne xl0
 moveq #1,%d0
 and.l OP_PMASK(%a0),%d0
 beq xl0
 move.l OP_MODE(%a0),%d0
 subq.l #1,%d0
 cmp.l #3,%d0
 bhi xl0
 | the inversion as a mask; reverse transparent is transparent inverted
 moveq #0,%d7
 btst #0,OP_FLAGS+3(%a0)
 beq 1f
 moveq #-1,%d7
1: cmp.l #3,%d0
 bne 1f
 not.l %d7
 moveq #1,%d0
1: move.l %d7,X_INV(%a6)
 move.l %d0,X_FAST(%a6)
 move.l X_ROW(%a6),%d0
 bsr rowat
 move.l %a2,%a4
 | n0 = min(32 - bit, w), shifted up by 32 - bit - n0
 move.l OP_W(%a0),%d7
 moveq #32,%d2
 sub.l %d1,%d2
 cmp.l %d7,%d2
 ble 1f
 move.l %d7,%d2
1: move.l %d2,X_N0(%a6)
 moveq #32,%d3
 sub.l %d1,%d3
 sub.l %d2,%d3
 move.l %d3,X_SH0(%a6)
 moveq #-1,%d4
 lsr.l %d1,%d4
 move.l %d1,%d3
 add.l %d2,%d3
 moveq #-1,%d5
 lsr.l %d3,%d5
 not.l %d5
 and.l %d5,%d4
 move.l %d4,X_M0(%a6)
 and.l X_INV(%a6),%d4
 move.l %d4,X_IM0(%a6)
 sub.l %d2,%d7
 moveq #31,%d3
 and.l %d7,%d3
 lsr.l #5,%d7
 move.l %d7,X_NM(%a6)
 move.l %d3,X_LW(%a6)
 moveq #32,%d4
 sub.l %d3,%d4
 move.l %d4,X_LSH(%a6)
 moveq #-1,%d4
 lsr.l %d3,%d4
 not.l %d4
 move.l %d4,X_ML(%a6)
 and.l X_INV(%a6),%d4
 move.l %d4,X_IML(%a6)
 move.l X_SROW(%a6),%a3
 move.w #32,%a1
 move.l X_FAST(%a6),%d0
 bne x1b
 move.l X_FBX(%a6),%d2
 move.l F_BG(%a6),%d3
 x1rows x1rep
x1b: subq.l #1,%d0
 bne x1c
 move.l F_FG(%a6),%d2
 x1rows x1tr
x1c: x1rows x1xo
xl0:
 move.l F_PM(%a6),%d0
 | a whole longword in its own loop: all of pmask, modes 1-4
 moveq #0,%d1
 addq.l #1,%d0
 bne 1f
 move.l OP_MODE(%a0),%d1
 subq.l #1,%d1
 cmp.l #3,%d1
 bls 2f
 moveq #3,%d1
2: addq.l #1,%d1
1: move.l %d1,X_FAST(%a6)
 moveq #0,%d0
 btst #0,OP_FLAGS+3(%a0)
 beq 1f
 moveq #-1,%d0
1: move.l %d0,X_INV(%a6)
 move.l OP_W(%a0),%d0
 moveq #32,%d1
 cmp.l #1,OP_DEPTH(%a0)
 beq 1f
 lsl.l #3,%d0
 moveq #4,%d1
1: move.l %d0,X_NB(%a6)
 move.l %d1,X_P(%a6)
xrow:
 move.l X_ROW(%a6),%d0
 bsr rowat
 move.l X_NB(%a6),%d4
 move.l %d1,%d7
 bsr edges
 move.l %d4,X_E0(%a6)
 move.l %d6,X_EL(%a6)
 move.l %d3,X_NL(%a6)
 move.l %d3,%a4
 move.l X_SROW(%a6),%a3
 | d4 bits waiting, from the top; d5 how many; d6 the next source bit;
 | d7 source bits left.  The first longword's pixels before x count as
 | waiting bits.
 move.l %d7,%d5
 cmp.l #1,OP_DEPTH(%a0)
 beq 1f
 lsr.l #3,%d5
1: moveq #0,%d4
 move.l OP_SX(%a0),%d6
 move.l OP_W(%a0),%d7
xlong:
 cmp.l X_P(%a6),%d5
 bge 2f
 tst.l %d7
 beq 2f
 moveq #32,%d2
 sub.l %d5,%d2
 cmp.l %d7,%d2
 ble 1f
 move.l %d7,%d2
1: bfextu (%a3){%d6:%d2},%d3
 moveq #32,%d0
 sub.l %d5,%d0
 sub.l %d2,%d0
 lsl.l %d0,%d3
 or.l %d3,%d4
 add.l %d2,%d6
 sub.l %d2,%d7
 add.l %d2,%d5
2: cmp.l #1,OP_DEPTH(%a0)
 beq 3f
 bfextu %d4{#0:#4},%d0
 lsl.l #4,%d4
 subq.l #4,%d5
 move.l (%a5,%d0.l*4),%d0
 bra 4f
3: move.l %d4,%d0
 moveq #0,%d4
 sub.l #32,%d5
4: move.l X_INV(%a6),%d1
 eor.l %d1,%d0
 | d1 the edge mask
 moveq #-1,%d1
 cmp.l X_NL(%a6),%a4
 bne 5f
 and.l X_E0(%a6),%d1
5: cmp.w #1,%a4
 bne 6f
 and.l X_EL(%a6),%d1
6: move.l X_FAST(%a6),%d2
 beq xgen
 cmp.l #-1,%d1
 bne xgen
 subq.l #2,%d2
 bmi 11f
 beq 12f
 subq.l #2,%d2
 bmi 13f
 not.l %d0
 | transparent
12: move.l (%a2),%d2
 move.l F_FG(%a6),%d3
 eor.l %d3,%d2
 and.l %d0,%d2
 eor.l %d2,(%a2)+
 bra xnext
 | replace
11: and.l X_FBX(%a6),%d0
 move.l F_BG(%a6),%d2
 eor.l %d2,%d0
 move.l %d0,(%a2)+
 bra xnext
 | xor
13: eor.l %d0,(%a2)+
 bra xnext
xgen:
 movem.l %d4/%d6-%d7,-(%sp)
 move.l %d1,%d3
 move.l %d0,%d1
 bsr modeax
 and.l %d3,%d7
 not.l %d3
 or.l %d3,%d6
 move.l (%a2),%d3
 and.l %d6,%d3
 eor.l %d7,%d3
 move.l %d3,(%a2)+
 movem.l (%sp)+,%d4/%d6-%d7
xnext:
 subq.l #1,%a4
 cmp.w #0,%a4
 bne xlong
 move.l OP_DSTRIDE(%a0),%d0
 add.l %d0,X_ROW(%a6)
 move.l OP_SSTRIDE(%a0),%d0
 add.l %d0,X_SROW(%a6)
 subq.l #1,X_ROWS(%a6)
 bne xrow
9: unlk %a6
 rts

| ---- blit ----
.equ B_K02,-4
.equ B_M2,-8
.equ B_K13,-12
.equ B_M3,-16
.equ B_ROWS,-20
.equ B_DROW,-24
.equ B_SROW,-28
.equ B_DSTEP,-32
.equ B_SSTEP,-36
.equ B_BACK,-40
.equ B_NB,-44
.equ B_SB,-48
.equ B_NL,-52
.equ B_HB,-56
.equ B_OP,-60
.equ B_TMP,-140
.equ B_N0,-144
.equ B_NG,-148
.equ B_ENT,-152
.equ B_ADJ,-156
.equ B_SIZE,156

| ops without a source: as fills
blit:
 link %a6,#-B_SIZE
 tst.l OP_W(%a0)
 ble 99f
 tst.l OP_H(%a0)
 ble 99f
 move.l OP_MODE(%a0),%d0
 and.l #15,%d0
 cmp.l #5,%d0
 beq 99f
 moveq #1,%d1
 moveq #0,%d2
 tst.l %d0
 beq 1f
 moveq #-1,%d2
 cmp.l #15,%d0
 beq 1f
 cmp.l #10,%d0
 bne 2f
 moveq #3,%d1
1: lea B_TMP(%a6),%a1
 moveq #OP_SIZE/4-1,%d3
3: move.l (%a0)+,(%a1)+
 dbra %d3,3b
 lea B_TMP(%a6),%a0
 move.l #DO_FILL,OP_KIND(%a0)
 move.l %d1,OP_MODE(%a0)
 move.l %d2,OP_FG(%a0)
 moveq #-1,%d2
 move.l %d2,OP_PMASK(%a0)
 clr.l OP_PAT(%a0)
 bsr fill
 bra 99f
2: cmp.l #3,%d0
 bne 20f
 cmp.l #1,OP_DEPTH(%a0)
 bne 20f
 cmp.l #64,OP_W(%a0)
 blt 20f
 move.l OP_X(%a0),%d1
 move.l OP_SX(%a0),%d2
 eor.l %d1,%d2
 and.l #7,%d2
 beq bbytes
20: | the direction: from the end when the destination overlaps the
 | source after it
 move.l OP_Y(%a0),%d1
 muls.l OP_DSTRIDE(%a0),%d1
 add.l OP_DST(%a0),%d1
 move.l OP_SY(%a0),%d2
 muls.l OP_SSTRIDE(%a0),%d2
 add.l OP_SRC(%a0),%d2
 move.l %d1,B_DROW(%a6)
 move.l %d2,B_SROW(%a6)
 move.l OP_DSTRIDE(%a0),B_DSTEP(%a6)
 move.l OP_SSTRIDE(%a0),B_SSTEP(%a6)
 clr.l B_BACK(%a6)
 move.l OP_X(%a0),%d3
 move.l OP_SX(%a0),%d4
 cmp.l #1,OP_DEPTH(%a0)
 beq 4f
 add.l %d3,%d1
 add.l %d4,%d2
 moveq #0,%d3
 moveq #0,%d4
4: | d1/d3 destination, d2/d4 source: byte and bit
 cmp.l %d2,%d1
 bhi 5f
 bne 7f
 cmp.l %d4,%d3
 bls 7f
 move.l #1,B_BACK(%a6)
 bra 7f
5: move.l OP_SSTRIDE(%a0),%d5
 cmp.l OP_DSTRIDE(%a0),%d5
 bne 7f
 move.l %d1,%d6
 sub.l %d2,%d6
 move.l OP_W(%a0),%d7
 cmp.l #1,OP_DEPTH(%a0)
 beq 6f
 cmp.l %d7,%d6
 bcc 6f
 move.l #1,B_BACK(%a6)
6: move.l OP_H(%a0),%d7
 mulu.l %d5,%d7
 cmp.l %d7,%d6
 bcc 7f
 | bottom up
 move.l OP_H(%a0),%d7
 subq.l #1,%d7
 mulu.l %d5,%d7
 add.l %d7,B_DROW(%a6)
 add.l %d7,B_SROW(%a6)
 neg.l B_DSTEP(%a6)
 neg.l B_SSTEP(%a6)
7: move.l OP_H(%a0),B_ROWS(%a6)
 cmp.l #8,OP_DEPTH(%a0)
 bne b1copy
 cmp.l #3,%d0
 beq bcopy

| any operation, a longword at a time: the source bits for each from
| the source row by bit field
bbits:
 | f(s,d) = B ^ (d & (A ^ B)); A = m2 ^ (s & (m0 ^ m2)), B = m3 ^ (s & (m1 ^ m3))
 moveq #3,%d1
1: moveq #0,%d2
 btst %d1,%d0
 beq 2f
 moveq #-1,%d2
2: move.l %d2,-(%sp)
 dbra %d1,1b
 move.l (%sp)+,%d1
 move.l (%sp)+,%d2
 move.l (%sp)+,%d3
 move.l (%sp)+,%d4
 | d1..d4: m0 (s1 d1), m1 (s1 d0), m2 (s0 d1), m3 (s0 d0)
 move.l %d3,B_M2(%a6)
 move.l %d4,B_M3(%a6)
 eor.l %d3,%d1
 move.l %d1,B_K02(%a6)
 eor.l %d4,%d2
 move.l %d2,B_K13(%a6)
 moveq #15,%d0
 and.l OP_MODE(%a0),%d0
 move.l %d0,B_OP(%a6)
 move.l OP_W(%a0),%d0
 move.l OP_SX(%a0),%d1
 cmp.l #1,OP_DEPTH(%a0)
 beq 3f
 lsl.l #3,%d0
 lsl.l #3,%d1
3: move.l %d0,B_NB(%a6)
 move.l %d1,B_SB(%a6)
brow:
 move.l B_DROW(%a6),%d0
 bsr rowat
 move.l %d1,B_HB(%a6)
 move.l B_NB(%a6),%d4
 bsr edges
 move.l %d3,%a4
 move.l B_SROW(%a6),%a3
 | d4: bits from x to this longword's start, negative for the first
 moveq #0,%d4
 sub.l %d1,%d4
 moveq #32,%d7
 tst.l B_BACK(%a6)
 beq 1f
 move.l %d3,%d0
 subq.l #1,%d0
 lsl.l #2,%d0
 add.l %d0,%a2
 lsl.l #3,%d0
 add.l %d0,%d4
 moveq #-32,%d7
1: | whole longwords in a loop of their own
 tst.l %d4
 bmi 10f
 moveq #32,%d0
 add.l %d4,%d0
 cmp.l B_NB(%a6),%d0
 bgt 10f
 moveq #4,%d6
 tst.l %d7
 bmi 11f
 move.l B_NB(%a6),%d5
 sub.l %d4,%d5
 lsr.l #5,%d5
 bra 12f
11: move.l %d4,%d5
 lsr.l #5,%d5
 addq.l #1,%d5
 moveq #-4,%d6
12: sub.l %d5,%a4
 move.l B_SB(%a6),%d3
 add.l %d4,%d3
 cmp.l #3,B_OP(%a6)
 bne 14f
13: bfextu (%a3){%d3:32},%d0
 move.l %d0,(%a2)
 add.l %d6,%a2
 add.l %d7,%d3
 subq.l #1,%d5
 bne 13b
 bra 15f
14: bfextu (%a3){%d3:32},%d0
 move.l %d0,%d2
 and.l B_K02(%a6),%d2
 move.l B_M2(%a6),%d1
 eor.l %d1,%d2
 and.l B_K13(%a6),%d0
 move.l B_M3(%a6),%d1
 eor.l %d1,%d0
 eor.l %d0,%d2
 and.l (%a2),%d2
 eor.l %d0,%d2
 move.l %d2,(%a2)
 add.l %d6,%a2
 add.l %d7,%d3
 subq.l #1,%d5
 bne 14b
15: move.l %d3,%d4
 sub.l B_SB(%a6),%d4
 cmp.w #0,%a4
 bne 1b
 bra 7f
10: | d0 lo, d1 hi
 moveq #0,%d0
 tst.l %d4
 bmi 2f
 move.l %d4,%d0
2: moveq #32,%d1
 add.l %d4,%d1
 cmp.l B_NB(%a6),%d1
 ble 3f
 move.l B_NB(%a6),%d1
3: move.l %d1,%d2
 sub.l %d0,%d2
 move.l B_SB(%a6),%d3
 add.l %d0,%d3
 bfextu (%a3){%d3:%d2},%d5
 moveq #32,%d3
 add.l %d4,%d3
 sub.l %d1,%d3
 lsl.l %d3,%d5
 | d6 the edge mask: bits lo-d4 to hi-d4
 sub.l %d4,%d0
 sub.l %d4,%d1
 moveq #-1,%d6
 lsr.l %d0,%d6
 moveq #-1,%d0
 cmp.l #32,%d1
 beq 4f
 lsr.l %d1,%d0
 not.l %d0
 and.l %d0,%d6
4: | d5 source, d1 destination
 move.l %d5,%d2
 and.l B_K02(%a6),%d2
 move.l B_M2(%a6),%d0
 eor.l %d0,%d2
 and.l B_K13(%a6),%d5
 move.l B_M3(%a6),%d0
 eor.l %d0,%d5
 eor.l %d5,%d2
 move.l (%a2),%d1
 and.l %d1,%d2
 eor.l %d5,%d2
 eor.l %d1,%d2
 and.l %d6,%d2
 eor.l %d2,(%a2)
 tst.l %d7
 bmi 5f
 addq.l #4,%a2
 bra 6f
5: subq.l #4,%a2
6: add.l %d7,%d4
 subq.l #1,%a4
 cmp.w #0,%a4
 bne 1b
7: move.l B_DSTEP(%a6),%d0
 add.l %d0,B_DROW(%a6)
 move.l B_SSTEP(%a6),%d0
 add.l %d0,B_SROW(%a6)
 subq.l #1,B_ROWS(%a6)
 bne brow
99: unlk %a6
 rts

| 1-bit copies left to right, rows in the same place in a longword:
| the partial ends by bit field insert, whole longwords unrolled.
b1copy:
 cmp.l #3,%d0
 bne bbits
 tst.l B_BACK(%a6)
 bne bbits
 moveq #3,%d1
 and.l OP_DSTRIDE(%a0),%d1
 bne bbits
 move.l B_DROW(%a6),%d0
 bsr rowat
 move.l %a2,%a5
 move.l OP_W(%a0),%d7
 moveq #32,%d2
 sub.l %d1,%d2
 cmp.l %d7,%d2
 ble 1f
 move.l %d7,%d2
1: move.l %d2,B_N0(%a6)
 sub.l %d2,%d7
 moveq #31,%d5
 and.l %d7,%d5
 lsr.l #5,%d7
 | the rest of 8 enter the unrolled 8 part way, a1 biased to suit;
 | then groups of 8
 moveq #7,%d0
 and.l %d7,%d0
 moveq #8,%d3
 sub.l %d0,%d3
 lsl.l #3,%d3
 lea 3f(%pc),%a4
 add.l %d3,%a4
 lsr.l #1,%d3
 addq.l #4,%d3
 neg.l %d3
 move.l %d3,B_ADJ(%a6)
 lsr.l #3,%d7
 move.l %d7,B_NG(%a6)
 move.l B_SROW(%a6),%a3
 move.l B_ROWS(%a6),%d4
 subq.l #1,%d4
2: move.l %a5,%a2
 move.l OP_SX(%a0),%d3
 bfextu (%a3){%d3:%d2},%d0
 bfins %d0,(%a2){%d1:%d2}
 add.l %d2,%d3
 addq.l #4,%a2
 move.l B_NG(%a6),%d7
 move.l %a3,%a1
 add.l B_ADJ(%a6),%a1
 jmp (%a4)
3:
 .irp n,4,8,12,16,20,24,28,32
 bfextu \n(%a1){%d3:0},%d0
 move.l %d0,(%a2)+
 .endr
 lea 32(%a1),%a1
 dbra %d7,3b
 tst.l %d5
 beq 5f
 bfextu 4(%a1){%d3:%d5},%d0
 bfins %d0,(%a2){#0:%d5}
5: add.l B_DSTEP(%a6),%a5
 add.l B_SSTEP(%a6),%a3
 dbra %d4,2b
 clr.w %d4
 subq.l #1,%d4
 bpl 2b
 unlk %a6
 rts

| 8-bit copies: bytes to a longword boundary, unrolled longwords (lines
| of MOVE16 where source and destination share their place in a line),
| bytes after; from each row's end when B_BACK.
bcopy:
 move.l OP_W(%a0),%d4
 move.l B_ROWS(%a6),%d5
 subq.l #1,%d5
 move.l B_SROW(%a6),%a3
 add.l OP_SX(%a0),%a3
 move.l B_DROW(%a6),%a2
 add.l OP_X(%a0),%a2
 move.l B_SSTEP(%a6),%d6
 move.l B_DSTEP(%a6),%d7
 tst.l B_BACK(%a6)
 bne 20f
 sub.l %d4,%d6
 sub.l %d4,%d7
1: move.l %d4,%d1
2: move.l %a2,%d0
 and.l #3,%d0
 beq 3f
 move.b (%a3)+,(%a2)+
 subq.l #1,%d1
 bne 2b
 bra 7f
3: tst.l do_m16
 beq 4f
 cmp.l #128,%d1
 blo 4f
 move.l %a2,%d0
 move.l %a3,%d2
 eor.l %d2,%d0
 and.l #15,%d0
 bne 4f
10: move.l %a2,%d0
 and.l #15,%d0
 beq 11f
 move.l (%a3)+,(%a2)+
 subq.l #4,%d1
 bra 10b
11: move.l %d1,%d0
 lsr.l #6,%d0
 beq 13f
 subq.l #1,%d0
12: move16pp 3,2
 move16pp 3,2
 move16pp 3,2
 move16pp 3,2
 dbra %d0,12b
13: btst #5,%d1
 beq 14f
 move16pp 3,2
 move16pp 3,2
14: btst #4,%d1
 beq 15f
 move16pp 3,2
15: and.l #15,%d1
4: move.l %d1,%d0
 lsr.l #4,%d0
 beq 6f
 subq.l #1,%d0
5: move.l (%a3)+,(%a2)+
 move.l (%a3)+,(%a2)+
 move.l (%a3)+,(%a2)+
 move.l (%a3)+,(%a2)+
 dbra %d0,5b
6: and.l #15,%d1
 beq 7f
 subq.l #1,%d1
8: move.b (%a3)+,(%a2)+
 dbra %d1,8b
7: add.l %d6,%a3
 add.l %d7,%a2
 dbra %d5,1b
 bra 99b
20: add.l %d4,%a3
 add.l %d4,%a2
 add.l %d4,%d6
 add.l %d4,%d7
21: move.l %d4,%d1
22: move.l %a2,%d0
 and.l #3,%d0
 beq 23f
 move.b -(%a3),-(%a2)
 subq.l #1,%d1
 bne 22b
 bra 26f
23: move.l %d1,%d0
 lsr.l #4,%d0
 beq 25f
 subq.l #1,%d0
24: move.l -(%a3),-(%a2)
 move.l -(%a3),-(%a2)
 move.l -(%a3),-(%a2)
 move.l -(%a3),-(%a2)
 dbra %d0,24b
25: and.l #15,%d1
 beq 26f
 subq.l #1,%d1
27: move.b -(%a3),-(%a2)
 dbra %d1,27b
26: add.l %d6,%a3
 add.l %d7,%a2
 dbra %d5,21b
 bra 99b

| 1-bit copies with source and destination at the same place in a
| byte: the partial bytes at each end as bits, the rest as 8 bits.
| Done from the right when the destination is right of the source.
bbytes:
 move.l OP_X(%a0),%d1
 moveq #0,%d2
 sub.l %d1,%d2
 and.l #7,%d2
 move.l OP_X(%a0),%d3
 add.l OP_W(%a0),%d3
 and.l #7,%d3
 | d2 head bits, d3 tail bits
 move.l OP_X(%a0),%d1
 cmp.l OP_SX(%a0),%d1
 bgt 1f
 bsr bhead
 bsr bmiddle
 bsr btail
 bra 99b
1: bsr btail
 bsr bmiddle
 bsr bhead
 bra 99b
| a copy of the operation at B_TMP, a0 pointing at it; d0-d3 kept
bcopyop:
 move.l %a0,%a1
 lea B_TMP(%a6),%a0
 moveq #OP_SIZE/4-1,%d4
1: move.l (%a1)+,(%a0)+
 dbra %d4,1b
 lea B_TMP(%a6),%a0
 rts
bhead:
 tst.l %d2
 beq 9f
 movem.l %d2-%d3/%a0,-(%sp)
 bsr bcopyop
 move.l %d2,OP_W(%a0)
 bsr blit
 movem.l (%sp)+,%d2-%d3/%a0
9: rts
btail:
 tst.l %d3
 beq 9f
 movem.l %d2-%d3/%a0,-(%sp)
 bsr bcopyop
 move.l OP_W(%a0),%d4
 sub.l %d3,%d4
 add.l %d4,OP_X(%a0)
 add.l %d4,OP_SX(%a0)
 move.l %d3,OP_W(%a0)
 bsr blit
 movem.l (%sp)+,%d2-%d3/%a0
9: rts
bmiddle:
 movem.l %d2-%d3/%a0,-(%sp)
 bsr bcopyop
 move.l OP_X(%a0),%d4
 add.l %d2,%d4
 lsr.l #3,%d4
 move.l %d4,OP_X(%a0)
 move.l OP_SX(%a0),%d4
 add.l %d2,%d4
 lsr.l #3,%d4
 move.l %d4,OP_SX(%a0)
 move.l OP_W(%a0),%d4
 sub.l %d2,%d4
 sub.l %d3,%d4
 lsr.l #3,%d4
 move.l %d4,OP_W(%a0)
 move.l #8,OP_DEPTH(%a0)
 bsr blit
 movem.l (%sp)+,%d2-%d3/%a0
 rts

| ---- line ----
.equ L_PAT,-32
.equ L_TMP,-108
.equ L_SIZE,108

line:
 link %a6,#-L_SIZE
 move.l OP_X(%a0),%d0
 move.l OP_Y(%a0),%d1
 move.l OP_W(%a0),%d2
 move.l OP_H(%a0),%d3
 | the style twice over, so rotating the longword repeats it
 move.l OP_STYLE(%a0),%d7
 and.l #0xffff,%d7
 move.l %d7,%d4
 swap %d4
 or.l %d4,%d7
 | steps: d4 x, d5 y
 moveq #1,%d4
 sub.l %d0,%d2
 bpl 1f
 neg.l %d2
 moveq #-1,%d4
1: moveq #1,%d5
 sub.l %d1,%d3
 bpl 2f
 neg.l %d3
 moveq #-1,%d5
2: tst.l %d3
 beq lhoriz
 tst.l %d2
 beq lvert
 | d2 dx, d3 dy, d6 error, d1 x, a5 y, a3 the row, a4 its step
 move.l %d1,%a5
 muls.l OP_DSTRIDE(%a0),%d1
 add.l OP_DST(%a0),%d1
 move.l %d1,%a3
 move.l OP_DSTRIDE(%a0),%d1
 tst.l %d5
 bpl 3f
 neg.l %d1
3: move.l %d1,%a4
 move.l %d2,%d6
 sub.l %d3,%d6
 move.l %d0,%d1
 | 8 bits, solid, replace or transparent: the pen in d7
 cmp.l #8,OP_DEPTH(%a0)
 bne 4f
 cmp.l #-1,%d7
 bne 4f
 move.l OP_MODE(%a0),%d0
 subq.l #1,%d0
 cmp.l #1,%d0
 bhi 4f
 move.l OP_FG(%a0),%d7
 move.l %d3,%d0
 neg.l %d0
 move.l %d0,%a1
10: move.b %d7,(%a3,%d1.l)
 cmp.l OP_W(%a0),%d1
 bne 11f
 cmp.l OP_H(%a0),%a5
 beq 9f
11: move.l %d6,%d0
 add.l %d0,%d0
 cmp.l %a1,%d0
 ble 12f
 add.l %a1,%d6
 add.l %d4,%d1
12: cmp.l %d2,%d0
 bge 10b
 add.l %d2,%d6
 add.l %a4,%a3
 add.l %d5,%a5
 bra 10b
4: bsr lpixel
 rol.l #1,%d7
 cmp.l OP_W(%a0),%d1
 bne 5f
 cmp.l OP_H(%a0),%a5
 beq 9f
5: move.l %d6,%d0
 add.l %d0,%d0
 neg.l %d3
 cmp.l %d3,%d0
 ble 6f
 add.l %d3,%d6
 add.l %d4,%d1
6: neg.l %d3
 cmp.l %d2,%d0
 bge 4b
 add.l %d2,%d6
 add.l %a4,%a3
 add.l %d5,%a5
 bra 4b
9: unlk %a6
 rts

| one pixel at row a3, x d1, by style bit 31 of d7
lpixel:
 movem.l %d0/%d2-%d3,-(%sp)
 move.l OP_MODE(%a0),%d0
 move.l OP_FG(%a0),%d2
 tst.l %d7
 smi %d3
 cmp.l #1,%d0
 bne 1f
 tst.b %d3
 bne 5f
 move.l OP_BG(%a0),%d2
 bra 5f
1: cmp.l #3,%d0
 beq 3f
 cmp.l #2,%d0
 beq 2f
 not.b %d3
2: tst.b %d3
 beq 8f
 bra 5f
3: tst.b %d3
 beq 8f
 cmp.l #1,OP_DEPTH(%a0)
 beq 4f
 not.b (%a3,%d1.l)
 bra 8f
4: bfchg (%a3){%d1:#1}
 bra 8f
5: cmp.l #1,OP_DEPTH(%a0)
 beq 6f
 move.b %d2,(%a3,%d1.l)
 bra 8f
6: btst #0,%d2
 beq 7f
 bfset (%a3){%d1:#1}
 bra 8f
7: bfclr (%a3){%d1:#1}
8: movem.l (%sp)+,%d0/%d2-%d3
 rts

| a fill of one row: the style laid on x as the pattern word
lhoriz:
 moveq #0,%d6
 moveq #15,%d3
 move.l %d0,%d1
1: btst %d3,%d7
 beq 2f
 moveq #15,%d5
 and.l %d1,%d5
 neg.l %d5
 add.l #15,%d5
 bset %d5,%d6
2: add.l %d4,%d1
 dbra %d3,1b
 lea L_PAT(%a6),%a1
 moveq #15,%d3
3: move.w %d6,(%a1)+
 dbra %d3,3b
 tst.l %d4
 bpl 4f
 sub.l %d2,%d0
4: addq.l #1,%d2
 moveq #1,%d3
 move.l OP_Y(%a0),%d1
 bra lfill
| a fill of one column: the style laid on y as pattern rows
lvert:
 moveq #15,%d6
 lea L_PAT(%a6),%a1
 move.l %d1,%d2
1: moveq #15,%d4
 and.l %d2,%d4
 clr.w (%a1,%d4.l*2)
 btst %d6,%d7
 beq 2f
 move.w #-1,(%a1,%d4.l*2)
2: add.l %d5,%d2
 dbra %d6,1b
 tst.l %d5
 bpl 3f
 sub.l %d3,%d1
3: addq.l #1,%d3
 moveq #1,%d2
| x d0, y d1, w d2, h d3
lfill:
 lea L_TMP(%a6),%a1
 moveq #OP_SIZE/4-1,%d4
1: move.l (%a0)+,(%a1)+
 dbra %d4,1b
 lea L_TMP(%a6),%a0
 move.l #DO_FILL,OP_KIND(%a0)
 move.l %d0,OP_X(%a0)
 move.l %d1,OP_Y(%a0)
 move.l %d2,OP_W(%a0)
 move.l %d3,OP_H(%a0)
 moveq #-1,%d0
 move.l %d0,OP_PMASK(%a0)
 lea L_PAT(%a6),%a1
 move.l %a1,OP_PAT(%a0)
 bsr fill1
 unlk %a6
 rts
