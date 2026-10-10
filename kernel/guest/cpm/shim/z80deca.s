| z80deca.s -- the 8080/Z80 decoder for the 68k.
|
|	int z80dec(m, pc, in)  char *m;  z16 pc;  struct z80in *in;
|
| Same answers as z80dec.c, which stays the reference and decodes the
| four prefix bytes here too.  The base map is z80tab: per opcode len,
| op, fl, x, y and how the operand follows.  Fetches wrap at 16 bits.

	.text
	.globl	z80dec

z80dec:
	moveml	%d2/%a2,%sp@-
	movel	%sp@(12),%a0		| m
	movel	%sp@(16),%d2		| pc
	movel	%sp@(20),%a2		| in
	andl	&0xffff,%d2
	moveq	&0,%d1
	moveb	%a0@(0,%d2:l),%d1
	lslw	&3,%d1
	lea	z80tab,%a1
	addw	%d1,%a1
	moveb	%a1@(5),%d0		| how the operand follows
	cmpib	&4,%d0
	beq	Lpfx
	movel	%a1@,%a2@		| len op fl x
	moveb	%a1@(4),%a2@(4)		| y
	clrb	%a2@(5)			| pfx
	clrb	%a2@(6)			| sub
	clrw	%a2@(10)		| disp
	tstb	%d0
	bne	L1
	clrw	%a2@(8)
	bra	Ldone
L1:	addqw	&1,%d2
	moveq	&0,%d1
	moveb	%a0@(0,%d2:l),%d1
	cmpib	&2,%d0
	bhi	Lword
	beq	Lrel
	movew	%d1,%a2@(8)		| a byte
	bra	Ldone
Lrel:	extw	%d1			| pc + 2 + d
	addqw	&1,%d2
	addw	%d2,%d1
	movew	%d1,%a2@(8)
	bra	Ldone
Lword:	addqw	&1,%d2
	moveb	%a0@(0,%d2:l),%a2@(8)
	moveb	%d1,%a2@(9)
Ldone:	moveq	&0,%d0
	moveb	%a2@,%d0
	moveml	%sp@+,%d2/%a2
	rts
Lpfx:	moveml	%sp@+,%d2/%a2
	jmp	z80decc
