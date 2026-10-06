| The card's drawing functions at a fixed address for t_amiga: a table of
| their addresses, a caller that loads registers, a stand-in for P96's own.
.include "p96offsets.inc"
.include "rtgabi.inc"
.text
 .long fill_rect, invert_rect, blit_rect, blit_complete, blit_template
 .long callreg, deflt, deflt_count
| callreg(fn, regs): d0-d7/a0-a2 from regs[0..10]; after, regs[11..21]
callreg:
 movem.l %d2-%d7/%a2-%a5,-(%sp)
 move.l 44(%sp),%a4
 move.l 48(%sp),%a5
 movem.l (%a5),%d0-%d7/%a0-%a2
 jsr (%a4)
 movem.l %d0-%d7/%a0-%a2,44(%a5)
 movem.l (%sp)+,%d2-%d7/%a2-%a5
 rts
deflt:
 addq.l #1,deflt_count
 rts
ring:
 move.l #1,0xf7fffc
 rts
.include "draw.inc"
 .balign 4
deflt_count: .long 0
