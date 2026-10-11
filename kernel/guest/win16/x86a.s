| x86a.s -- the 68k run loop of the Win16 x86 core.
|
|	void x86runa(c, n)  struct x86 *c;
|
| Runs instructions until c->stop, or n of them when n is not 0: the
| frequent ones here, the rest through x86_exec1(), one instruction of
| the reference in x86.c.  A handler checks all that could fault before
| it changes anything; when a check fails the instruction goes to the
| reference, which then faults as it would have.  CMPS and SCAS may
| leave their rest to the reference after doing what was in reach.
| Only 16-bit code on a 16-bit stack runs here.
|
| Each segment register has a shadow: its host base and how many bytes
| from offset 0 may be read and written (none where the reference
| checks more: expand-down, missing rights).  Shadows are made again
| after every instruction of the reference.
|
|	a6 shadows	a5 c	a4 the r/m operand	a3 CS's host base
|	a2 Ltab	a1 the next code byte	d7 IP of the instruction
|	d6 segment override (shadow offset) or -1
|	d5 effective address, then 0 for memory, 1 for a register

| struct x86: r[] (a word register's low half at 4 * n + 2), then
RAX	=	2
RCX	=	6
RDX	=	10
RBX	=	14
RSP	=	18
RBP	=	22
RSI	=	26
RDI	=	30
C_EIP	=	32
C_FL	=	36
C_S	=	40		| struct seg[6]: base, limit, sel, acc, big
C_PROT	=	112
C_LFOP	=	120		| op, sz, dst, src, res
C_LFRES	=	136
C_MEM	=	164
C_MEMSIZE =	168
C_LDT	=	172
C_STOP	=	196
C_ICOUNT =	208
CS_BASE	=	C_S+12
CS_LIM	=	C_S+16
CS_BIG	=	C_S+23
SS_BIG	=	C_S+35

| shadows: 16 bytes each, base, readable, writable
SH_ES	=	0
SH_SS	=	32
SH_DS	=	48

	.text
	.globl	x86runa

	.lcomm	Lsh,96
	.lcomm	Lsps,4		| the loop's stack pointer
	.lcomm	Lcsg,4		| IP must be below this to run here
	.lcomm	Lcslim,4	| CS's limit
	.lcomm	Lone,4		| instructions left, 0: no count

x86runa:
	moveml	%d2-%d7/%a2-%a6,%sp@-
	movel	%sp@(48),%a5
	movel	%sp@(52),Lone
	lea	Lsh,%a6
	lea	Ltab,%a2
	movel	%sp,Lsps
	tstl	Lone
	bne	1f
	tstl	%a5@(C_STOP)
	bne	Lexit
1:	bsr	Lload
	bra	Ltop

| the instruction is done: on to the next
Lnext:
	movel	%a1,%d7
	subl	%a3,%d7
Ljump:
	addql	&1,%a5@(C_ICOUNT)
	tstl	Lone
	bne	Lcount
Ltop:
	cmpl	Lcsg,%d7
	bcc	Lslow
	lea	%a3@(0,%d7:l),%a1
	moveq	&-1,%d6
	moveq	&0,%d0
	moveb	%a1@+,%d0
	movew	%a2@(0,%d0:w:2),%d1
	jmp	%a2@(0,%d1:w)

| one instruction of the reference
Lslow:
	movel	Lsps,%sp
	movel	%d7,%a5@(C_EIP)
	movel	Lone,%sp@-
	jsr	x86_exec1
	movel	%sp@+,Lone
	movel	%sp,Lsps
	tstl	%a5@(C_STOP)
	bne	Lexit
	tstl	Lone
	beq	1f
	subql	&1,Lone
	beq	Lexit
1:	bsr	Lload
	bra	Ltop
Lcount:
	subql	&1,Lone
	bne	Ltop
Lout:
	movel	%d7,%a5@(C_EIP)
Lexit:
	moveml	%sp@+,%d2-%d7/%a2-%a6
	rts

| ---- segments

| IP, every shadow and CS
Lload:
	movel	%a5@(C_EIP),%d7
	moveq	&5,%d3
1:	movel	%d3,%d1
	bsr	Lshadow
	dbra	%d3,1b
	bra	Lsetcs

| segment register d1's shadow; d0-d2, a0 are lost
Lshadow:
	movel	%d1,%d0
	mulu	&12,%d0
	lea	%a5@(C_S,%d0:w),%a0
	lslw	&4,%d1
	movel	%a5@(C_MEM),%d0
	addl	%a0@,%d0
	movel	%d0,%a6@(0,%d1:w)
	movel	%a5@(C_MEMSIZE),%d2	| bytes to the end of memory
	subl	%a0@,%d2
	bcc	1f
	moveq	&0,%d2
1:	movel	%a0@(4),%d0		| or to the limit
	addql	&1,%d0
	beq	2f
	cmpl	%d2,%d0
	bcc	2f
	movel	%d0,%d2
2:	movel	%d2,%a6@(4,%d1:w)
	movel	%d2,%a6@(8,%d1:w)
	moveb	%a0@(10),%d0		| acc
	btst	&3,%d0
	bne	3f
	btst	&2,%d0			| expand-down
	bne	5f
3:	tstl	%a5@(C_PROT)
	beq	6f
	tstb	%d0
	beq	5f
	andb	&0x0a,%d0		| CODE, R/W
	cmpib	&0x08,%d0
	bne	4f
	clrl	%a6@(4,%d1:w)		| code, not readable
4:	cmpib	&0x02,%d0
	beq	6f
	clrl	%a6@(8,%d1:w)		| not writable data
	rts
5:	clrl	%a6@(4,%d1:w)
	clrl	%a6@(8,%d1:w)
6:	rts

| CS's host base, limit and how far IP may go: 15 bytes short of the
| limit and of memory, and nowhere unless code and stack are 16-bit
Lsetcs:
	movel	%a5@(C_MEM),%a3
	addl	%a5@(CS_BASE),%a3
	movel	%a5@(CS_LIM),%d0
	movel	%d0,Lcslim
	addql	&1,%d0
	bne	1f
	subql	&1,%d0
1:	movel	%a5@(C_MEMSIZE),%d2
	subl	%a5@(CS_BASE),%d2
	bcc	2f
	moveq	&0,%d2
2:	cmpl	%d2,%d0
	bls	3f
	movel	%d2,%d0
3:	subl	&15,%d0
	bcc	4f
	moveq	&0,%d0
4:	tstb	%a5@(CS_BIG)
	bne	5f
	tstb	%a5@(SS_BIG)
	beq	6f
5:	moveq	&0,%d0
6:	movel	%d0,Lcsg
	rts

| segment register d1 (not CS) = selector d0, or the reference;
| d0-d2, a0 are lost
Lldseg:
	movel	%a1,%sp@-
	andl	&0xffff,%d0
	movel	%d1,%d2
	mulu	&12,%d2
	lea	%a5@(C_S,%d2:w),%a0
	tstl	%a5@(C_PROT)
	beq	5f
	movel	%d0,%d2
	andw	&0xfffc,%d2
	bne	1f
	cmpib	&2,%d1			| null: not SS or CS
	beq	Lslow
	cmpib	&1,%d1
	beq	Lslow
	movew	%d0,%a0@(8)
	clrl	%a0@
	clrl	%a0@(4)
	clrb	%a0@(10)
	bra	6f
1:	btst	&2,%d0			| the LDT
	beq	Lslow
	movel	%d0,%d2
	lsrw	&3,%d2
	mulu	&12,%d2
	movel	%a5@(C_LDT),%a1
	addl	%d2,%a1
	cmpw	%a0@(8),%d0		| the selector and descriptor it has
	bne	7f
	movel	%a1@,%d2
	cmpl	%a0@,%d2
	bne	7f
	movel	%a1@(4),%d2
	cmpl	%a0@(4),%d2
	bne	7f
	moveb	%a1@(8),%d2
	cmpb	%a0@(10),%d2
	bne	7f
	tstb	%a0@(11)
	bne	7f
	movel	%sp@+,%a1
	rts
7:	moveb	%a1@(8),%d2
	btst	&4,%d2			| code or data
	beq	Lslow
	cmpib	&1,%d1
	bne	2f
	btst	&3,%d2			| CS: code
	beq	Lslow
	bra	4f
2:	andb	&0x0a,%d2
	cmpib	&2,%d1
	bne	3f
	cmpib	&0x02,%d2		| SS: writable data
	bne	Lslow
	bra	4f
3:	cmpib	&0x08,%d2		| else not unreadable code
	beq	Lslow
4:	moveb	%a1@(8),%d2		| present
	bpl	Lslow
	movew	%d0,%a0@(8)
	movel	%a1@,%a0@
	movel	%a1@(4),%a0@(4)
	moveb	%d2,%a0@(10)
	clrb	%a0@(11)
	bra	6f
5:	movew	%d0,%a0@(8)		| real mode
	movel	%d0,%d2
	lsll	&4,%d2
	movel	%d2,%a0@
	cmpib	&1,%d1
	bne	6f
	moveb	&0x9b,%a0@(10)
6:	movel	%sp@+,%a1
	bra	Lshadow

| CS:IP = d0:d3 (16 bits each), or the reference; d0-d2, a0 are lost
Lldcs:
	andl	&0xffff,%d0
	tstl	%a5@(C_PROT)
	beq	1f
	movel	%d0,%d2
	andw	&0xfffc,%d2
	beq	Lslow
	btst	&2,%d0
	beq	Lslow
	movel	%d0,%d2
	lsrw	&3,%d2
	mulu	&12,%d2
	movel	%a5@(C_LDT),%a0
	cmpl	%a0@(4,%d2:l),%d3
	bhi	Lslow
	bra	2f
1:	cmpl	%a5@(CS_LIM),%d3
	bhi	Lslow
2:	moveq	&1,%d1
	bsr	Lldseg
	bsr	Lsetcs
	movel	%d3,%d7
	rts

| ---- operands

| the mod r/m byte at a1: d1 = its reg field; a register: d5 = rm, Z
| clear; memory: d5 = the offset, d2 = its shadow, Z set.  d0 (0 for
| memory) is what the Lloc routines take.  a0 is lost.
Lmodrm:
	moveq	&0,%d0
	moveb	%a1@+,%d0
	movel	%d0,%d1
	lsrb	&3,%d1
	andw	&7,%d1			| reg
	moveq	&7,%d2
	andb	%d0,%d2			| rm
	cmpib	&0xc0,%d0
	bcs	1f
	movel	%d2,%d5
	moveq	&1,%d0
	rts
1:	moveq	&0,%d5
	cmpib	&0x40,%d0
	bcs	2f
	cmpib	&0x80,%d0
	bcc	3f
	moveb	%a1@+,%d5		| mod 1
	extw	%d5
	bra	4f
3:	movew	%a1@+,%d5		| mod 2
	rorw	&8,%d5
	bra	4f
2:	cmpib	&6,%d2			| mod 0
	bne	4f
	movew	%a1@+,%d5		| a direct address
	rorw	&8,%d5
	moveq	&SH_DS,%d2
	bra	6f
4:	lea	Lrmsg,%a0
	moveb	%a0@(0,%d2:w),%d0
	addw	%d2,%d2
	lea	Lea16,%a0
	movew	%a0@(0,%d2:w),%d2
	jsr	%a0@(0,%d2:w)
	moveq	&0,%d2
	moveb	%d0,%d2
6:	andl	&0xffff,%d5
	tstl	%d6
	bmi	7f
	movel	%d6,%d2
7:	moveq	&0,%d0
	rts

	.include "x86com/ea16.s"

| each rm's segment: DS, or SS for those through BP
Lrmsg:	.byte	SH_DS, SH_DS, SH_SS, SH_SS, SH_DS, SH_DS, SH_SS, SH_DS
| byte register n's offset in r[]: AL CL DL BL AH CH DH BH
Lboff:	.byte	3, 7, 11, 15, 2, 6, 10, 14
	.even
Lmask:	.long	0xff, 0xffff, 0xffffffff

| a4 = where the word operand is, for reading, writing or both; d5 = 1
| for a register; or the reference.  d0 is lost.
Llocw_rw:
	tstb	%d0
	bne	Llocwr
	movel	%d5,%d0
	addql	&2,%d0
	cmpl	%a6@(4,%d2:w),%d0
	bhi	Lslow
	cmpl	%a6@(8,%d2:w),%d0
	bhi	Lslow
	bra	Llocm
Llocw_r:
	tstb	%d0
	bne	Llocwr
	movel	%d5,%d0
	addql	&2,%d0
	cmpl	%a6@(4,%d2:w),%d0
	bhi	Lslow
	bra	Llocm
Llocw_w:
	tstb	%d0
	bne	Llocwr
	movel	%d5,%d0
	addql	&2,%d0
	cmpl	%a6@(8,%d2:w),%d0
	bhi	Lslow
Llocm:	movel	%a6@(0,%d2:w),%a4
	addl	%d5,%a4
	moveq	&0,%d5
	rts
Llocwr:	lea	%a5@(2,%d5:w:4),%a4
	moveq	&1,%d5
	rts

| the same for a byte
Llocb_rw:
	tstb	%d0
	bne	Llocbr
	movel	%d5,%d0
	addql	&1,%d0
	cmpl	%a6@(4,%d2:w),%d0
	bhi	Lslow
	cmpl	%a6@(8,%d2:w),%d0
	bhi	Lslow
	bra	Llocm
Llocb_r:
	tstb	%d0
	bne	Llocbr
	movel	%d5,%d0
	addql	&1,%d0
	cmpl	%a6@(4,%d2:w),%d0
	bhi	Lslow
	bra	Llocm
Llocb_w:
	tstb	%d0
	bne	Llocbr
	movel	%d5,%d0
	addql	&1,%d0
	cmpl	%a6@(8,%d2:w),%d0
	bhi	Lslow
	bra	Llocm
Llocbr:	lea	Lboff,%a4
	moveq	&0,%d0
	moveb	%a4@(0,%d5:w),%d0
	lea	%a5@(0,%d0:w),%a4
	moveq	&1,%d5
	rts

| d3 = the operand at a4
Lgetw:	moveq	&0,%d3
	movew	%a4@,%d3
	tstb	%d5
	bne	1f
	rorw	&8,%d3
1:	rts
Lgetb:	moveq	&0,%d3
	moveb	%a4@,%d3
	rts

| the operand at a4 = d0, which is lost
Lputw:	tstb	%d5
	bne	1f
	rorw	&8,%d0
1:	movew	%d0,%a4@
	rts
Lputb:	moveb	%d0,%a4@
	rts

| d1 = byte register d1's offset in r[]
Lbreg:	lea	Lboff,%a0
	moveb	%a0@(0,%d1:w),%d1
	rts

| d4 = word register d1
Lgregw:	moveq	&0,%d4
	movew	%a5@(2,%d1:w:4),%d4
	rts

| d4 = byte register d1; d1 = its offset
Lgregb:	bsr	Lbreg
	moveq	&0,%d4
	moveb	%a5@(0,%d1:w),%d4
	rts

| d0 = the 16-bit offset at a1, then the source's shadow in d2
Lmoffs:	moveq	&0,%d5
	movew	%a1@+,%d5
	rorw	&8,%d5
	moveq	&SH_DS,%d2
	tstl	%d6
	bmi	1f
	movel	%d6,%d2
1:	moveq	&0,%d0
	rts

| ---- the stack

| push word d0; d0-d2, a0 are lost
Lpush:	moveq	&0,%d1
	movew	%a5@(RSP),%d1
	subqw	&2,%d1
	movel	%d1,%d2
	addql	&2,%d2
	cmpl	%a6@(SH_SS+8),%d2
	bhi	Lslow
	movel	%a6@(SH_SS),%a0
	rorw	&8,%d0
	movew	%d0,%a0@(0,%d1:l)
	movew	%d1,%a5@(RSP)
	rts

| d0 = the word at SS:SP, d2 = SP + 2; d1, a0 are lost
Lpeek:	moveq	&0,%d1
	movew	%a5@(RSP),%d1
	movel	%d1,%d2
	addql	&2,%d2
	cmpl	%a6@(SH_SS+4),%d2
	bhi	Lslow
	movel	%a6@(SH_SS),%a0
	moveq	&0,%d0
	movew	%a0@(0,%d1:l),%d0
	rorw	&8,%d0
	rts

| pop word d0; d1, d2, a0 are lost
Lpop:	bsr	Lpeek
	movew	%d2,%a5@(RSP)
	rts

| the stack has room for d1 bytes below SP, without wrapping
Lroom:	moveq	&0,%d2
	movew	%a5@(RSP),%d2
	cmpl	%d1,%d2
	bcs	Lslow
	cmpl	%a6@(SH_SS+8),%d2
	bhi	Lslow
	rts

| ---- flags

| d0 = condition d0 (Z clear when it holds)
Lcond:	moveml	%d1-%d4/%a0-%a1,%sp@-
	movel	%d0,%a0
	movel	%a5@(C_LFRES),%d0
	moveml	%a5@(C_LFOP),%d1-%d4
	movel	%a5@(C_FL),%a1
	jsr	x86c_cond
	moveml	%sp@+,%d1-%d4/%a0-%a1
	tstl	%d0
	rts

| the flags of a pending record made
Lfold:	tstl	%a5@(C_LFOP)
	beq	1f
	moveml	%d0-%d4/%a0-%a1,%sp@-
	movel	%a5@(C_LFRES),%d0
	moveml	%a5@(C_LFOP),%d1-%d4
	movel	%a5@(C_FL),%a1
	jsr	x86c_flags
	movel	%d0,%a5@(C_FL)
	clrl	%a5@(C_LFOP)
	moveml	%sp@+,%d0-%d4/%a0-%a1
1:	rts

| INC and DEC read CF from the flags word: a pending record of another
| kind leaves its CF there first.  d1 is lost.
Lcfold:	movel	%a5@(C_LFOP),%d1
	beq	3f
	cmpib	&4,%d1
	bcc	3f
	movel	%d0,%sp@-
	moveq	&2,%d0
	bsr	Lcond
	bne	1f
	bclr	&0,%a5@(C_FL+3)
	bra	2f
1:	bset	&0,%a5@(C_FL+3)
2:	movel	%sp@+,%d0
3:	rts

| a record: class d1, size d2, a d3, b d4, result d0
Lrec:	moveml	%d1-%d4,%a5@(C_LFOP)
	movel	%d0,%a5@(C_LFRES)
	rts

| ALU operation d0 of a d3 and b d4 (masked): d0 = the result, d1 = the
| operation; recorded.  d2, a0 are lost.
Lalub:	moveq	&0,%d2
	bra	Lalu
Laluw:	moveq	&1,%d2
Lalu:	movel	%d0,%sp@-
	movel	%d2,%sp@-
	moveq	&0,%d2
	cmpib	&2,%d0			| ADC, SBB: the carry in
	beq	1f
	cmpib	&3,%d0
	bne	2f
1:	moveq	&2,%d0
	bsr	Lcond
	movel	%d0,%d2
	movel	%sp@(4),%d0
2:	jsr	x86c_alu
	movel	%sp@+,%d2
	lea	Lmask,%a0
	andl	%a0@(0,%d2:w:4),%d0
	bsr	Lrec
	movel	%sp@+,%d1
	rts

| SF, ZF, PF of result d0 (size d1) into flags d2; d1, a0 are lost
Lszp:	tstb	%d1
	bne	1f
	tstb	%d0
	bne	2f
	orw	&0x40,%d2
2:	tstb	%d0
	bpl	3f
	orw	&0x80,%d2
	bra	3f
1:	tstw	%d0
	bne	2f
	orw	&0x40,%d2
2:	tstw	%d0
	bpl	3f
	orw	&0x80,%d2
3:	lea	x86c_parity,%a0
	moveq	&0,%d1
	moveb	%d0,%d1
	moveb	%a0@(0,%d1:w),%d1
	orw	%d1,%d2
	rts

| ---- the handlers: d0 the opcode, a1 past it

| ALU r/m, r: bits 5-3 the operation
Lalu_eb:
	lsrb	&3,%d0
	movew	%d0,%sp@-
	bsr	Lmodrm
	bsr	Lgregb
	cmpiw	&7,%sp@
	beq	1f
	bsr	Llocb_rw
	bra	2f
1:	bsr	Llocb_r
2:	bsr	Lgetb
	movew	%sp@+,%d0
	bsr	Lalub
	cmpib	&7,%d1
	beq	Lnext
	bsr	Lputb
	bra	Lnext

Lalu_ew:
	lsrb	&3,%d0
	movew	%d0,%sp@-
	bsr	Lmodrm
	bsr	Lgregw
	cmpiw	&7,%sp@
	beq	1f
	bsr	Llocw_rw
	bra	2f
1:	bsr	Llocw_r
2:	bsr	Lgetw
	movew	%sp@+,%d0
	bsr	Laluw
	cmpib	&7,%d1
	beq	Lnext
	bsr	Lputw
	bra	Lnext

| ALU r, r/m
Lalu_gb:
	lsrb	&3,%d0
	movew	%d0,%sp@-
	bsr	Lmodrm
	bsr	Llocb_r
	bsr	Lgetb
	movel	%d3,%d4
	bsr	Lbreg
	lea	%a5@(0,%d1:w),%a4
	moveq	&0,%d3
	moveb	%a4@,%d3
	movew	%sp@+,%d0
	bsr	Lalub
	cmpib	&7,%d1
	beq	Lnext
	moveb	%d0,%a4@
	bra	Lnext

Lalu_gw:
	lsrb	&3,%d0
	movew	%d0,%sp@-
	bsr	Lmodrm
	bsr	Llocw_r
	bsr	Lgetw
	movel	%d3,%d4
	lea	%a5@(2,%d1:w:4),%a4
	moveq	&0,%d3
	movew	%a4@,%d3
	movew	%sp@+,%d0
	bsr	Laluw
	cmpib	&7,%d1
	beq	Lnext
	movew	%d0,%a4@
	bra	Lnext

| ALU AL, imm8 and AX, imm16
Lalu_ib:
	lsrb	&3,%d0
	moveq	&0,%d3
	moveb	%a5@(RAX+1),%d3
	moveq	&0,%d4
	moveb	%a1@+,%d4
	bsr	Lalub
	cmpib	&7,%d1
	beq	Lnext
	moveb	%d0,%a5@(RAX+1)
	bra	Lnext

Lalu_iw:
	lsrb	&3,%d0
	moveq	&0,%d3
	movew	%a5@(RAX),%d3
	moveq	&0,%d4
	movew	%a1@+,%d4
	rorw	&8,%d4
	bsr	Laluw
	cmpib	&7,%d1
	beq	Lnext
	movew	%d0,%a5@(RAX)
	bra	Lnext

| group 1: ALU r/m, imm
Lgrp1b:
	bsr	Lmodrm
	movew	%d1,%sp@-
	cmpib	&7,%d1
	beq	1f
	bsr	Llocb_rw
	bra	2f
1:	bsr	Llocb_r
2:	moveq	&0,%d4
	moveb	%a1@+,%d4
	bsr	Lgetb
	movew	%sp@+,%d0
	bsr	Lalub
	cmpib	&7,%d1
	beq	Lnext
	bsr	Lputb
	bra	Lnext

Lgrp1w:
	moveq	&0,%d3			| 81: imm16
	bra	1f
Lgrp1s:
	moveq	&1,%d3			| 83: imm8, sign extended
1:	movew	%d3,%sp@-
	bsr	Lmodrm
	movew	%d1,%sp@-
	cmpib	&7,%d1
	beq	1f
	bsr	Llocw_rw
	bra	2f
1:	bsr	Llocw_r
2:	moveq	&0,%d4
	tstw	%sp@(2)
	bne	3f
	movew	%a1@+,%d4
	rorw	&8,%d4
	bra	4f
3:	moveb	%a1@+,%d4
	extw	%d4
4:	andl	&0xffff,%d4
	bsr	Lgetw
	movew	%sp@+,%d0
	addql	&2,%sp
	bsr	Laluw
	cmpib	&7,%d1
	beq	Lnext
	bsr	Lputw
	bra	Lnext

| TEST r/m, r and TEST AL/AX, imm
Ltestb:
	bsr	Lmodrm
	bsr	Lgregb
	bsr	Llocb_r
	bsr	Lgetb
	moveq	&4,%d0
	bsr	Lalub
	bra	Lnext

Ltestw:
	bsr	Lmodrm
	bsr	Lgregw
	bsr	Llocw_r
	bsr	Lgetw
	moveq	&4,%d0
	bsr	Laluw
	bra	Lnext

Ltestib:
	moveq	&0,%d3
	moveb	%a5@(RAX+1),%d3
	moveq	&0,%d4
	moveb	%a1@+,%d4
	moveq	&4,%d0
	bsr	Lalub
	bra	Lnext

Ltestiw:
	moveq	&0,%d3
	movew	%a5@(RAX),%d3
	moveq	&0,%d4
	movew	%a1@+,%d4
	rorw	&8,%d4
	moveq	&4,%d0
	bsr	Laluw
	bra	Lnext

| MOV
Lmov88:
	bsr	Lmodrm
	bsr	Lgregb
	bsr	Llocb_w
	moveb	%d4,%a4@
	bra	Lnext

Lmov89:
	bsr	Lmodrm
	bsr	Lgregw
	bsr	Llocw_w
	movel	%d4,%d0
	bsr	Lputw
	bra	Lnext

Lmov8a:
	bsr	Lmodrm
	bsr	Llocb_r
	bsr	Lbreg
	moveb	%a4@,%a5@(0,%d1:w)
	bra	Lnext

Lmov8b:
	bsr	Lmodrm
	bsr	Llocw_r
	bsr	Lgetw
	movew	%d3,%a5@(2,%d1:w:4)
	bra	Lnext

Lmovc6:
	bsr	Lmodrm
	tstb	%d1
	bne	Lslow
	bsr	Llocb_w
	moveb	%a1@+,%a4@
	bra	Lnext

Lmovc7:
	bsr	Lmodrm
	tstb	%d1
	bne	Lslow
	bsr	Llocw_w
	movew	%a1@+,%d0
	rorw	&8,%d0
	bsr	Lputw
	bra	Lnext

Lmovbi:
	andw	&7,%d0
	lea	Lboff,%a0
	moveb	%a0@(0,%d0:w),%d0
	moveb	%a1@+,%a5@(0,%d0:w)
	bra	Lnext

Lmovwi:
	andw	&7,%d0
	movew	%a1@+,%d1
	rorw	&8,%d1
	movew	%d1,%a5@(2,%d0:w:4)
	bra	Lnext

Lmova0:
	bsr	Lmoffs
	bsr	Llocb_r
	moveb	%a4@,%a5@(RAX+1)
	bra	Lnext

Lmova1:
	bsr	Lmoffs
	bsr	Llocw_r
	bsr	Lgetw
	movew	%d3,%a5@(RAX)
	bra	Lnext

Lmova2:
	bsr	Lmoffs
	bsr	Llocb_w
	moveb	%a5@(RAX+1),%a4@
	bra	Lnext

Lmova3:
	bsr	Lmoffs
	bsr	Llocw_w
	movew	%a5@(RAX),%d0
	bsr	Lputw
	bra	Lnext

| MOV r/m, sreg and MOV sreg, r/m
Lmov8c:
	bsr	Lmodrm
	cmpib	&5,%d1
	bhi	Lslow
	movel	%d1,%d3
	mulu	&12,%d3
	movew	%a5@(C_S+8,%d3:w),%d3
	bsr	Llocw_w
	movel	%d3,%d0
	bsr	Lputw
	bra	Lnext

Lmov8e:
	bsr	Lmodrm
	cmpib	&1,%d1
	beq	Lslow
	cmpib	&5,%d1
	bhi	Lslow
	bsr	Llocw_r
	bsr	Lgetw
	movel	%d3,%d0
	bsr	Lldseg
	bra	Lnext

Llea:
	bsr	Lmodrm
	bne	Lslow
	movew	%d5,%a5@(2,%d1:w:4)
	bra	Lnext

| LES, LDS
Lles:	moveq	&0,%d4
	bra	1f
Llds:	moveq	&3,%d4
1:	bsr	Lmodrm
	bne	Lslow
	cmpl	&0xfffc,%d5
	bhi	Lslow
	movel	%d5,%d0
	addql	&4,%d0
	cmpl	%a6@(4,%d2:w),%d0
	bhi	Lslow
	movel	%a6@(0,%d2:w),%a0
	addl	%d5,%a0
	moveq	&0,%d3
	movew	%a0@+,%d3
	rorw	&8,%d3
	moveq	&0,%d0
	movew	%a0@,%d0
	rorw	&8,%d0
	movew	%d1,%sp@-
	movel	%d4,%d1
	bsr	Lldseg
	movew	%sp@+,%d1
	movew	%d3,%a5@(2,%d1:w:4)
	bra	Lnext

| XCHG
Lxchgb:
	bsr	Lmodrm
	bsr	Llocb_rw
	bsr	Lgetb
	bsr	Lbreg
	moveb	%a5@(0,%d1:w),%d0
	bsr	Lputb
	moveb	%d3,%a5@(0,%d1:w)
	bra	Lnext

Lxchgw:
	bsr	Lmodrm
	bsr	Llocw_rw
	bsr	Lgetw
	movew	%a5@(2,%d1:w:4),%d0
	bsr	Lputw
	movew	%d3,%a5@(2,%d1:w:4)
	bra	Lnext

Lxchga:
	andw	&7,%d0
	movew	%a5@(2,%d0:w:4),%d1
	movew	%a5@(RAX),%a5@(2,%d0:w:4)
	movew	%d1,%a5@(RAX)
	bra	Lnext

Lnop:	bra	Lnext

Lcbw:	moveb	%a5@(RAX+1),%d0
	extw	%d0
	movew	%d0,%a5@(RAX)
	bra	Lnext

Lcwd:	tstw	%a5@(RAX)
	smi	%d0
	extw	%d0
	movew	%d0,%a5@(RDX)
	bra	Lnext

| INC, DEC
Linc:	andw	&7,%d0
	bsr	Lcfold
	moveq	&0,%d3
	movew	%a5@(2,%d0:w:4),%d3
	movel	%d3,%d4
	addqw	&1,%d4
	movew	%d4,%a5@(2,%d0:w:4)
	moveq	&4,%d1
	bra	1f
Ldec:	andw	&7,%d0
	bsr	Lcfold
	moveq	&0,%d3
	movew	%a5@(2,%d0:w:4),%d3
	movel	%d3,%d4
	subqw	&1,%d4
	movew	%d4,%a5@(2,%d0:w:4)
	moveq	&5,%d1
1:	movel	%d4,%d0
	moveq	&1,%d2
	moveq	&1,%d4
	bsr	Lrec
	bra	Lnext

| group 4: INC, DEC r/m8
Lgrp4:	bsr	Lmodrm
	cmpib	&1,%d1
	bhi	Lslow
	movew	%d1,%sp@-
	bsr	Llocb_rw
	bsr	Lcfold
	bsr	Lgetb
	movel	%d3,%d0
	moveq	&4,%d1
	tstw	%sp@+
	bne	1f
	addqb	&1,%d0
	bra	2f
1:	subqb	&1,%d0
	moveq	&5,%d1
2:	moveq	&0,%d2
	moveq	&1,%d4
	bsr	Lrec
	bsr	Lputb
	bra	Lnext

| group 5: INC, DEC, CALL, CALL FAR, JMP, JMP FAR, PUSH r/m16
Lgrp5:	bsr	Lmodrm
	cmpib	&1,%d1
	bls	Lincw
	cmpib	&2,%d1
	beq	Lcalli
	cmpib	&3,%d1
	beq	Lcallfi
	cmpib	&4,%d1
	beq	Ljmpi
	cmpib	&5,%d1
	beq	Ljmpfi
	cmpib	&6,%d1
	beq	Lpushm
	bra	Lslow

Lincw:	movew	%d1,%sp@-
	bsr	Llocw_rw
	bsr	Lcfold
	bsr	Lgetw
	movel	%d3,%d0
	moveq	&4,%d1
	tstw	%sp@+
	bne	1f
	addqw	&1,%d0
	bra	2f
1:	subqw	&1,%d0
	moveq	&5,%d1
2:	moveq	&1,%d2
	moveq	&1,%d4
	bsr	Lrec
	bsr	Lputw
	bra	Lnext

Lcalli:	bsr	Llocw_r
	bsr	Lgetw
	cmpl	Lcslim,%d3
	bhi	Lslow
	movel	%a1,%d0
	subl	%a3,%d0
	bsr	Lpush
	movel	%d3,%d7
	bra	Ljump

Ljmpi:	bsr	Llocw_r
	bsr	Lgetw
	cmpl	Lcslim,%d3
	bhi	Lslow
	movel	%d3,%d7
	bra	Ljump

Lpushm:	bsr	Llocw_r
	bsr	Lgetw
	movel	%d3,%d0
	bsr	Lpush
	bra	Lnext

| a far pointer in memory: d3 offset, d0 selector
Lfarm:	tstb	%d0
	bne	Lslow
	cmpl	&0xfffc,%d5
	bhi	Lslow
	movel	%d5,%d0
	addql	&4,%d0
	cmpl	%a6@(4,%d2:w),%d0
	bhi	Lslow
	movel	%a6@(0,%d2:w),%a0
	addl	%d5,%a0
	moveq	&0,%d3
	movew	%a0@+,%d3
	rorw	&8,%d3
	moveq	&0,%d0
	movew	%a0@,%d0
	rorw	&8,%d0
	rts

Ljmpfi:	bsr	Lfarm
	bsr	Lldcs
	bra	Ljump

Lcallfi:
	bsr	Lfarm
	bra	Lfarcall

| CALL FAR to d0:d3
Lfarcall:
	movel	%d0,%d4
	moveq	&4,%d1
	bsr	Lroom
	movel	%d4,%d0
	movel	%a1,%d4
	subl	%a3,%d4
	moveq	&0,%d5
	movew	%a5@(C_S+20),%d5	| CS
	bsr	Lldcs
	movel	%d5,%d0
	bsr	Lpush
	movel	%d4,%d0
	bsr	Lpush
	bra	Ljump

Lcallf:	moveq	&0,%d3
	movew	%a1@+,%d3
	rorw	&8,%d3
	moveq	&0,%d0
	movew	%a1@+,%d0
	rorw	&8,%d0
	bra	Lfarcall

Ljmpf:	moveq	&0,%d3
	movew	%a1@+,%d3
	rorw	&8,%d3
	moveq	&0,%d0
	movew	%a1@+,%d0
	rorw	&8,%d0
	bsr	Lldcs
	bra	Ljump

| RETF, RETF n
Lretf:	moveq	&0,%d4
	cmpib	&0xca,%d0
	bne	1f
	movew	%a1@+,%d4
	rorw	&8,%d4
1:	moveq	&0,%d1
	movew	%a5@(RSP),%d1
	cmpl	&0xfffc,%d1
	bhi	Lslow
	movel	%d1,%d5
	addql	&4,%d5
	cmpl	%a6@(SH_SS+4),%d5
	bhi	Lslow
	movel	%a6@(SH_SS),%a0
	moveq	&0,%d3
	movew	%a0@(0,%d1:l),%d3
	rorw	&8,%d3
	moveq	&0,%d0
	movew	%a0@(2,%d1:l),%d0
	rorw	&8,%d0
	addl	%d4,%d5
	bsr	Lldcs
	movew	%d5,%a5@(RSP)
	bra	Ljump

| CALL, JMP, Jcc, RET, LOOP
Lcall:	movew	%a1@+,%d3
	rorw	&8,%d3
	movel	%a1,%d0
	subl	%a3,%d0
	addw	%d0,%d3
	andl	&0xffff,%d3
	cmpl	Lcslim,%d3
	bhi	Lslow
	bsr	Lpush
	movel	%d3,%d7
	bra	Ljump

Ljcc:	andw	&15,%d0
	movel	%d0,%a0
	movel	%a1,%a4
	movel	%a5@(C_LFRES),%d0
	moveml	%a5@(C_LFOP),%d1-%d4
	movel	%a5@(C_FL),%a1
	jsr	x86c_cond
	movel	%a4,%a1
	tstl	%d0
	bne	Ljmp8
	addql	&1,%a1
	bra	Lnext

Ljmp8:	moveb	%a1@+,%d3
	extw	%d3
	bra	Lrel
Ljmp16:	movew	%a1@+,%d3
	rorw	&8,%d3
Lrel:	movel	%a1,%d0
	subl	%a3,%d0
	addw	%d0,%d3
	andl	&0xffff,%d3
	cmpl	Lcslim,%d3
	bhi	Lslow
	movel	%d3,%d7
	bra	Ljump

Lret:	movew	%d0,%d4
	bsr	Lpeek
	cmpl	Lcslim,%d0
	bhi	Lslow
	cmpib	&0xc2,%d4
	bne	1f
	movew	%a1@+,%d1
	rorw	&8,%d1
	addw	%d1,%d2
1:	movew	%d2,%a5@(RSP)
	movel	%d0,%d7
	bra	Ljump

Lloop:	moveb	%a1@+,%d3
	extw	%d3
	movel	%a1,%d2
	subl	%a3,%d2
	addw	%d2,%d3
	andl	&0xffff,%d3
	cmpl	Lcslim,%d3
	bhi	Lslow
	cmpib	&0xe3,%d0
	bne	1f
	tstw	%a5@(RCX)		| JCXZ
	bne	Lnext
	bra	3f
1:	subqw	&1,%a5@(RCX)
	beq	Lnext
	cmpib	&0xe2,%d0
	beq	3f
	cmpib	&0xe1,%d0
	bne	2f
	moveq	&4,%d0			| LOOPE
	bra	4f
2:	moveq	&5,%d0			| LOOPNE
4:	bsr	Lcond
	beq	Lnext
3:	movel	%d3,%d7
	bra	Ljump

| PUSH, POP
Lpushr:	andw	&7,%d0
	movew	%a5@(2,%d0:w:4),%d0
	bsr	Lpush
	bra	Lnext

Lpopr:	andw	&7,%d0
	movel	%d0,%d3
	bsr	Lpop
	movew	%d0,%a5@(2,%d3:w:4)
	bra	Lnext

Lpushs:	lsrb	&3,%d0
	mulu	&12,%d0
	movew	%a5@(C_S+8,%d0:w),%d0
	bsr	Lpush
	bra	Lnext

Lpops:	lsrb	&3,%d0
	movel	%d0,%d3
	bsr	Lpeek
	movew	%d2,%sp@-
	movel	%d3,%d1
	bsr	Lldseg
	movew	%sp@+,%a5@(RSP)
	bra	Lnext

Lpushi:	movew	%a1@+,%d0
	rorw	&8,%d0
	bsr	Lpush
	bra	Lnext

Lpushi8:
	moveb	%a1@+,%d0
	extw	%d0
	bsr	Lpush
	bra	Lnext

Lpushf:	bsr	Lfold
	movew	%a5@(C_FL+2),%d0
	bsr	Lpush
	bra	Lnext

| POPF: the low word; IOPL kept in protected mode
Lpopf:	bsr	Lpeek
	movew	%d2,%sp@-
	bsr	Lfold
	andw	&0x7fd7,%d0
	orw	&2,%d0
	tstl	%a5@(C_PROT)
	beq	1f
	andw	&0xcfff,%d0
	movew	%a5@(C_FL+2),%d1
	andw	&0x3000,%d1
	orw	%d1,%d0
1:	movew	%d0,%a5@(C_FL+2)
	movew	%sp@+,%a5@(RSP)
	bra	Lnext

| ENTER n, 0 and LEAVE
Lenter:	movew	%a1@+,%d3
	rorw	&8,%d3
	moveb	%a1@+,%d0
	andb	&31,%d0
	bne	Lslow
	movew	%a5@(RBP),%d0
	bsr	Lpush
	movew	%a5@(RSP),%d0
	movew	%d0,%a5@(RBP)
	subw	%d3,%d0
	movew	%d0,%a5@(RSP)
	bra	Lnext

Lleave:	moveq	&0,%d1
	movew	%a5@(RBP),%d1
	movel	%d1,%d2
	addql	&2,%d2
	cmpl	%a6@(SH_SS+4),%d2
	bhi	Lslow
	movel	%a6@(SH_SS),%a0
	movew	%a0@(0,%d1:l),%d0
	rorw	&8,%d0
	movew	%d2,%a5@(RSP)
	movew	%d0,%a5@(RBP)
	bra	Lnext

| CLC STC CMC CLI STI CLD STD
Lclc:	bsr	Lfold
	bclr	&0,%a5@(C_FL+3)
	bra	Lnext
Lstc:	bsr	Lfold
	bset	&0,%a5@(C_FL+3)
	bra	Lnext
Lcmc:	bsr	Lfold
	bchg	&0,%a5@(C_FL+3)
	bra	Lnext
Lcli:	bclr	&1,%a5@(C_FL+2)
	bra	Lnext
Lsti:	bset	&1,%a5@(C_FL+2)
	bra	Lnext
Lcld:	bclr	&2,%a5@(C_FL+2)
	bra	Lnext
Lstd:	bset	&2,%a5@(C_FL+2)
	bra	Lnext

| segment override prefixes ES CS SS DS, one per instruction
Lpfx:	tstl	%d6
	bpl	Lslow
	lsrb	&3,%d0
	andw	&3,%d0
	lslw	&4,%d0
	movel	%d0,%d6
	moveq	&0,%d0
	moveb	%a1@+,%d0
	movew	%a2@(0,%d0:w:2),%d1
	jmp	%a2@(0,%d1:w)

| shifts: SHL SHR SAL SAR r/m by 1, CL or imm8; flags made here
Lshb:	moveq	&0,%d4
	bra	1f
Lshw:	moveq	&1,%d4
1:	movew	%d0,%sp@-		| opcode
	movew	%d4,%sp@-		| size
	bsr	Lmodrm
	cmpib	&4,%d1
	bcs	Lslow			| rotates
	movew	%d1,%sp@-		| operation
	tstw	%sp@(2)
	bne	1f
	bsr	Llocb_rw
	bsr	Lgetb
	bra	2f
1:	bsr	Llocw_rw
	bsr	Lgetw
2:	movew	%sp@(4),%d0
	moveq	&1,%d4
	cmpib	&0xd0,%d0
	bcs	3f
	cmpib	&0xd2,%d0
	bcs	4f
	moveb	%a5@(RCX+1),%d4
	bra	4f
3:	moveb	%a1@+,%d4
4:	movew	%sp@+,%d1
	movew	%sp@+,%d2
	addql	&2,%sp
	andl	&31,%d4
	bne	5f
	movel	%d3,%d0			| no count: written back as it was
	bra	Lshput
5:	bsr	Lfold
	movel	%d3,%d0
	cmpib	&5,%d1
	beq	Lshr
	cmpib	&7,%d1
	beq	Lsar
	lsll	%d4,%d0			| SHL
	moveq	&8,%d1
	tstb	%d2
	beq	6f
	moveq	&16,%d1
6:	btst	%d1,%d0
	sne	%d6			| CF: the last bit out
	lea	Lmask,%a0
	andl	%a0@(0,%d2:w:4),%d0
	subql	&1,%d1
	btst	%d1,%d0
	sne	%d4
	eorb	%d6,%d4			| OF: the top bit changed
	bra	Lshf
Lshr:	subql	&1,%d4
	lsrl	%d4,%d0
	btst	&0,%d0
	sne	%d6
	lsrl	&1,%d0
	movel	%d0,%d4			| OF: the top two bits differ
	addl	%d4,%d4
	eorl	%d0,%d4
	moveq	&7,%d1
	tstb	%d2
	beq	6f
	moveq	&15,%d1
6:	btst	%d1,%d4
	sne	%d4
	bra	Lshf
Lsar:	tstb	%d2
	bne	6f
	extbl	%d0
	bra	7f
6:	extl	%d0
7:	subql	&1,%d4
	asrl	%d4,%d0
	btst	&0,%d0
	sne	%d6
	asrl	&1,%d0
	lea	Lmask,%a0
	andl	%a0@(0,%d2:w:4),%d0
	moveq	&0,%d4
| result d0, CF d6, OF d4, size d2
Lshf:	movel	%d2,%d3
	movel	%a5@(C_FL),%d2
	andw	&0xf72a,%d2
	tstb	%d6
	beq	1f
	orw	&1,%d2
1:	tstb	%d4
	beq	2f
	orw	&0x800,%d2
2:	movel	%d3,%d1
	bsr	Lszp
	movel	%d2,%a5@(C_FL)
	movel	%d3,%d2
Lshput:	tstb	%d2
	bne	1f
	bsr	Lputb
	bra	Lnext
1:	bsr	Lputw
	bra	Lnext

| group 3: TEST NOT NEG MUL IMUL DIV IDIV r/m
Lgrp3b:	bsr	Lmodrm
	cmpib	&1,%d1
	bhi	1f
	bsr	Llocb_r			| TEST
	bsr	Lgetb
	moveq	&0,%d4
	moveb	%a1@+,%d4
	moveq	&4,%d0
	bsr	Lalub
	bra	Lnext
1:	cmpib	&3,%d1
	bhi	3f
	movew	%d1,%sp@-
	bsr	Llocb_rw
	bsr	Lgetb
	cmpiw	&2,%sp@+
	bne	2f
	movel	%d3,%d0			| NOT
	notb	%d0
	bsr	Lputb
	bra	Lnext
2:	movel	%d3,%d4			| NEG: 0 - v
	moveq	&0,%d3
	moveq	&5,%d0
	bsr	Lalub
	bsr	Lputb
	bra	Lnext
3:	movew	%d1,%sp@-
	bsr	Llocb_r
	bsr	Lgetb
	movew	%sp@+,%d1
	cmpib	&5,%d1
	bhi	Ldivb
	moveq	&0,%d0
	moveb	%a5@(RAX+1),%d0
	cmpib	&4,%d1
	bne	4f
	muluw	%d3,%d0			| MUL
	movel	%d0,%d4
	lsrw	&8,%d4
	sne	%d4
	bra	5f
4:	extbl	%d0			| IMUL
	extbl	%d3
	mulsw	%d3,%d0
	moveb	%d0,%d4
	extw	%d4
	cmpw	%d0,%d4
	sne	%d4
5:	movew	%d0,%a5@(RAX)
	moveq	&0,%d1
	bra	Lmulf
Ldivb:	tstb	%d3
	beq	Lslow
	moveq	&0,%d0
	movew	%a5@(RAX),%d0
	cmpib	&6,%d1
	bne	1f
	divuw	%d3,%d0			| DIV
	cmpw	&0xff,%d0
	bhi	Lslow
	bra	2f
1:	extl	%d0			| IDIV
	extw	%d3
	divsw	%d3,%d0
	bvs	Lslow
	moveb	%d0,%d1
	extw	%d1
	cmpw	%d0,%d1
	bne	Lslow
2:	movel	%d0,%d1			| AH remainder, AL quotient
	swap	%d1
	lslw	&8,%d1
	moveb	%d0,%d1
	movew	%d1,%a5@(RAX)
	bra	Lnext

Lgrp3w:	bsr	Lmodrm
	cmpib	&1,%d1
	bhi	1f
	bsr	Llocw_r			| TEST
	bsr	Lgetw
	moveq	&0,%d4
	movew	%a1@+,%d4
	rorw	&8,%d4
	moveq	&4,%d0
	bsr	Laluw
	bra	Lnext
1:	cmpib	&3,%d1
	bhi	3f
	movew	%d1,%sp@-
	bsr	Llocw_rw
	bsr	Lgetw
	cmpiw	&2,%sp@+
	bne	2f
	movel	%d3,%d0			| NOT
	notw	%d0
	bsr	Lputw
	bra	Lnext
2:	movel	%d3,%d4			| NEG
	moveq	&0,%d3
	moveq	&5,%d0
	bsr	Laluw
	bsr	Lputw
	bra	Lnext
3:	movew	%d1,%sp@-
	bsr	Llocw_r
	bsr	Lgetw
	movew	%sp@+,%d1
	cmpib	&5,%d1
	bhi	Ldivw
	moveq	&0,%d0
	movew	%a5@(RAX),%d0
	cmpib	&4,%d1
	bne	4f
	muluw	%d3,%d0			| MUL
	movel	%d0,%d4
	clrw	%d4
	tstl	%d4
	sne	%d4
	bra	5f
4:	mulsw	%d3,%d0			| IMUL
	movew	%d0,%d4
	extl	%d4
	cmpl	%d0,%d4
	sne	%d4
5:	movew	%d0,%a5@(RAX)
	swap	%d0
	movew	%d0,%a5@(RDX)
	swap	%d0
	moveq	&1,%d1
| MUL's flags: CF and OF when the high half matters (d4), SF ZF PF of
| the low half d0 of size d1
Lmulf:	bsr	Lfold
	movel	%a5@(C_FL),%d2
	andw	&0xf73a,%d2
	tstb	%d4
	beq	1f
	orw	&0x801,%d2
1:	bsr	Lszp
	movel	%d2,%a5@(C_FL)
	bra	Lnext
Ldivw:	tstw	%d3
	beq	Lslow
	moveq	&0,%d0
	movew	%a5@(RDX),%d0
	swap	%d0
	movew	%a5@(RAX),%d0
	cmpib	&6,%d1
	bne	1f
	divuw	%d3,%d0			| DIV
	bvs	Lslow
	bra	2f
1:	divsw	%d3,%d0			| IDIV
	bvs	Lslow
2:	movew	%d0,%a5@(RAX)
	swap	%d0
	movew	%d0,%a5@(RDX)
	bra	Lnext

| ---- string instructions

| REP, REPNE: a string instruction follows
Lrep:	movel	%d0,%a4
	moveq	&0,%d0
	moveb	%a1@+,%d0
	cmpib	&0xa4,%d0
	bcs	Lslow
	cmpib	&0xaf,%d0
	bhi	Lslow
	cmpib	&0xa8,%d0
	beq	Lslow
	cmpib	&0xa9,%d0
	beq	Lslow
	bra	Lstr

| d0 the opcode, a4 its REP prefix or 0.  Then d2 the source's shadow,
| d3 the unit, d4 how many, d6 the prefix.
Lstr0:	subal	%a4,%a4
Lstr:	moveq	&SH_DS,%d2
	tstl	%d6
	bmi	1f
	movel	%d6,%d2
1:	movel	%a4,%d6
	moveq	&1,%d3
	btst	&0,%d0
	beq	2f
	moveq	&2,%d3
2:	moveq	&1,%d4
	tstl	%d6
	beq	3f
	moveq	&0,%d4
	movew	%a5@(RCX),%d4
	beq	Lnext
3:	cmpib	&0xa6,%d0
	bcs	Lmovs
	cmpib	&0xa8,%d0
	bcs	Lcmps
	cmpib	&0xac,%d0
	bcs	Lstos
	cmpib	&0xae,%d0
	bcs	Llods
	bra	Lscas

| d4 = how many of d4 units of d3 bytes from offset d5 the shadow at a0
| covers (limit d1 4: read, 8: write), going as DF says, before the
| offset would wrap.  d0, d1 are lost.
Lspan:	movel	%a0@(0,%d1:w),%d1
	cmpl	&0x10000,%d1
	bls	1f
	movel	&0x10000,%d1
1:	btst	&2,%a5@(C_FL+2)
	bne	3f
	subl	%d5,%d1
	bls	5f
	cmpib	&2,%d3
	bne	2f
	lsrl	&1,%d1
2:	cmpl	%d4,%d1
	bcc	4f
	movel	%d1,%d4
4:	rts
3:	movel	%d5,%d0
	addl	%d3,%d0
	cmpl	%d1,%d0
	bhi	5f
	movel	%d5,%d1
	cmpib	&2,%d3
	bne	6f
	lsrl	&1,%d1
6:	addql	&1,%d1
	bra	2b
5:	moveq	&0,%d4
	rts

| d0 = d4 units of d3 bytes, negative going down
Lstep:	movel	%d4,%d0
	cmpib	&2,%d3
	bne	1f
	addl	%d0,%d0
1:	btst	&2,%a5@(C_FL+2)
	beq	2f
	negl	%d0
2:	rts

| CX less d4 under REP
Lsdone:	tstl	%d6
	beq	Lnext
	subw	%d4,%a5@(RCX)
	bra	Lnext

| MOVS and STOS run here only when all of it is in reach: one that may
| have written over its own code is not restarted
Lmovs:	movel	%d4,%sp@-
	lea	%a6@(0,%d2:w),%a0
	moveq	&4,%d1
	moveq	&0,%d5
	movew	%a5@(RSI),%d5
	bsr	Lspan
	movel	%a0@,%a4
	addl	%d5,%a4
	lea	%a6@(SH_ES),%a0
	moveq	&8,%d1
	moveq	&0,%d5
	movew	%a5@(RDI),%d5
	bsr	Lspan
	cmpl	%sp@+,%d4
	bne	Lslow
	movel	%a1,%sp@-
	movel	%a6@(SH_ES),%a1
	addl	%d5,%a1
	movel	%a4,%a0
	movel	%d4,%d0
	movel	%d3,%d1
	moveq	&0,%d2
	btst	&2,%a5@(C_FL+2)
	sne	%d2
	jsr	x86c_movs
	movel	%sp@+,%a1
	bsr	Lstep
	addw	%d0,%a5@(RSI)
	addw	%d0,%a5@(RDI)
	bra	Lsdone

Lstos:	movel	%d4,%sp@-
	lea	%a6@(SH_ES),%a0
	moveq	&8,%d1
	moveq	&0,%d5
	movew	%a5@(RDI),%d5
	bsr	Lspan
	cmpl	%sp@+,%d4
	bne	Lslow
	movel	%a1,%sp@-
	movel	%a6@(SH_ES),%a1
	addl	%d5,%a1
	movel	%d3,%d1
	movew	%a5@(RAX),%d3
	cmpib	&2,%d1
	bne	1f
	rorw	&8,%d3
1:	movel	%d4,%d0
	moveq	&0,%d2
	btst	&2,%a5@(C_FL+2)
	sne	%d2
	jsr	x86c_stos
	movel	%d1,%d3
	movel	%sp@+,%a1
	bsr	Lstep
	addw	%d0,%a5@(RDI)
	bra	Lsdone

Llods:	tstl	%d6
	bne	Lslow
	lea	%a6@(0,%d2:w),%a0
	moveq	&4,%d1
	moveq	&0,%d5
	movew	%a5@(RSI),%d5
	bsr	Lspan
	tstl	%d4
	beq	Lslow
	movel	%a0@,%a0
	cmpib	&2,%d3
	beq	1f
	moveb	%a0@(0,%d5:l),%a5@(RAX+1)
	bra	2f
1:	movew	%a0@(0,%d5:l),%d0
	rorw	&8,%d0
	movew	%d0,%a5@(RAX)
2:	bsr	Lstep
	addw	%d0,%a5@(RSI)
	bra	Lnext

| CMPS and SCAS a unit at a time: [sp] the source's shadow, [sp+4] size
Lcmps:	subql	&1,%d3
	movel	%d3,%sp@-
	movel	%d2,%sp@-
1:	movel	%sp@(4),%d2
	moveq	&1,%d1
	addl	%d2,%d1
	movel	%sp@,%d0
	lea	%a6@(0,%d0:w),%a0
	moveq	&0,%d5
	movew	%a5@(RSI),%d5
	movel	%d5,%d0
	addl	%d1,%d0
	cmpl	%a0@(4),%d0
	bhi	Lslow
	movel	%a0@,%a0
	addl	%d5,%a0
	bsr	Lunit
	movel	%d0,%d3
	bsr	Ldunit
	bsr	Lscmp
	addw	%d1,%a5@(RSI)
	addw	%d1,%a5@(RDI)
	bsr	Lsrep
	bne	1b
	addql	&8,%sp
	bra	Lnext

Lscas:	subql	&1,%d3
	movel	%d3,%sp@-
	movel	%d2,%sp@-
1:	movel	%sp@(4),%d2
	moveq	&1,%d1
	addl	%d2,%d1
	moveq	&0,%d3
	movew	%a5@(RAX),%d3
	tstl	%d2
	bne	2f
	andw	&0xff,%d3
2:	bsr	Ldunit
	bsr	Lscmp
	addw	%d1,%a5@(RDI)
	bsr	Lsrep
	bne	1b
	addql	&8,%sp
	bra	Lnext

| d0 = the unit of size d2 at a0
Lunit:	moveq	&0,%d0
	tstl	%d2
	bne	1f
	moveb	%a0@,%d0
	rts
1:	movew	%a0@,%d0
	rorw	&8,%d0
	rts

| d4 = the unit at ES:DI (d1 bytes), or the reference
Ldunit:	moveq	&0,%d5
	movew	%a5@(RDI),%d5
	movel	%d5,%d0
	addl	%d1,%d0
	cmpl	%a6@(SH_ES+4),%d0
	bhi	Lslow
	movel	%a6@(SH_ES),%a0
	addl	%d5,%a0
	bsr	Lunit
	movel	%d0,%d4
	rts

| record d3 - d4 at size d2; d1 = the step (d1 bytes, signed by DF)
Lscmp:	movel	%d1,%sp@-
	movel	%d3,%d0
	subl	%d4,%d0
	lea	Lmask,%a0
	andl	%a0@(0,%d2:w:4),%d0
	moveq	&2,%d1
	bsr	Lrec
	movel	%sp@+,%d1
	btst	&2,%a5@(C_FL+2)
	beq	1f
	negl	%d1
1:	rts

| under REP, CX less one; Z set when the instruction is done
Lsrep:	tstl	%d6
	beq	2f
	subqw	&1,%a5@(RCX)
	cmpl	&0xf3,%d6
	bne	1f
	tstl	%d0			| REPE: stop when unequal
	bne	2f
	bra	3f
1:	tstl	%d0			| REPNE: stop when equal
	beq	2f
3:	tstw	%a5@(RCX)
	rts
2:	moveq	&0,%d0
	rts

| opcode -> handler
Ltab:
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpushs-Ltab, Lpops-Ltab
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpushs-Ltab, Lslow-Ltab
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpushs-Ltab, Lpops-Ltab
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpushs-Ltab, Lpops-Ltab
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpfx-Ltab, Lslow-Ltab
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpfx-Ltab, Lslow-Ltab
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpfx-Ltab, Lslow-Ltab
	.word	Lalu_eb-Ltab, Lalu_ew-Ltab, Lalu_gb-Ltab, Lalu_gw-Ltab, Lalu_ib-Ltab, Lalu_iw-Ltab, Lpfx-Ltab, Lslow-Ltab
	.word	Linc-Ltab, Linc-Ltab, Linc-Ltab, Linc-Ltab, Linc-Ltab, Linc-Ltab, Linc-Ltab, Linc-Ltab
	.word	Ldec-Ltab, Ldec-Ltab, Ldec-Ltab, Ldec-Ltab, Ldec-Ltab, Ldec-Ltab, Ldec-Ltab, Ldec-Ltab
	.word	Lpushr-Ltab, Lpushr-Ltab, Lpushr-Ltab, Lpushr-Ltab, Lpushr-Ltab, Lpushr-Ltab, Lpushr-Ltab, Lpushr-Ltab
	.word	Lpopr-Ltab, Lpopr-Ltab, Lpopr-Ltab, Lpopr-Ltab, Lpopr-Ltab, Lpopr-Ltab, Lpopr-Ltab, Lpopr-Ltab
	.word	Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lpushi-Ltab, Lslow-Ltab, Lpushi8-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab
	.word	Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab, Ljcc-Ltab
	.word	Lgrp1b-Ltab, Lgrp1w-Ltab, Lgrp1b-Ltab, Lgrp1s-Ltab, Ltestb-Ltab, Ltestw-Ltab, Lxchgb-Ltab, Lxchgw-Ltab
	.word	Lmov88-Ltab, Lmov89-Ltab, Lmov8a-Ltab, Lmov8b-Ltab, Lmov8c-Ltab, Llea-Ltab, Lmov8e-Ltab, Lslow-Ltab
	.word	Lnop-Ltab, Lxchga-Ltab, Lxchga-Ltab, Lxchga-Ltab, Lxchga-Ltab, Lxchga-Ltab, Lxchga-Ltab, Lxchga-Ltab
	.word	Lcbw-Ltab, Lcwd-Ltab, Lcallf-Ltab, Lnop-Ltab, Lpushf-Ltab, Lpopf-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lmova0-Ltab, Lmova1-Ltab, Lmova2-Ltab, Lmova3-Ltab, Lstr0-Ltab, Lstr0-Ltab, Lstr0-Ltab, Lstr0-Ltab
	.word	Ltestib-Ltab, Ltestiw-Ltab, Lstr0-Ltab, Lstr0-Ltab, Lstr0-Ltab, Lstr0-Ltab, Lstr0-Ltab, Lstr0-Ltab
	.word	Lmovbi-Ltab, Lmovbi-Ltab, Lmovbi-Ltab, Lmovbi-Ltab, Lmovbi-Ltab, Lmovbi-Ltab, Lmovbi-Ltab, Lmovbi-Ltab
	.word	Lmovwi-Ltab, Lmovwi-Ltab, Lmovwi-Ltab, Lmovwi-Ltab, Lmovwi-Ltab, Lmovwi-Ltab, Lmovwi-Ltab, Lmovwi-Ltab
	.word	Lshb-Ltab, Lshw-Ltab, Lret-Ltab, Lret-Ltab, Lles-Ltab, Llds-Ltab, Lmovc6-Ltab, Lmovc7-Ltab
	.word	Lenter-Ltab, Lleave-Ltab, Lretf-Ltab, Lretf-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lshb-Ltab, Lshw-Ltab, Lshb-Ltab, Lshw-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lloop-Ltab, Lloop-Ltab, Lloop-Ltab, Lloop-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lcall-Ltab, Ljmp16-Ltab, Ljmpf-Ltab, Ljmp8-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lslow-Ltab, Lslow-Ltab, Lrep-Ltab, Lrep-Ltab, Lslow-Ltab, Lcmc-Ltab, Lgrp3b-Ltab, Lgrp3w-Ltab
	.word	Lclc-Ltab, Lstc-Ltab, Lcli-Ltab, Lsti-Ltab, Lcld-Ltab, Lstd-Ltab, Lgrp4-Ltab, Lgrp5-Ltab
