	.text
	.even
	.globl otr_admission_hold,otr_admission_release
	.extern __aslm_ctx
otr_admission_hold:
	link.w %a6,#0
	move.l %a2,-(%sp)
	move.l __aslm_ctx,%a2
	move.l %a2,%d0
	beq.s hold_missing
	moveq #1,%d0
	move.l %d0,-(%sp)
	move.l 8(%a6),-(%sp)
	move.l %a2,-(%sp)
	move.l (%a2),%a0
	move.l 64(%a0),%a0
	jsr (%a0)
	lea 12(%sp),%sp
	ext.l %d0
	bra.s hold_done
hold_missing:
	moveq #-50,%d0
hold_done:
	move.l (%sp)+,%a2
	unlk %a6
	rts
otr_admission_release:
	link.w %a6,#0
	move.l %a2,-(%sp)
	move.l __aslm_ctx,%a2
	move.l %a2,%d0
	beq.s release_done
	move.l 8(%a6),-(%sp)
	move.l %a2,-(%sp)
	move.l (%a2),%a0
	move.l 68(%a0),%a0
	jsr (%a0)
	addq.l #8,%sp
release_done:
	move.l (%sp)+,%a2
	unlk %a6
	rts
