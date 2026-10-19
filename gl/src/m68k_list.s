| Display-list replay: vertex records and fans for the Voodoo fifo.
|
| void m68k_lxf(const float m[16], const float (*pos)[3], int n,
|     unsigned long *out, const unsigned long *col, const unsigned short *vn)
|
| m maps object coordinates to window ones before the divide.  Each vertex
| gets a record of x/w, y/w, col[vn[i]], z/w, 1/w, floats with their halves
| swapped as the fifo takes them.  Operation order matches the C.  This
| one, for the 68030 and 6888x, runs the rows one after another.

	.text
	.globl	m68k_lxf
m68k_lxf:
	movm.l	%d2/%a2-%a4,-(%sp)
	fmovm	%fp2-%fp7,-(%sp)
	mov.l	92(%sp),%a0		| m
	mov.l	96(%sp),%a1		| pos
	mov.l	100(%sp),%d1		| n
	mov.l	104(%sp),%a2		| out
	mov.l	108(%sp),%a3		| col
	mov.l	112(%sp),%a4		| vn
	fmov.s	&0x3f800000,%fp6
	subq.l	&1,%d1
	bmi	.L9
.L1:	fmov.s	(%a1)+,%fp2		| x
	fmov.s	(%a1)+,%fp1		| y
	fmov.s	(%a1)+,%fp0		| z
	fmov.x	%fp2,%fp3
	fsglmul.s (%a0),%fp3
	fmov.x	%fp1,%fp7
	fsglmul.s 16(%a0),%fp7
	fadd.x	%fp7,%fp3
	fmov.x	%fp0,%fp7
	fsglmul.s 32(%a0),%fp7
	fadd.x	%fp7,%fp3
	fadd.s	48(%a0),%fp3		| X
	fmov.x	%fp2,%fp4
	fsglmul.s 4(%a0),%fp4
	fmov.x	%fp1,%fp7
	fsglmul.s 20(%a0),%fp7
	fadd.x	%fp7,%fp4
	fmov.x	%fp0,%fp7
	fsglmul.s 36(%a0),%fp7
	fadd.x	%fp7,%fp4
	fadd.s	52(%a0),%fp4		| Y
	fmov.x	%fp2,%fp5
	fsglmul.s 8(%a0),%fp5
	fmov.x	%fp1,%fp7
	fsglmul.s 24(%a0),%fp7
	fadd.x	%fp7,%fp5
	fmov.x	%fp0,%fp7
	fsglmul.s 40(%a0),%fp7
	fadd.x	%fp7,%fp5
	fadd.s	56(%a0),%fp5		| Z
	fsglmul.s 12(%a0),%fp2
	fsglmul.s 28(%a0),%fp1
	fadd.x	%fp1,%fp2
	fsglmul.s 44(%a0),%fp0
	fadd.x	%fp0,%fp2
	fadd.s	60(%a0),%fp2		| W
	fmov.x	%fp6,%fp0
	fsgldiv.x %fp2,%fp0		| 1/W
	fsglmul.x %fp0,%fp3
	fsglmul.x %fp0,%fp4
	fsglmul.x %fp0,%fp5
	fmov.s	%fp3,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	fmov.s	%fp4,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	clr.l	%d2
	mov.w	(%a4)+,%d2
	mov.l	(%a3,%d2.l*4),(%a2)+
	fmov.s	%fp5,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	fmov.s	%fp0,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	dbra	%d1,.L1
	clr.w	%d1			| dbra counts 16 bits
	subq.l	&1,%d1
	bpl	.L1
.L9:	fmovm	(%sp)+,%fp2-%fp7
	movm.l	(%sp)+,%d2/%a2-%a4
	rts

| The same for the 68040: the four rows interleaved so no operation
| waits on the one before it.

	.globl	m68k_lxf40
m68k_lxf40:
	movm.l	%d2-%d6/%a2-%a4,-(%sp)
	fmovm	%fp2-%fp7,-(%sp)
	mov.l	108(%sp),%a0		| m
	mov.l	112(%sp),%a1		| pos
	mov.l	116(%sp),%d1		| n
	mov.l	120(%sp),%a2		| out
	mov.l	124(%sp),%a3		| col
	mov.l	128(%sp),%a4		| vn
	subq.l	&1,%d1
	bmi	.L79
