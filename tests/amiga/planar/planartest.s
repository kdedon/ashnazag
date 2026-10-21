| planartest -- t_amiga's planar drawing check: lines and a rectangle
| into a two-plane 64x16 BitMap, then words of each plane and the set
| bits, as the name of a file it fails to open (SYS: is read-only).
| A second BitMap gets a patterned line, a template, an area fill and
| text.
.text
.globl _start
_start:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 4.w,%a6
 lea gfxname(%pc),%a1
 moveq #39,%d0
 jsr -552(%a6)
 tst.l %d0
 beq 9f
 move.l %d0,%a5
 lea dosname(%pc),%a1
 moveq #39,%d0
 jsr -552(%a6)
 tst.l %d0
 beq 8f
 move.l %d0,%a4
 move.l %a5,%a6
 moveq #64,%d0
 moveq #16,%d1
 moveq #2,%d2
 moveq #1,%d3
 sub.l %a0,%a0
 jsr -918(%a6)
 tst.l %d0
 beq 7f
 move.l %d0,%a3
 lea rp(%pc),%a2
 move.l %a2,%a1
 jsr -198(%a6)
 move.l %a3,4(%a2)
 move.l %a2,%a1
 moveq #3,%d0
 jsr -342(%a6)
 move.l %a2,%a1
 moveq #0,%d0
 jsr -354(%a6)
 | a horizontal line, a vertical one from its end, a diagonal
 move.l %a2,%a1
 moveq #0,%d0
 moveq #0,%d1
 jsr -240(%a6)
 move.l %a2,%a1
 moveq #15,%d0
 moveq #0,%d1
 jsr -246(%a6)
 move.l %a2,%a1
 moveq #15,%d0
 moveq #7,%d1
 jsr -246(%a6)
 move.l %a2,%a1
 moveq #0,%d0
 moveq #8,%d1
 jsr -240(%a6)
 move.l %a2,%a1
 moveq #7,%d0
 moveq #15,%d1
 jsr -246(%a6)
 | pen 1: plane 0 set, plane 1 cleared
 move.l %a2,%a1
 moveq #1,%d0
 jsr -342(%a6)
 move.l %a2,%a1
 moveq #32,%d0
 moveq #8,%d1
 moveq #47,%d2
 moveq #11,%d3
 jsr -306(%a6)
 jsr -228(%a6)
 | both planes' words at byte offsets 0, 7 rows, 8 rows + 4, 15 rows
 move.w (%a3),%d4
 move.l 8(%a3),%a0
 move.l 12(%a3),%a1
 lea vals(%pc),%a6
 moveq #0,%d0
 bsr word
 move.w %d4,%d0
 mulu #7,%d0
 bsr word
 move.w %d4,%d0
 lsl.w #3,%d0
 addq.w #4,%d0
 bsr word
 move.w %d4,%d0
 mulu #15,%d0
 bsr word
 | the set bits of both planes
 moveq #0,%d1
 move.w %d4,%d2
 lsl.w #4,%d2
 subq.w #1,%d2
1: move.b (%a0,%d2.w),%d0
 bsr bits
 move.b (%a1,%d2.w),%d0
 bsr bits
 dbf %d2,1b
 move.l %d1,(%a6)
 lea fmt(%pc),%a0
 lea vals(%pc),%a1
 lea putch(%pc),%a2
 lea name(%pc),%a3
 move.l 4.w,%a6
 jsr -522(%a6)
 move.l %a4,%a6
 lea name(%pc),%a0
 move.l %a0,%d1
 move.l #1005,%d2
 jsr -30(%a6)
 move.l %d0,%d1
 beq 6f
 jsr -36(%a6)
6: move.l %a5,%a6
 bsr second
 lea rp(%pc),%a0
 move.l 4(%a0),%a0
 jsr -924(%a6)
7: move.l 4.w,%a6
 move.l %a4,%a1
 jsr -414(%a6)
8: move.l 4.w,%a6
 move.l %a5,%a1
 jsr -414(%a6)
