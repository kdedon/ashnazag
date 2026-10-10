| i86deca.s -- the 8086 decoder for the 68k.
|
|	int i86dec(cs, ip, in)  char *cs;  i16 ip;  struct i86in *in;
|
| Same answers as i86dec.c, which stays the reference and decodes the
| prefixed forms and those whose tail needs the opcode (form bit
| 0x8000 in i86tab).  Fetches wrap at 16 bits.
|
|	a0 cs	a1 the opcode's record	a2 in
|	d2 ip	d3 bytes so far		d4 the form

| struct i86in
I_LEN	=	0
I_OP	=	1
I_FL	=	2
I_W	=	3
I_X	=	4
I_MOD	=	5
I_REG	=	6
I_RM	=	7
I_SEG	=	8
I_DISP	=	10
I_IMM	=	12
I_IMM2	=	14

	.text
	.globl	i86dec

i86dec:
	moveml	%d2-%d4/%a2,%sp@-
	movel	%sp@(20),%a0		| cs
	movel	%sp@(24),%d2		| ip
	movel	%sp@(28),%a2		| in
	andl	&0xffff,%d2
	moveq	&0,%d0
	moveb	%a0@(0,%d2:l),%d0
	movel	%d0,%d1
	lslw	&2,%d1
	lea	i86tab,%a1
	addw	%d1,%a1
	movew	%a1@(2),%d4
	bmi	Lc			| the C decoder's
	moveb	%a1@,%a2@(I_OP)
	moveb	%a1@(1),%a2@(I_X)
	clrl	%a2@(I_MOD)		| mod reg rm 0, then seg
	moveb	&3,%a2@(I_SEG)		| DS
	clrw	%a2@(I_DISP)
	clrl	%a2@(I_IMM)		| imm and imm2
	movew	%d4,%d1
	lsrw	&2,%d1
	andb	&0xc0,%d1		| D_I, D_D as IN_IMM, IN_DIR
	moveb	%d1,%a2@(I_FL)
	movew	%d4,%d1
	lsrw	&8,%d1
	lsrw	&2,%d1
	andb	&1,%d1
	moveb	%d1,%a2@(I_W)
	moveq	&1,%d3
	btst	&3,%d4			| D_M
	beq	Ln3
	bsr	Lbyte			| the mod r/m byte
	moveb	%d0,%d1
	lsrb	&6,%d1
	moveb	%d1,%a2@(I_MOD)
	moveb	%d0,%d1
	lsrb	&3,%d1
	andb	&7,%d1
	moveb	%d1,%a2@(I_REG)
	andb	&7,%d0
	moveb	%d0,%a2@(I_RM)
	orb	&1,%a2@(I_FL)		| IN_MODRM
	moveb	%a2@(I_MOD),%d1
	cmpib	&3,%d1
	beq	Ln3
	orb	&2,%a2@(I_FL)		| IN_MEM
	tstb	%d1
	bne	1f
	cmpib	&6,%d0
	bne	Lseg
	bsr	Lword			| mod 0, rm 6: a direct address
	movew	%d0,%a2@(I_DISP)
	bra	Lseg
1:	cmpib	&1,%d1
	bne	2f
	bsr	Lbyte			| mod 1: a signed byte
	extw	%d0
	movew	%d0,%a2@(I_DISP)
	bra	Lseg
2:	bsr	Lword			| mod 2: a word
	movew	%d0,%a2@(I_DISP)
Lseg:	moveb	%a2@(I_RM),%d0		| SS for BP-based forms
	cmpib	&2,%d0
	beq	1f
	cmpib	&3,%d0
	beq	1f
	cmpib	&6,%d0
	bne	Ln3
	tstb	%a2@(I_MOD)
	beq	Ln3
1:	moveb	&2,%a2@(I_SEG)
Ln3:	btst	&5,%d4			| D_3: a register named by the opcode
	beq	Lx
	moveb	&3,%a2@(I_MOD)
	btst	&6,%d4			| D_L
	beq	Lx
	moveq	&0,%d0
	moveb	%a0@(0,%d2:l),%d0
	andb	&7,%d0
	moveb	%d0,%a2@(I_RM)
Lx:	btst	&4,%d4			| D_X: .x is the reg field
	beq	Limm
	moveb	%a2@(I_REG),%a2@(I_X)
Limm:	moveq	&7,%d1
	andw	%d4,%d1
	beq	Ldone
	subqw	&1,%d1
	addw	%d1,%d1
	lea	Lktab,%a1
	movew	%a1@(0,%d1:w),%d1
	jmp	%a1@(0,%d1:w)
Lktab:	.word	Lib-Lktab, Liw-Lktab, Lsb-Lktab, Lj8-Lktab
	.word	Ljw-Lktab, Lda-Lktab, Lfp-Lktab
Lib:	bsr	Lbyte
	movew	%d0,%a2@(I_IMM)
	bra	Ldone
Liw:	bsr	Lword
	movew	%d0,%a2@(I_IMM)
	bra	Ldone
Lsb:	bsr	Lbyte
	extw	%d0
	movew	%d0,%a2@(I_IMM)
	bra	Ldone
Lj8:	bsr	Lbyte
	extw	%d0
	addw	%d2,%d0
	addw	%d3,%d0
	movew	%d0,%a2@(I_DISP)
	bra	Ldone
Ljw:	bsr	Lword
	addw	%d2,%d0
	addw	%d3,%d0
	movew	%d0,%a2@(I_DISP)
	bra	Ldone
Lda:	orb	&3,%a2@(I_FL)		| IN_MODRM | IN_MEM
	moveb	&6,%a2@(I_RM)
	bsr	Lword
	movew	%d0,%a2@(I_DISP)
	bra	Ldone
Lfp:	bsr	Lword
	movew	%d0,%a2@(I_IMM)
	bsr	Lword
	movew	%d0,%a2@(I_IMM2)
Ldone:	moveb	%d3,%a2@(I_LEN)
	moveq	&0,%d0
	moveb	%d3,%d0
	moveml	%sp@+,%d2-%d4/%a2
	rts

Lc:	moveml	%sp@+,%d2-%d4/%a2
	jmp	i86decc

| d0 = the byte at ip + d3, which then counts it; d1 is lost
Lbyte:
	movel	%d2,%d1
	addw	%d3,%d1
	moveq	&0,%d0
	moveb	%a0@(0,%d1:l),%d0
	addqw	&1,%d3
	rts

| d0 = the little-endian word at ip + d3, which then counts it
Lword:
	bsr	Lbyte
	movew	%d0,%sp@-
	bsr	Lbyte
	lslw	&8,%d0
	orw	%sp@+,%d0
	rts