.L71:	fmov.s	(%a1)+,%fp2
	fmov.s	(%a1)+,%fp1
	fmov.s	(%a1)+,%fp0
	fmov.x	%fp2,%fp3
	fsglmul.s (%a0),%fp3
	fmov.x	%fp1,%fp7
	fsglmul.s 16(%a0),%fp7
	fmov.x	%fp2,%fp4
	fsglmul.s 4(%a0),%fp4
	fadd.x	%fp7,%fp3
	fmov.x	%fp1,%fp6
	fsglmul.s 20(%a0),%fp6
	fmov.x	%fp0,%fp7
	fsglmul.s 32(%a0),%fp7
	fadd.x	%fp6,%fp4
	fmov.x	%fp2,%fp5
	fsglmul.s 8(%a0),%fp5
	fadd.x	%fp7,%fp3
	fmov.x	%fp0,%fp6
	fsglmul.s 36(%a0),%fp6
	fmov.x	%fp1,%fp7
	fsglmul.s 24(%a0),%fp7
	fadd.x	%fp6,%fp4
	fsglmul.s 12(%a0),%fp2
	fadd.x	%fp7,%fp5
	fmov.x	%fp0,%fp6
	fsglmul.s 40(%a0),%fp6
	fsglmul.s 28(%a0),%fp1
	fadd.s	48(%a0),%fp3		| X
	fadd.x	%fp6,%fp5
	fadd.x	%fp1,%fp2
	fsglmul.s 44(%a0),%fp0
	fadd.s	52(%a0),%fp4		| Y
	fadd.x	%fp0,%fp2
	fadd.s	56(%a0),%fp5		| Z
	fadd.s	60(%a0),%fp2		| W
	fmov.s	&0x3f800000,%fp0
	fsgldiv.x %fp2,%fp0		| 1/W
	fsglmul.x %fp0,%fp3
	fsglmul.x %fp0,%fp4
	fsglmul.x %fp0,%fp5
	fmov.s	%fp3,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	fmov.s	%fp4,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	clr.l	%d2
	mov.w	(%a4)+,%d2
	mov.l	(%a3,%d2.l*4),(%a2)+
	fmov.s	%fp5,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	fmov.s	%fp0,%d0
	swap	%d0
	mov.l	%d0,(%a2)+
	dbra	%d1,.L71
	clr.w	%d1
	subq.l	&1,%d1
	bpl	.L71
.L79:	fmovm	(%sp)+,%fp2-%fp7
	movm.l	(%sp)+,%d2-%d6/%a2-%a4
	rts

| The 68060 runs integer instructions beside the FPU: the record's colour
| and stores go between FPU operations, each in its own register.

	.globl	m68k_lxf60
m68k_lxf60:
	movm.l	%d2-%d6/%a2-%a4,-(%sp)
	fmovm	%fp2-%fp7,-(%sp)
	mov.l	108(%sp),%a0		| m
	mov.l	112(%sp),%a1		| pos
	mov.l	116(%sp),%d1		| n
	mov.l	120(%sp),%a2		| out
	mov.l	124(%sp),%a3		| col
	mov.l	128(%sp),%a4		| vn
	subq.l	&1,%d1
	bmi	.L89
.L81:	fmov.s	(%a1)+,%fp2
	clr.l	%d2
	fmov.s	(%a1)+,%fp1
	mov.w	(%a4)+,%d2
	fmov.s	(%a1)+,%fp0
	mov.l	(%a3,%d2.l*4),%d6
	fmov.x	%fp2,%fp3
	fsglmul.s (%a0),%fp3
	fmov.x	%fp1,%fp7
	fsglmul.s 16(%a0),%fp7
	fmov.x	%fp2,%fp4
	fsglmul.s 4(%a0),%fp4
	fadd.x	%fp7,%fp3
	fmov.x	%fp1,%fp6
	fsglmul.s 20(%a0),%fp6
	fmov.x	%fp0,%fp7
	fsglmul.s 32(%a0),%fp7
	fadd.x	%fp6,%fp4
	fmov.x	%fp2,%fp5
	fsglmul.s 8(%a0),%fp5
	fadd.x	%fp7,%fp3
	fmov.x	%fp0,%fp6
	fsglmul.s 36(%a0),%fp6
	fmov.x	%fp1,%fp7
	fsglmul.s 24(%a0),%fp7
	fadd.x	%fp6,%fp4
	fsglmul.s 12(%a0),%fp2
	fadd.x	%fp7,%fp5
	fmov.x	%fp0,%fp6
	fsglmul.s 40(%a0),%fp6
	fsglmul.s 28(%a0),%fp1
	fadd.s	48(%a0),%fp3		| X
	fadd.x	%fp6,%fp5
	fadd.x	%fp1,%fp2
	fsglmul.s 44(%a0),%fp0
	fadd.s	52(%a0),%fp4		| Y
	fadd.x	%fp0,%fp2
	fadd.s	56(%a0),%fp5		| Z
	fadd.s	60(%a0),%fp2		| W
	fmov.s	&0x3f800000,%fp0
	fsgldiv.x %fp2,%fp0		| 1/W
	fsglmul.x %fp0,%fp3
	mov.l	%d6,8(%a2)
	fsglmul.x %fp0,%fp4
	fmov.s	%fp3,%d3
	fsglmul.x %fp0,%fp5
	swap	%d3
	mov.l	%d3,(%a2)
	fmov.s	%fp4,%d4
	swap	%d4
	mov.l	%d4,4(%a2)
	fmov.s	%fp5,%d5
	swap	%d5
	mov.l	%d5,12(%a2)
	fmov.s	%fp0,%d0
	swap	%d0
	mov.l	%d0,16(%a2)
	lea	20(%a2),%a2
	dbra	%d1,.L81
	clr.w	%d1
	subq.l	&1,%d1
	bpl	.L81
