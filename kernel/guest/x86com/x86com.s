| x86com.s -- what the 68k x86 run loops share: conditions and flags
| from a lazy record, parity, string moves and fills.
|
| A lazy record in registers:
|	d0 result	d1 class (0 none, 1 add, 2 sub, 3 logic, 4 inc, 5 dec)
|	d2 size (0 byte, 1 word, 2 long)	d3 a	d4 b	a1 the flags word
| An add or sub record's carry in is what r - a - b (a - b - r) leaves,
| so ADC and SBB need no field of their own.  INC and DEC take CF from
| the flags word.

	.text
	.globl	x86c_cond, x86c_flags, x86c_parity, x86c_alu, x86c_movs, x86c_stos

| d0 = 1 when condition a0 (0-15, the Jcc order) holds, else 0.
| d1-d4 and a0 are lost.
x86c_cond:
	cmpw	&4,%a0
	bcs	Lcc
	cmpw	&5,%a0
	bhi	Lcc
	tstl	%d1			| E, NE: the result is zero
	bne	1f
	movel	%a1,%d0
	lsrw	&6,%d0
	bra	5f
1:	cmpl	&1,%d2
	bcs	2f
	beq	3f
	tstl	%d0
	bra	4f
2:	tstb	%d0
	bra	4f
3:	tstw	%d0
4:	seq	%d0
5:	movel	%a0,%d1
	eorb	%d1,%d0
	andl	&1,%d0
	rts
Lcc:	cmpw	&10,%a0
	beq	Lpar
	cmpw	&11,%a0
	beq	Lpar
	movel	%a0,%sp@-
	bsr	Lccr
	movel	%sp@+,%d1
	lslw	&2,%d1
	lea	Lstab,%a0
	addw	%d1,%a0
	movew	%d0,%cc
	jmp	%a0@
Lstab:	svs	%d0			| O
	bras	Lst
	svc	%d0			| NO
	bras	Lst
	scs	%d0			| B
	bras	Lst
	scc	%d0			| AE
	bras	Lst
	seq	%d0			| E
	bras	Lst
	sne	%d0			| NE
	bras	Lst
	sls	%d0			| BE
	bras	Lst
	shi	%d0			| A
	bras	Lst
	smi	%d0			| S
	bras	Lst
	spl	%d0			| NS
	bras	Lst
	sf	%d0			| P, NP: not here
	bras	Lst
	sf	%d0
	bras	Lst
	slt	%d0			| L
	bras	Lst
	sge	%d0			| GE
	bras	Lst
	sle	%d0			| LE
	bras	Lst
	sgt	%d0			| G
Lst:	andl	&1,%d0
	rts
Lpar:	tstl	%d1			| PF
	bne	1f
	movel	%a1,%d0
	lsrw	&2,%d0
	bra	2f
1:	lea	x86c_parity,%a1
	andw	&0xff,%d0
	moveb	%a1@(0,%d0:w),%d0
	lsrb	&2,%d0
2:	movel	%a0,%d1
	eorb	%d1,%d0			| odd cc: NP
	andl	&1,%d0
	rts

| d0 = the flags word with the record's arithmetic flags.  d1-d4, a0
| are lost.
x86c_flags:
	tstl	%d1
	bne	1f
	movel	%a1,%d0
	rts
1:	movel	%d0,%sp@-		| r, for PF
	movel	%d3,%sp@-		| a ^ b ^ r, for AF
	eorl	%d4,%sp@
	eorl	%d0,%sp@
	cmpl	&3,%d1
	bne	2f
	clrl	%sp@
2:	bsr	Lccr
	movel	%a1,%d1
	andw	&0xf72a,%d1		| CF PF AF ZF SF OF
	andw	&15,%d0
	addw	%d0,%d0
	lea	Lc2f,%a0
	orw	%a0@(0,%d0:w),%d1
	movel	%sp@+,%d0
	andw	&0x10,%d0
	orw	%d0,%d1
	movel	%sp@+,%d0
	andw	&0xff,%d0
	lea	x86c_parity,%a0
	moveq	&0,%d2
	moveb	%a0@(0,%d0:w),%d2
	orw	%d2,%d1
	movel	%d1,%d0
	rts

| the 68k's N Z V C -> SF ZF OF CF
Lc2f:	.word	0x000, 0x001, 0x800, 0x801, 0x040, 0x041, 0x840, 0x841
	.word	0x080, 0x081, 0x880, 0x881, 0x0c0, 0x0c1, 0x8c0, 0x8c1

