| Vertex transform, clip test and perspective divide for 68030+6888x,
| 68040 and 68060.
|
| unsigned long m68k_xfcl3(const float m[16], const float *from, int stride,
|     float (*clip)[4], float (*ndc)[4], unsigned char *mask, int count)
|
| clip = m * (x, y, z, 1); mask = Mesa clip bits; ndc = clip / w, or
| (0, 0, 0, 1) when clipped.  Returns or-mask | and-mask << 8 (of the
| clipped vertices, from 0x3f) | clipped count << 16.  Rounding and
| operation order match the compiled C, so results agree bit for bit.
|
| The 060 runs one FPU operation at a time but overlaps integer work and
| predicts branches: the clip compares branch only for clipped vertices.

	.text
	.globl	m68k_xfcl3
m68k_xfcl3:
	movm.l	%d2-%d7/%a2-%a4,-(%sp)
	fmovm	%fp2-%fp7,-(%sp)
	mov.l	112(%sp),%a0		| m
	mov.l	116(%sp),%a1		| from
	mov.l	120(%sp),%d1		| stride
	mov.l	124(%sp),%a2		| clip
	mov.l	128(%sp),%a3		| ndc
	mov.l	132(%sp),%a4		| mask
	mov.l	136(%sp),%d7		| count
	clr.l	%d2			| or
	moveq	&0x3f,%d3		| and
	clr.l	%d4			| clipped
	subq.l	&1,%d7
	bmi	.L9
.L1:	fmov.s	(%a1),%fp2		| x
	fmov.s	4(%a1),%fp1		| y
	fmov.s	8(%a1),%fp0		| z
	add.l	%d1,%a1
	fmov.x	%fp2,%fp3
	fsglmul.s (%a0),%fp3
	fmov.x	%fp1,%fp7
	fsglmul.s 16(%a0),%fp7
	fadd.x	%fp7,%fp3
	fmov.x	%fp0,%fp7
	fsglmul.s 32(%a0),%fp7
	fadd.x	%fp7,%fp3
	fadd.s	48(%a0),%fp3		| cx
	fmov.x	%fp2,%fp4
	fsglmul.s 4(%a0),%fp4
	fmov.x	%fp1,%fp7
	fsglmul.s 20(%a0),%fp7
	fadd.x	%fp7,%fp4
	fmov.x	%fp0,%fp7
	fsglmul.s 36(%a0),%fp7
	fadd.x	%fp7,%fp4
	fadd.s	52(%a0),%fp4		| cy
	fmov.x	%fp2,%fp5
	fsglmul.s 8(%a0),%fp5
	fmov.x	%fp1,%fp7
	fsglmul.s 24(%a0),%fp7
	fadd.x	%fp7,%fp5
	fmov.x	%fp0,%fp7
	fsglmul.s 40(%a0),%fp7
	fadd.x	%fp7,%fp5
	fadd.s	56(%a0),%fp5		| cz
	fsglmul.s 12(%a0),%fp2
	fsglmul.s 28(%a0),%fp1
	fadd.x	%fp1,%fp2
	fsglmul.s 44(%a0),%fp0
	fadd.x	%fp0,%fp2
	fadd.s	60(%a0),%fp2		| cw
	fmov.s	%fp3,(%a2)+
	fmov.s	%fp4,(%a2)+
	fmov.s	%fp5,(%a2)+
	fmov.s	%fp2,(%a2)+
	fabs.x	%fp3,%fp6		| |c| > cw: c > cw or c < -cw
	fcmp.x	%fp2,%fp6
	fbogt	.L5
	fabs.x	%fp4,%fp6
	fcmp.x	%fp2,%fp6
	fbogt	.L5
	fabs.x	%fp5,%fp6
	fcmp.x	%fp2,%fp6
	fbogt	.L5
	clr.b	(%a4)+
	fmov.s	&0x3f800000,%fp0
	fsgldiv.x %fp2,%fp0		| 1 / w
	fsglmul.x %fp0,%fp3
	fsglmul.x %fp0,%fp4
	fsglmul.x %fp0,%fp5
	fmov.s	%fp3,(%a3)+
	fmov.s	%fp4,(%a3)+
	fmov.s	%fp5,(%a3)+
	fmov.s	%fp0,(%a3)+
	dbra	%d7,.L1
	bra	.L8
| clipped: the six compares again, building the mask
.L5:	clr.l	%d0
	fneg.x	%fp2,%fp6		| -cw
	fcmp.x	%fp2,%fp3
	fbngt	.L6
	or.b	&1,%d0
.L6:	fcmp.x	%fp6,%fp3
	fbnlt	.L61
	or.b	&2,%d0
.L61:	fcmp.x	%fp2,%fp4
	fbngt	.L62
	or.b	&4,%d0
.L62:	fcmp.x	%fp6,%fp4
	fbnlt	.L63
	or.b	&8,%d0
.L63:	fcmp.x	%fp2,%fp5
	fbngt	.L64
	or.b	&0x20,%d0
.L64:	fcmp.x	%fp6,%fp5
	fbnlt	.L65
	or.b	&0x10,%d0
.L65:	mov.b	%d0,(%a4)+
	or.b	%d0,%d2
	and.b	%d0,%d3
	addq.l	&1,%d4
	clr.l	(%a3)+
	clr.l	(%a3)+
	clr.l	(%a3)+
	mov.l	&0x3f800000,(%a3)+
	dbra	%d7,.L1
.L8:	clr.w	%d7			| dbra counts 16 bits
	subq.l	&1,%d7
	bpl	.L1
.L9:	mov.l	%d4,%d0
	swap	%d0
	lsl.w	&8,%d3
	or.w	%d3,%d0
	or.b	%d2,%d0
	fmovm	(%sp)+,%fp2-%fp7
	movm.l	(%sp)+,%d2-%d7/%a2-%a4
	rts
