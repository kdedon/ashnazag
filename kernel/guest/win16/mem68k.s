| mem68k.s -- memset, memcpy and memmove a long word at a time; the C
| library's go a byte at a time.  calloc, so that it clears with ours.
|
|	void *memset(d, c, n)	void *memcpy(d, s, n)	void *memmove(d, s, n)
|	void *calloc(n, size)

	.text
	.globl	memset, memcpy, memmove, calloc

calloc:	movel	%sp@(4),%d0
	mulul	%sp@(8),%d0
	bvs	1f
	movel	%d0,%sp@-
	jsr	malloc
	movel	%a0,%d0
	beq	2f
	clrl	%sp@-
	movel	%d0,%sp@-
	bsr	memset
	lea	%sp@(12),%sp
	rts
2:	addql	&4,%sp
1:	moveq	&0,%d0
	movel	%d0,%a0
	rts

memset:	movel	%d2,%sp@-
	movel	%sp@(8),%a0
	movel	%sp@(16),%d1
	moveq	&0,%d0
	moveb	%sp@(15),%d0
	cmpl	&16,%d1
	bcs	3f
	mulul	&0x01010101,%d0
1:	movel	%a0,%d2			| to a long word boundary
	andl	&3,%d2
	beq	2f
	moveb	%d0,%a0@+
	subql	&1,%d1
	bra	1b
2:	movel	%d1,%d2
	lsrl	&4,%d2
	andl	&15,%d1
	tstl	%d2
	beq	3f
4:	movel	%d0,%a0@+
	movel	%d0,%a0@+
	movel	%d0,%a0@+
	movel	%d0,%a0@+
	subql	&1,%d2
	bne	4b
3:	tstl	%d1
	beq	6f
5:	moveb	%d0,%a0@+
	subql	&1,%d1
	bne	5b
6:	movel	%sp@(8),%d0
	movel	%d0,%a0
	movel	%sp@+,%d2
	rts

| down when the destination is above the source, so an overlap is safe
memcpy:
memmove:
	movel	%d2,%sp@-
	movel	%sp@(8),%a1
	movel	%sp@(12),%a0
	movel	%sp@(16),%d1
	cmpl	%a0,%a1
	bhi	Ldown
	cmpl	&16,%d1
	bcs	3f
1:	movel	%a1,%d2
	andl	&3,%d2
	beq	2f
	moveb	%a0@+,%a1@+
	subql	&1,%d1
	bra	1b
2:	movel	%d1,%d2
	lsrl	&4,%d2
	andl	&15,%d1
	tstl	%d2
	beq	3f
4:	movel	%a0@+,%a1@+
	movel	%a0@+,%a1@+
	movel	%a0@+,%a1@+
	movel	%a0@+,%a1@+
	subql	&1,%d2
	bne	4b
3:	tstl	%d1
	beq	Lmdone
5:	moveb	%a0@+,%a1@+
	subql	&1,%d1
	bne	5b
	bra	Lmdone
Ldown:	addl	%d1,%a0
	addl	%d1,%a1
	cmpl	&16,%d1
	bcs	3f
1:	movel	%a1,%d2
	andl	&3,%d2
	beq	2f
	moveb	%a0@-,%a1@-
	subql	&1,%d1
	bra	1b
2:	movel	%d1,%d2
	lsrl	&4,%d2
	andl	&15,%d1
	tstl	%d2
	beq	3f
4:	movel	%a0@-,%a1@-
	movel	%a0@-,%a1@-
	movel	%a0@-,%a1@-
	movel	%a0@-,%a1@-
	subql	&1,%d2
	bne	4b
3:	tstl	%d1
	beq	Lmdone
5:	moveb	%a0@-,%a1@-
	subql	&1,%d1
	bne	5b
Lmdone:	movel	%sp@(8),%d0
	movel	%d0,%a0
	movel	%sp@+,%d2
	rts
