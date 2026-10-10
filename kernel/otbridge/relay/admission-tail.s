	.text
	.even
	.globl otr_admission_tail,otr_admission_lo
otr_admission_lo:
	.extern otr_admit_configuration
	.globl otr_admission_jump,otr_admission_helper

otr_admission_tail:
	movem.l %d0-%d7/%a0-%a6,-(%sp)
	move.w %ccr,-(%sp)
	move.l 70(%sp),-(%sp)
	.word 0x4eb9
	.long otr_admission_helper
	addq.l #4,%sp
	move.w (%sp)+,%ccr
	movem.l (%sp)+,%d0-%d7/%a0-%a6
	link.w %a6,#0
	movem.l %a2-%a4,-(%sp)
otr_admission_jump:
	.word 0x4ef9
	.long 0
otr_admission_helper:
	jmp otr_admit_configuration
	.long 0x4f544144
	.word 1
	.long 0