.L89:	fmovm	(%sp)+,%fp2-%fp7
	movm.l	(%sp)+,%d2-%d6/%a2-%a4
	rts

| unsigned long *m68k_lfan(unsigned long *f, const unsigned short *fan,
|     int nfan, int nv, unsigned long hdr, const unsigned long *vtx,
|     int words, int flat)
|
| Writes nfan fans of nv vertices, each the header and the vertices'
| first words of their records; flat ones take the provoking vertex's
| colour.  fan holds byte offsets of records, the provoking one first.
| With words 3, vertices go without colour and flat points to a
| register write header and the last colour written: the provoking
| colour is written first when it differs.  Returns the next fifo word.

	.globl	m68k_lfan
m68k_lfan:
	movm.l	%d2-%d5/%a2-%a3,-(%sp)
	mov.l	28(%sp),%a0		| f
	mov.l	32(%sp),%a1		| fan
	mov.l	36(%sp),%d1		| nfan
	mov.l	40(%sp),%d3		| nv
	mov.l	44(%sp),%d5		| hdr
	mov.l	48(%sp),%a2		| vtx
	subq.l	&1,%d3
	subq.l	&1,%d1
	bmi	.L29
	mov.l	52(%sp),%d0
	subq.l	&4,%d0
	beq	.L40
	addq.l	&1,%d0
	beq	.L60
	tst.l	56(%sp)
	beq	.L30
| flat, 5 words
.L20:	mov.l	%d5,(%a0)+
	mov.w	(%a1)+,%d0
	mov.l	8(%a2,%d0.w),%d4
	mov.w	%d3,%d2
.L21:	mov.w	(%a1)+,%d0
	lea	(%a2,%d0.w),%a3
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3)+,(%a0)+
	mov.l	%d4,(%a0)+
	addq.l	&4,%a3
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3),(%a0)+
	dbra	%d2,.L21
	dbra	%d1,.L20
	bra	.L29
| smooth, 5 words
.L30:	mov.l	%d5,(%a0)+
	addq.l	&2,%a1
	mov.w	%d3,%d2
.L31:	mov.w	(%a1)+,%d0
	lea	(%a2,%d0.w),%a3
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3),(%a0)+
	dbra	%d2,.L31
	dbra	%d1,.L30
	bra	.L29
.L40:	tst.l	56(%sp)
	beq	.L50
| flat, 4 words
.L41:	mov.l	%d5,(%a0)+
	mov.w	(%a1)+,%d0
	mov.l	8(%a2,%d0.w),%d4
	mov.w	%d3,%d2
.L42:	mov.w	(%a1)+,%d0
	lea	(%a2,%d0.w),%a3
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3),(%a0)+
	mov.l	%d4,(%a0)+
	mov.l	8(%a3),(%a0)+
	dbra	%d2,.L42
	dbra	%d1,.L41
	bra	.L29
| smooth, 4 words
.L50:	mov.l	%d5,(%a0)+
	addq.l	&2,%a1
	mov.w	%d3,%d2
.L51:	mov.w	(%a1)+,%d0
	lea	(%a2,%d0.w),%a3
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3),(%a0)+
	dbra	%d2,.L51
	dbra	%d1,.L50
	bra	.L29
| 3 words, colour written when it changes
.L60:	mov.l	56(%sp),%a3
	mov.l	(%a3)+,%d4
	mov.l	(%a3),%d2
