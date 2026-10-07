| hstart.s -- the harness entry: a stack, h_main, then the halt port.

	.section .text.start,"ax"
	.globl	h_start
h_start:
	movew	&0x2000,%sr
	lea	0x00f00000,%sp
	jsr	h_main
	movel	%d0,0x01000004
L1:	bra.b	L1