9: movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
| the second BitMap; a2 the RastPort again, a3 the BitMap
second:
 movem.l %a2-%a3,-(%sp)
 lea rp(%pc),%a0
 move.l 4(%a0),-(%sp)
 moveq #64,%d0
 moveq #16,%d1
 moveq #2,%d2
 moveq #1,%d3
 sub.l %a0,%a0
 jsr -918(%a6)
 tst.l %d0
 beq 5f
 move.l %d0,%a3
 moveq #64,%d0
 moveq #16,%d1
 jsr -492(%a6)
 lea tmpbuf(%pc),%a0
 move.l %d0,(%a0)
 beq 4f
 lea rp(%pc),%a2
 move.l %a2,%a1
 jsr -198(%a6)
 move.l %a3,4(%a2)
 lea tmpras(%pc),%a0
 move.l tmpbuf(%pc),%a1
 move.l #128,%d0
 jsr -468(%a6)
 lea tmpras(%pc),%a0
 move.l %a0,12(%a2)
 lea area(%pc),%a0
 lea areabuf(%pc),%a1
 moveq #8,%d0
 jsr -282(%a6)
 lea area(%pc),%a0
 move.l %a0,16(%a2)
 move.l %a2,%a1
 moveq #3,%d0
 jsr -342(%a6)
 move.l %a2,%a1
 moveq #0,%d0
 jsr -354(%a6)
 | a line patterned 0xF000 along row 0, x 0-31
 move.w #0xf000,34(%a2)
 bset #0,33(%a2)
 move.b #15,30(%a2)
 move.l %a2,%a1
 moveq #0,%d0
 moveq #0,%d1
 jsr -240(%a6)
 move.l %a2,%a1
 moveq #31,%d0
 moveq #0,%d1
 jsr -246(%a6)
 move.w #0xffff,34(%a2)
 | a template's bits 3-12 at 36,2
 lea tmpl(%pc),%a0
 moveq #3,%d0
 moveq #2,%d1
 move.l %a2,%a1
 moveq #36,%d2
 moveq #2,%d3
 moveq #10,%d4
 moveq #2,%d5
 jsr -36(%a6)
 | pen 1: a square 48,4-55,11 by AreaEnd
 move.l %a2,%a1
 moveq #1,%d0
 jsr -342(%a6)
 move.l %a2,%a1
 moveq #48,%d0
 moveq #4,%d1
 jsr -252(%a6)
 move.l %a2,%a1
 moveq #55,%d0
 moveq #4,%d1
 jsr -258(%a6)
 move.l %a2,%a1
 moveq #55,%d0
 moveq #11,%d1
 jsr -258(%a6)
 move.l %a2,%a1
 moveq #48,%d0
 moveq #11,%d1
 jsr -258(%a6)
 move.l %a2,%a1
 jsr -264(%a6)
 | pen 2: an X from 0,8
 move.l %a2,%a1
 moveq #2,%d0
 jsr -342(%a6)
 move.l %a2,%a1
 moveq #0,%d0
 moveq #8,%d1
 add.w 62(%a2),%d1
 jsr -240(%a6)
 move.l %a2,%a1
 lea xstr(%pc),%a0
 moveq #1,%d0
 jsr -60(%a6)
 | the line's long, the template's words, the square's words, the text's bits
 move.l 8(%a3),%a0
 move.l 12(%a3),%a1
 lea vals(%pc),%a6
 move.l (%a0),(%a6)+
 move.w 20(%a0),(%a6)+
 move.w 28(%a1),(%a6)+
 move.w 38(%a0),(%a6)+
 move.w 38(%a1),(%a6)+
 moveq #0,%d1
 moveq #64,%d2
1: move.b (%a0,%d2.w),%d0
 bsr bits
 addq.w #8,%d2
 cmp.w #128,%d2
 blt 1b
 swap %d1
 moveq #64,%d2
2: move.b (%a1,%d2.w),%d0
 bsr bits
 addq.w #8,%d2
 cmp.w #128,%d2
 blt 2b
 move.l %d1,(%a6)
 lea fmt2(%pc),%a0
 bsr report
 move.l %a5,%a6
 clr.l 12(%a2)
 clr.l 16(%a2)
 move.l tmpbuf(%pc),%a0
 moveq #64,%d0
 moveq #16,%d1
 jsr -498(%a6)
4: move.l %a3,%a0
 jsr -924(%a6)
5: lea rp(%pc),%a0
 move.l (%sp)+,4(%a0)
 movem.l (%sp)+,%a2-%a3
 rts
| vals through fmt at a0 as the name of a file opened for reading
report:
 lea vals(%pc),%a1
 movem.l %a2-%a3,-(%sp)
 lea putch(%pc),%a2
 lea name(%pc),%a3
 move.l 4.w,%a6
 jsr -522(%a6)
 move.l %a4,%a6
 lea name(%pc),%a0
 move.l %a0,%d1
 move.l #1005,%d2
 jsr -30(%a6)
 move.l %d0,%d1
 beq 3f
 jsr -36(%a6)
3: movem.l (%sp)+,%a2-%a3
 rts
| plane 0's word at d0 high, plane 1's low, to (a6)+
word:
 move.w (%a0,%d0.w),(%a6)+
 move.w (%a1,%d0.w),(%a6)+
 rts
| d1 += the bits of d0.b
bits:
 moveq #7,%d3
2: lsr.b #1,%d0
 bcc 3f
 addq.l #1,%d1
3: dbf %d3,2b
 rts
putch:
 move.b %d0,(%a3)+
 rts
.balign 4
vals: .long 0, 0, 0, 0, 0
tmpbuf: .long 0
rp: .space 100
tmpras: .space 8
area: .space 24
areabuf: .space 40
tmpl: .word 0xa5f0, 0x0f0f
gfxname: .asciz "graphics.library"
dosname: .asciz "dos.library"
fmt: .asciz "SYS:planar %lx %lx %lx %lx %ld"
fmt2: .asciz "SYS:planar2 %lx %lx %lx %lx"
xstr: .asciz "X"
name: .space 80
.balign 2