.L61:	mov.w	(%a1)+,%d0
	mov.l	8(%a2,%d0.w),%a3
	cmpa.l	%d2,%a3
	beq	.L63
	mov.l	%d4,(%a0)+
	mov.l	%a3,(%a0)+
	mov.l	%a3,%d2
.L63:	mov.l	%d5,(%a0)+
	mov.w	%d3,%d0
.L64:	mov.w	(%a1)+,%a3
	add.l	%a2,%a3
	mov.l	(%a3)+,(%a0)+
	mov.l	(%a3),(%a0)+
	mov.l	8(%a3),(%a0)+
	dbra	%d0,.L64
	dbra	%d1,.L61
	mov.l	56(%sp),%a3
	mov.l	%d2,4(%a3)
.L29:	mov.l	%a0,%d0
	movm.l	(%sp)+,%d2-%d5/%a2-%a3
	rts

| void m68k_light(const float (*n)[3], int nn, const struct m68k_lt *l,
|     unsigned long *col)
|
| One infinite light without specular: col[i] = R G B A bytes of
| base + max(n.vp, 0) diff, clamped as the C converts floats to bytes.

	.globl	m68k_light
m68k_light:
	movm.l	%d2-%d4/%a2,-(%sp)
	mov.l	20(%sp),%a0		| n
	mov.l	24(%sp),%d1		| nn
	mov.l	28(%sp),%a1		| l
	mov.l	32(%sp),%a2		| col
	mov.l	40(%a1),%d4		| alpha
	subq.l	&1,%d1
	bmi	.L99
.L90:	fmov.s	(%a0)+,%fp0
	fsglmul.s (%a1),%fp0
	fmov.s	(%a0)+,%fp1
	fsglmul.s 4(%a1),%fp1
	fadd.x	%fp1,%fp0
	fmov.s	(%a0)+,%fp1
	fsglmul.s 8(%a1),%fp1
	fadd.x	%fp1,%fp0		| n.vp
	ftst.x	%fp0
	fblt	.L95
	clr.l	%d3
	fmov.x	%fp0,%fp1
	fsglmul.s 12(%a1),%fp1
	fadd.s	24(%a1),%fp1
	fmov.s	%fp1,%d0
	mov.l	%d0,%d2
	sub.l	&0x3f7f0000,%d2
	bcc	.L90h
	fmov.s	%d0,%fp1
	fsglmul.s &0x3f7f0000,%fp1
	fadd.s	&0x47000000,%fp1
	fmov.s	%fp1,%d0
	bra	.L90j
.L90h:	tst.l	%d0
	smi	%d0
	not.b	%d0
.L90j:	lsl.l	&8,%d3
	or.b	%d0,%d3
	fmov.x	%fp0,%fp1
	fsglmul.s 16(%a1),%fp1
	fadd.s	28(%a1),%fp1
	fmov.s	%fp1,%d0
	mov.l	%d0,%d2
	sub.l	&0x3f7f0000,%d2
	bcc	.L91h
	fmov.s	%d0,%fp1
	fsglmul.s &0x3f7f0000,%fp1
	fadd.s	&0x47000000,%fp1
	fmov.s	%fp1,%d0
	bra	.L91j
.L91h:	tst.l	%d0
	smi	%d0
	not.b	%d0
.L91j:	lsl.l	&8,%d3
	or.b	%d0,%d3
	fmov.x	%fp0,%fp1
	fsglmul.s 20(%a1),%fp1
	fadd.s	32(%a1),%fp1
	fmov.s	%fp1,%d0
	mov.l	%d0,%d2
	sub.l	&0x3f7f0000,%d2
	bcc	.L92h
	fmov.s	%d0,%fp1
	fsglmul.s &0x3f7f0000,%fp1
	fadd.s	&0x47000000,%fp1
	fmov.s	%fp1,%d0
	bra	.L92j
.L92h:	tst.l	%d0
	smi	%d0
	not.b	%d0
.L92j:	lsl.l	&8,%d3
	or.b	%d0,%d3
	lsl.l	&8,%d3
	or.b	%d4,%d3
	mov.l	%d3,(%a2)+
	dbra	%d1,.L90
	bra	.L98
.L95:	mov.l	36(%a1),(%a2)+
	dbra	%d1,.L90
.L98:	clr.w	%d1
	subq.l	&1,%d1
	bpl	.L90
.L99:	movm.l	(%sp)+,%d2-%d4/%a2
	rts