| d0 = the 68k condition codes of the record: N Z V C as x86 SF ZF OF
| CF.  d1, d3 and a0 are lost.
Lccr:
	tstl	%d1
	bne	1f
	movel	%a1,%d1			| the flags word
	movel	%d1,%d0
	lsrb	&4,%d0			| SF ZF -> N Z
	andw	&0xc,%d0
	btst	&0,%d1
	beq	2f
	orb	&1,%d0
2:	btst	&11,%d1
	beq	3f
	orb	&2,%d0
3:	rts
1:	subql	&1,%d1			| (class - 1) * 3 + size
	movel	%d1,%a0
	addl	%d1,%d1
	addl	%a0,%d1
	addl	%d2,%d1
	addw	%d1,%d1
	lea	Lctab,%a0
	movew	%a0@(0,%d1:w),%d1
	jmp	%a0@(0,%d1:w)
Lctab:	.word	Lcab-Lctab, Lcaw-Lctab, Lcal-Lctab
	.word	Lcsb-Lctab, Lcsw-Lctab, Lcsl-Lctab
	.word	Lclb-Lctab, Lclw-Lctab, Lcll-Lctab
	.word	Lcib-Lctab, Lciw-Lctab, Lcil-Lctab
	.word	Lcdb-Lctab, Lcdw-Lctab, Lcdl-Lctab

| redo the operation at its size, X the carry in and Z set beforehand
Lcab:	movel	%d0,%d1
	subb	%d3,%d1
	subb	%d4,%d1
	beq	1f
	movew	&0x14,%cc
	bra	2f
1:	movew	&4,%cc
2:	addxb	%d4,%d3
	movew	%cc,%d0
	rts
Lcsb:	movel	%d3,%d1
	subb	%d4,%d1
	subb	%d0,%d1
	beq	1f
	movew	&0x14,%cc
	bra	2f
1:	movew	&4,%cc
2:	subxb	%d4,%d3
	movew	%cc,%d0
	rts
Lclb:	tstb	%d0
	movew	%cc,%d0
	rts
Lcib:	addqb	&1,%d3
	bra	1f
Lcdb:	subqb	&1,%d3
1:	movew	%cc,%d0
	andw	&0xe,%d0
	movel	%a1,%d1
	andw	&1,%d1
	orw	%d1,%d0
	rts
Lcaw:	movel	%d0,%d1
	subw	%d3,%d1
	subw	%d4,%d1
	beq	1f
	movew	&0x14,%cc
	bra	2f
1:	movew	&4,%cc
2:	addxw	%d4,%d3
	movew	%cc,%d0
	rts
Lcsw:	movel	%d3,%d1
	subw	%d4,%d1
	subw	%d0,%d1
	beq	1f
	movew	&0x14,%cc
	bra	2f
1:	movew	&4,%cc
2:	subxw	%d4,%d3
	movew	%cc,%d0
	rts
Lclw:	tstw	%d0
	movew	%cc,%d0
	rts
Lciw:	addqw	&1,%d3
	bra	1f
Lcdw:	subqw	&1,%d3
1:	movew	%cc,%d0
	andw	&0xe,%d0
	movel	%a1,%d1
	andw	&1,%d1
	orw	%d1,%d0
	rts
Lcal:	movel	%d0,%d1
	subl	%d3,%d1
	subl	%d4,%d1
	beq	1f
	movew	&0x14,%cc
	bra	2f
1:	movew	&4,%cc
2:	addxl	%d4,%d3
	movew	%cc,%d0
	rts
Lcsl:	movel	%d3,%d1
	subl	%d4,%d1
	subl	%d0,%d1
	beq	1f
	movew	&0x14,%cc
	bra	2f
1:	movew	&4,%cc
2:	subxl	%d4,%d3
	movew	%cc,%d0
	rts
Lcll:	tstl	%d0
	movew	%cc,%d0
	rts
Lcil:	addql	&1,%d3
	bra	1f
Lcdl:	subql	&1,%d3
1:	movew	%cc,%d0
	andw	&0xe,%d0
	movel	%a1,%d1
	andw	&1,%d1
	orw	%d1,%d0
	rts

| ALU operation d0 (ADD OR ADC SBB AND SUB XOR CMP) of a d3 and b d4,
| carry in d2 for ADC and SBB: d0 = the result, unmasked; d1 = its
| class.  a0 is lost.
x86c_alu:
	addw	%d0,%d0
	lea	Latab,%a0
	movew	%a0@(0,%d0:w),%d0
	jmp	%a0@(0,%d0:w)
