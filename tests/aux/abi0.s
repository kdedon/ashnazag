| abi0.s -- start-up and A/UX system call stubs for abi.c.
|
| sys15(n, a0, d1, a1, d2, a2): trap #15, arguments in registers.
| sys15w: the same with X, N, Z, V, C set, as libc's wait3 calls it.
| sys0(n, args...): trap #0, arguments on the stack.
| All return d0, or -1 with the A/UX errno in errno; d1 goes to sysd1.

	.text
	.globl	_start
_start:
	movel	%sp,%a0
	pea	%a0@(4)
	movel	%a0@,%sp@-
	jsr	main
	moveal	%d0,%a0
	moveq	&1,%d0
	trap	&15
	bra.b	_start

	.globl	sys15
sys15:
	moveml	&0x2020,%sp@-		| d2/a2
	movel	%sp@(12),%d0
	moveal	%sp@(16),%a0
	movel	%sp@(20),%d1
	moveal	%sp@(24),%a1
	movel	%sp@(28),%d2
	moveal	%sp@(32),%a2
	trap	&15
	moveml	%sp@+,&0x0404		| leaves the CCR alone
	bcs.b	Lerr
	movel	%d1,sysd1
	rts

	.globl	sys15w
sys15w:
	moveml	&0x2020,%sp@-
	movel	%sp@(12),%d0
	moveal	%sp@(16),%a0
	movel	%sp@(20),%d1
	moveal	%sp@(24),%a1
	movel	%sp@(28),%d2
	moveal	%sp@(32),%a2
	orib	&0x1f,%cc
	trap	&15
	moveml	%sp@+,&0x0404
	bcs.b	Lerr
	movel	%d1,sysd1
	rts

	.globl	sys0
sys0:
	movel	%sp@(4),%d0
	movel	%sp@+,%sp@		| return address over the number
	trap	&0
	bcc.b	L0ok
	movel	%sp@,%sp@-
	bra.b	Lerr
L0ok:
	movel	%sp@,%sp@-
	movel	%d1,sysd1
	rts

Lerr:
	movel	%d0,errno
	moveq	&-1,%d0
	rts
