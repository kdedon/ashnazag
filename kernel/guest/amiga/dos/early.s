.text
/* After romboot, before the boot menu: bind the RTG board. */
.globl early_resident
early_resident:
 .word 0x4afc
 .long early_resident,early_name
 .byte 1,1,0,-45
 .long early_name,early_name,early_entry
early_name: .asciz "container.early"
.balign 2
/* After the boot menu returns. */
menu_resident:
 .word 0x4afc
 .long menu_resident,menu_name
 .byte 1,1,0,-51
 .long menu_name,menu_name,menu_entry
menu_name: .asciz "container.menu"
.balign 4
early_entry:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 jsr early_init
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
menu_entry:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 jsr early_menu
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts

/*
 * U xcall(a6, function, regs): regs d0-d7, a0-a5 in and out; returns d0.
 * The function is entered through rts, so every register is free for it.
 */
.globl xcall
xcall:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 48(%sp),%a6
 move.l 56(%sp),%a0
 move.l %a0,-(%sp)
 pea 1f(%pc)
 move.l 60(%sp),-(%sp)
 movem.l (%a0),%d0-%d7/%a0-%a5
 rts
1: move.l %a0,-(%sp)
 move.l 4(%sp),%a0
 movem.l %d0-%d7,(%a0)
 move.l (%sp)+,32(%a0)
 movem.l %a1-%a5,36(%a0)
 addq.l #4,%sp
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 rts

/*
 * The stand-in libraries' vectors.  Each pushes the caller's a6 and its
 * offset; the real library gets the call once it exists, otherwise
 * early_emulate(base, offset, d0-d7/a0-a6).
 */
.equ F_REAL, 34
.balign 4
vectors:
 .set k,1
 .rept 200
 move.l %a6,-(%sp)
 move.l #k*6,-(%sp)
 bra.w fwd
 .set k,k+1
 .endr
.globl fake_vectors
.balign 4
fake_vectors:
 .set k,0
 .rept 200
 .long vectors+k*12
 .set k,k+1
 .endr
 .long -1
fwd:
 cmp.l #24,(%sp)
 bls 2f
 tst.l F_REAL(%a6)
 bne 1f
 movem.l %d0-%d1/%a0-%a1,-(%sp)
 move.l %a6,-(%sp)
 jsr early_resolve
 addq.l #4,%sp
 movem.l (%sp)+,%d0-%d1/%a0-%a1
 tst.l F_REAL(%a6)
 bne 1f
2: movem.l %d0-%d7/%a0-%a6,-(%sp)
 move.l %sp,-(%sp)
 move.l 64(%sp),-(%sp)
 move.l %a6,-(%sp)
 jsr early_emulate
 lea 12(%sp),%sp
 movem.l (%sp)+,%d0-%d7/%a0-%a6
 addq.l #4,%sp
 move.l (%sp)+,%a6
 rts
/* [offset][a6][ret] becomes [function][restore][a6][ret] */
1: move.l (%sp),-(%sp)
 move.l #3f,4(%sp)
 move.l %a0,-(%sp)
 move.l F_REAL(%a6),%a6
 move.l %a6,%a0
 sub.l 4(%sp),%a0
 move.l %a0,4(%sp)
 move.l (%sp)+,%a0
 rts
3: move.l (%sp)+,%a6
 rts

/* intuition's OpenScreen and OpenScreenTagList, entered from trampolines that pushed the state */
.globl os_stub, osl_stub
os_stub:
 sub.l %a1,%a1
osl_stub:
 movem.l %d0-%d7/%a0-%a6,-(%sp)
 move.l %sp,-(%sp)
 move.l 64(%sp),-(%sp)
 jsr early_openscreen
 addq.l #8,%sp
 movem.l (%sp)+,%d0-%d7/%a0-%a6
 addq.l #4,%sp
 rts

/* DisplayAlert(d0 number, a0 text, d1 height); TimedDisplayAlert with a1 frames */
.globl alert_stub, talert_stub
alert_stub:
 sub.l %a1,%a1
talert_stub:
 movem.l %d1-%d7/%a0-%a6,-(%sp)
 move.l %a1,-(%sp)
 move.l %d1,-(%sp)
 move.l %a0,-(%sp)
 move.l %d0,-(%sp)
 jsr early_alert
 lea 16(%sp),%sp
 movem.l (%sp)+,%d1-%d7/%a0-%a6
 rts

/* intuition's SetPrefs, from a trampoline that pushed the state */
.globl sp_stub
sp_stub:
 movem.l %d0-%d7/%a0-%a6,-(%sp)
 move.l %sp,-(%sp)
 move.l 64(%sp),-(%sp)
 jsr early_setprefs
 addq.l #8,%sp
 movem.l (%sp)+,%d0-%d7/%a0-%a6
 addq.l #4,%sp
 rts