Latab:	.word	Laadd-Latab, Laor-Latab, Laadc-Latab, Lasbb-Latab
	.word	Laand-Latab, Lasub-Latab, Laxor-Latab, Lasub-Latab
Laadc:	movel	%d3,%d0
	addl	%d4,%d0
	addl	%d2,%d0
	moveq	&1,%d1
	rts
Laadd:	movel	%d3,%d0
	addl	%d4,%d0
	moveq	&1,%d1
	rts
Lasbb:	movel	%d3,%d0
	subl	%d4,%d0
	subl	%d2,%d0
	moveq	&2,%d1
	rts
Lasub:	movel	%d3,%d0
	subl	%d4,%d0
	moveq	&2,%d1
	rts
Laor:	movel	%d3,%d0
	orl	%d4,%d0
	moveq	&3,%d1
	rts
Laand:	movel	%d3,%d0
	andl	%d4,%d0
	moveq	&3,%d1
	rts
Laxor:	movel	%d3,%d0
	eorl	%d4,%d0
	moveq	&3,%d1
	rts

| copy d0 (1-65536) units of d1 (1, 2, 4) bytes from a0 to a1, up
| (d2 0) or down, each unit read before it is written.  d0-d2, a0, a1
| are lost.
x86c_movs:
	tstl	%d2
	bne	Lmdown
	cmpl	&1,%d1
	beq	1f
	lsll	&1,%d0
	cmpl	&2,%d1
	beq	1f
	lsll	&1,%d0			| d0 bytes
1:	cmpl	%a0,%a1			| a destination inside the source
	bls	2f			| repeats it: a unit at a time
	movel	%a0,%d2
	addl	%d0,%d2
	cmpl	%d2,%a1
	bcc	2f
	cmpl	&1,%d1
	beq	Lmb
	cmpl	&2,%d1
	beq	Lmw
	lsrl	&2,%d0
	bra	Lml
2:	movel	%d0,%d2			| longs, then the tail
	lsrl	&2,%d2
	beq	4f
	subql	&1,%d2
3:	movel	%a0@+,%a1@+
	dbra	%d2,3b
	clrw	%d2
	subql	&1,%d2
	bcc	3b
4:	andw	&3,%d0
	beq	6f
	subqw	&1,%d0
5:	moveb	%a0@+,%a1@+
	dbra	%d0,5b
6:	rts
Lmb:	subql	&1,%d0
1:	moveb	%a0@+,%a1@+
	dbra	%d0,1b
	clrw	%d0
	subql	&1,%d0
	bcc	1b
	rts
Lmw:	lsrl	&1,%d0
	subql	&1,%d0
1:	movew	%a0@+,%a1@+
	dbra	%d0,1b
	rts
Lml:	subql	&1,%d0
1:	movel	%a0@+,%a1@+
	dbra	%d0,1b
	rts
Lmdown:	addl	%d1,%a0
	addl	%d1,%a1
	subql	&1,%d0
	cmpl	&2,%d1
	beq	2f
	bhi	3f
1:	moveb	%a0@-,%a1@-
	dbra	%d0,1b
	rts
2:	movew	%a0@-,%a1@-
	dbra	%d0,2b
	rts
3:	movel	%a0@-,%a1@-
	dbra	%d0,3b
	rts

| store d3 (in memory order) d0 (1-65536) times as units of d1 bytes
| from a1, up (d2 0) or down.  d0, a1 are lost.
x86c_stos:
	subql	&1,%d0
	tstl	%d2
	bne	Lsdown
	cmpl	&2,%d1
	beq	2f
	bhi	3f
1:	moveb	%d3,%a1@+
	dbra	%d0,1b
	rts
2:	movew	%d3,%a1@+
	dbra	%d0,2b
	rts
3:	movel	%d3,%a1@+
	dbra	%d0,3b
	rts
Lsdown:	addl	%d1,%a1
	cmpl	&2,%d1
	beq	2f
	bhi	3f
1:	moveb	%d3,%a1@-
	dbra	%d0,1b
	rts
2:	movew	%d3,%a1@-
	dbra	%d0,2b
	rts
3:	movel	%d3,%a1@-
	dbra	%d0,3b
	rts

| PF of each byte value
	.data
x86c_parity:
	.byte	4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0
	.byte	0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4
	.byte	0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4
	.byte	4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0
	.byte	0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4
	.byte	4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0
	.byte	4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0
	.byte	0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4
