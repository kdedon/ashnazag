| i86runa.s -- the 8086 run loop for the 68k.
|
|	int i86runa(m, in)  struct i86 *m;  struct i86in *in;
|
| Same answers as i86run() in i86exec.c, which stays the reference.  An
| instruction of a frequent class whose memory and stack segments are
| unbiased runs here; anything else -- a biased segment, the trap flag,
| a delay loop, the rarer classes -- goes to i86step(), one instruction
| of the reference.  i86nrun counts the X_OK steps down, as there.
|
| Guest offsets index host segments biased, off ^ 0x8000 off a base
| + 0x8000, so that a word index wraps with the offset.
|
| Decodes are kept in a direct-mapped cache by IP, each with the code
| segment's base and the instruction's bytes, which are compared on
| every hit: code that rewrites itself decodes again.
|
| Addressing, the ALU, flags and conditions of a lazy record, and string
| moves and fills are those the Win16 core's loop uses (x86com).
|
|	a5	m		a4	the decode (a cache entry)
|	a3	in		d6	steps left
|	d5	effective address	d3 a, d4 b

| struct i86
M_SR	=	16
M_SB	=	24
M_SO	=	40
M_IP	=	48
M_FL	=	50
M_LZ	=	52
M_LW	=	53
M_LC	=	54
M_LA	=	56
M_LB	=	58
M_LR	=	60
M_FAULT	=	63
M_CX	=	2
M_SP	=	8
M_SI	=	12
M_DI	=	14
RBX	=	6
RBP	=	10
RSI	=	12
RDI	=	14

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

IN_MEM	=	2
IN_IMM	=	0x40
IN_DIR	=	0x80
IN_REPS	=	0x18
F_CF	=	1
S_CS	=	1
S_SS	=	2
X_OK	=	0

	.text
	.globl	i86runa

| the cache: NCACHE entries of the base [4], IP [2], length [1], a
| byte spare, the instruction's bytes [8] and its decode [16]
NCACHE	=	1024
C_IP	=	4
C_LEN	=	6
C_TEXT	=	8
C_IN	=	16

	.lcomm	Lcache,32*NCACHE

i86runa:
	moveml	%d2-%d7/%a2-%a5,%sp@-
	movel	%sp@(44),%a5
	movel	%sp@(48),%a3
	movew	i86nrun,%d6
	clrb	%a5@(M_FAULT)

Ltop:
	tstw	%a5@(M_SO+2*S_CS)
	bne	Lfall
	btst	&0,%a5@(M_FL)		| TF
	bne	Lfall
	moveq	&0,%d2
	movew	%a5@(M_IP),%d2		| ip
	movew	%d2,%d0
	andw	&NCACHE-1,%d0
	lslw	&5,%d0
	lea	Lcache,%a4
	addw	%d0,%a4			| the entry
	movel	%a5@(M_SB+4*S_CS),%a0	| cs
	cmpl	%a4@,%a0
	bne	Lmiss
	cmpw	%a4@(C_IP),%d2
	bne	Lmiss
	addl	%d2,%a0			| the bytes, unchanged?
	lea	%a4@(C_TEXT),%a1
	moveq	&0,%d1
	moveb	%a4@(C_LEN),%d1
	subqw	&1,%d1
1:	cmpmb	%a0@+,%a1@+
	dbne	%d1,1b
	bne	Lmiss
	lea	%a4@(C_IN),%a4
	bra	Ldecoded
Lmiss:
	pea	%a4@(C_IN)
	movel	%d2,%sp@-
	movel	%a5@(M_SB+4*S_CS),%sp@-
	jsr	i86dec
	lea	%sp@(12),%sp
	movel	%a5@(M_SB+4*S_CS),%a0
	clrl	%a4@			| no entry, unless it fits
	moveq	&0,%d1
	moveb	%a4@(C_IN+I_LEN),%d1
	beq	2f
	cmpib	&8,%d1
	bhi	2f
	movel	%d2,%d0
	addl	%d1,%d0
	cmpl	&0x10000,%d0		| not across the end of the segment
	bhi	2f
	movew	%d2,%a4@(C_IP)
	moveb	%d1,%a4@(C_LEN)
	addl	%d2,%a0
	lea	%a4@(C_TEXT),%a1
	subqw	&1,%d1
1:	moveb	%a0@+,%a1@+
	dbra	%d1,1b
	movel	%a5@(M_SB+4*S_CS),%a4@
2:	lea	%a4@(C_IN),%a4
Ldecoded:
	moveq	&0,%d0
	moveb	%a4@(I_OP),%d0
	cmpib	&47,%d0
	bhi	Lfall
	btst	&1,%a4@(I_FL)	| a memory operand: its segment unbiased
	beq	1f
	moveq	&3,%d1
	andb	%a4@(I_SEG),%d1
	addw	%d1,%d1
	tstw	%a5@(M_SO,%d1:w)
	bne	Lfall
1:	addw	%d0,%d0
	lea	Ltab,%a0
	movew	%a0@(0,%d0:w),%d0
	jmp	%a0@(0,%d0:w)

| the instruction is ours: count it, step IP past it, form its address
Lgo:
	addql	&1,i86ninsn
	moveq	&0,%d0
	moveb	%a4@(I_LEN),%d0
	addw	%d0,%a5@(M_IP)
	moveq	&0,%d5
	btst	&1,%a4@(I_FL)
	beq	9f
	movew	%a4@(I_DISP),%d5
	moveb	%a4@(I_RM),%d0
	tstb	%a4@(I_MOD)
	bne	1f
	cmpib	&6,%d0
	beq	9f
1:	addw	%d0,%d0
	lea	Lea16,%a0
	movew	%a0@(0,%d0:w),%d0
	jmp	%a0@(0,%d0:w)
9:	rts

	.include "x86com/ea16.s"

Lnext:
	subqw	&1,%d6
	bne	Ltop
	moveq	&X_OK,%d0
Lout:
	movew	%d6,i86nrun
	moveml	%sp@+,%d2-%d7/%a2-%a5
	rts

| one instruction of the reference
Lfall:
	movew	%d6,i86nrun
	movel	%a3,%sp@-
	movel	%a5,%sp@-
	jsr	i86step
	addql	&8,%sp
	tstl	%d0
	bne	Lout
	subqw	&1,%d6
	bne	Ltop
	bra	Lout

| ---- operands

| a0 = the host address of seg:d5, biased; d1 is lost
Lmem:
	moveq	&3,%d1
	andb	%a4@(I_SEG),%d1
	lslw	&2,%d1
	movel	%a5@(M_SB,%d1:w),%a0
	addl	&0x8000,%a0
	rts

| d0 = the mod r/m operand
Lrmrd:
	cmpib	&3,%a4@(I_MOD)
	bne	2f
	moveq	&0,%d1
	moveb	%a4@(I_RM),%d1
1:	tstb	%a4@(I_W)		| a register, d1
	beq	Lbreg
	addw	%d1,%d1
	moveq	&0,%d0
	movew	%a5@(0,%d1:w),%d0
	rts
Lbreg:	lea	Lboff,%a0
	moveb	%a0@(0,%d1:w),%d1
	moveq	&0,%d0
	moveb	%a5@(0,%d1:w),%d0
	rts
2:	bsr	Lmem
	movew	%d5,%d1
	eorw	&0x8000,%d1
	moveq	&0,%d0
	moveb	%a0@(0,%d1:w),%d0
	tstb	%a4@(I_W)
	beq	3f
	addqw	&1,%d1
	moveb	%a0@(0,%d1:w),%d1
	lslw	&8,%d1
	orw	%d1,%d0
3:	rts

| d0 = the reg-field operand
Lrgrd:
	moveq	&0,%d1
	moveb	%a4@(I_REG),%d1
	bra	1b

| the mod r/m operand = d0; d1, d2 are lost
Lrmwr:
	cmpib	&3,%a4@(I_MOD)
	bne	2f
	moveq	&0,%d1
	moveb	%a4@(I_RM),%d1
1:	tstb	%a4@(I_W)
	beq	Lbregw
	addw	%d1,%d1
	movew	%d0,%a5@(0,%d1:w)
	rts
Lbregw:	lea	Lboff,%a0
	moveb	%a0@(0,%d1:w),%d1
	moveb	%d0,%a5@(0,%d1:w)
	rts
2:	bsr	Lmem
	movew	%d5,%d1
	eorw	&0x8000,%d1
	moveb	%d0,%a0@(0,%d1:w)
	tstb	%a4@(I_W)
	beq	3f
	addqw	&1,%d1
	movew	%d0,%d2
	lsrw	&8,%d2
	moveb	%d2,%a0@(0,%d1:w)
3:	rts

| the reg-field operand = d0
Lrgwr:
	moveq	&0,%d1
	moveb	%a4@(I_REG),%d1
	bra	1b

| push d0; d1, d2 are lost
Lpush:
	movel	%a5@(M_SB+4*S_SS),%a0
	addl	&0x8000,%a0
	subqw	&2,%a5@(M_SP)
	movew	%a5@(M_SP),%d1
	eorw	&0x8000,%d1
	moveb	%d0,%a0@(0,%d1:w)
	addqw	&1,%d1
	movew	%d0,%d2
	lsrw	&8,%d2
	moveb	%d2,%a0@(0,%d1:w)
	rts

| d0 = popped; d1 is lost
Lpop:
	movel	%a5@(M_SB+4*S_SS),%a0
	addl	&0x8000,%a0
	movew	%a5@(M_SP),%d1
	eorw	&0x8000,%d1
	moveq	&0,%d0
	moveb	%a0@(0,%d1:w),%d0
	addqw	&1,%d1
	moveb	%a0@(0,%d1:w),%d1
	lslw	&8,%d1
	orw	%d1,%d0
	addqw	&2,%a5@(M_SP)
	rts

| materialise a pending record (i86flags); keeps d2-d7
Lfold:
	tstb	%a5@(M_LZ)
	beq	1f
	moveml	%d2-%d4,%sp@-
	bsr	Lrecld
	jsr	x86c_flags
	movew	%d0,%a5@(M_FL)
	clrb	%a5@(M_LZ)
	addql	&1,i86nflag
	moveml	%sp@+,%d2-%d4
1:	rts

| the record in x86com's registers
Lrecld:
	moveq	&0,%d1
	moveb	%a5@(M_LZ),%d1
	moveq	&0,%d2
	moveb	%a5@(M_LW),%d2
	moveq	&0,%d3
	movew	%a5@(M_LA),%d3
	moveq	&0,%d4
	movew	%a5@(M_LB),%d4
	moveq	&0,%d0
	movew	%a5@(M_LR),%d0
	movew	%a5@(M_FL),%a1
	rts

| d0 nonzero when condition d0 holds (i86lcond); keeps d2-d7
Lcond:
	moveml	%d2-%d4,%sp@-
	movel	%d0,%sp@-
	bsr	Lrecld
	movel	%sp@+,%a0
	jsr	x86c_cond
	moveml	%sp@+,%d2-%d4
	rts

| a record: class d1, a d3, b d4, result d0, carry in d2
Lrec:
	moveb	%d1,%a5@(M_LZ)
	moveb	%a4@(I_W),%a5@(M_LW)
	moveb	%d2,%a5@(M_LC)
	movew	%d3,%a5@(M_LA)
	movew	%d4,%a5@(M_LB)
	movew	%d0,%a5@(M_LR)
	rts

| an unbiased stack, or the reference
Lstack:
	tstw	%a5@(M_SO+2*S_SS)
	rts

| ---- the handlers; Ltop has checked the memory operand's segment

| ADD OR ADC SBB AND SUB XOR CMP
Lh_alu:
	bsr	Lgo
	moveb	%a4@(I_FL),%d0
	btst	&6,%d0			| IN_IMM: rm op imm
	beq	1f
	bsr	Lrmrd
	movel	%d0,%d3
	movew	%a4@(I_IMM),%d4
	bra	3f
1:	tstb	%d0			| IN_DIR: reg op rm
	bpl	2f
	bsr	Lrgrd
	movel	%d0,%d3
	bsr	Lrmrd
	movel	%d0,%d4
	bra	3f
2:	bsr	Lrmrd			| rm op reg
	movel	%d0,%d3
	bsr	Lrgrd
	movel	%d0,%d4
3:	moveq	&0,%d2			| carry in
	cmpib	&2,%a4@(I_X)		| ADC, SBB
	beq	4f
	cmpib	&3,%a4@(I_X)
	bne	5f
4:	bsr	Lfold
	moveq	&F_CF,%d2
	andb	%a5@(M_FL+1),%d2
5:	moveq	&0,%d0
	moveb	%a4@(I_X),%d0
	jsr	x86c_alu
	bsr	Lrec
	cmpib	&7,%a4@(I_X)		| CMP
	beq	Lnext
	tstb	%a4@(I_FL)
	bpl	1f
	bsr	Lrgwr
	bra	Lnext
1:	bsr	Lrmwr
	bra	Lnext

Lh_test:
	bsr	Lgo
	bsr	Lrmrd
	movel	%d0,%d3
	movew	%a4@(I_IMM),%d4
	btst	&6,%a4@(I_FL)
	bne	1f
	bsr	Lrgrd
	movel	%d0,%d4
1:	movew	%d3,%d0
	andw	%d4,%d0
	moveq	&3,%d1
	moveq	&0,%d2
	bsr	Lrec
	bra	Lnext

Lh_mov:
	bsr	Lgo
	moveb	%a4@(I_FL),%d0
	btst	&6,%d0
	beq	1f
	movew	%a4@(I_IMM),%d0		| rm = imm
	bsr	Lrmwr
	bra	Lnext
1:	tstb	%d0
	bpl	2f
	bsr	Lrmrd			| reg = rm
	bsr	Lrgwr
	bra	Lnext
2:	bsr	Lrgrd			| rm = reg
	bsr	Lrmwr
	bra	Lnext

Lh_jcc:
	bsr	Lgo
	moveq	&0,%d0
	moveb	%a4@(I_X),%d0
	bsr	Lcond
	tstl	%d0
	beq	Lnext
	movew	%a4@(I_DISP),%a5@(M_IP)
	bra	Lnext

Lh_jmp:
	bsr	Lgo
	movew	%a4@(I_DISP),%a5@(M_IP)
	bra	Lnext

| INC and DEC keep CF: fold a record that does not
Lh_inc:
	moveq	&4,%d7
	bra	1f
Lh_dec:
	moveq	&5,%d7
	cmpib	&3,%a4@(I_MOD)		| a register DEC that may close a delay loop
	bne	1f
	tstl	i86fast
	beq	1f
	bsr	Ldelay
	tstl	%d0
	bne	Lfall
1:	bsr	Lgo
	moveb	%a5@(M_LZ),%d0
	cmpib	&4,%d0
	beq	2f
	cmpib	&5,%d0
	beq	2f
	bsr	Lfold
2:	bsr	Lrmrd
	movel	%d0,%d3
	moveq	&1,%d4
	moveq	&1,%d0
	cmpib	&4,%d7
	beq	3f
	moveq	&-1,%d0
3:	addw	%d3,%d0
	movel	%d7,%d1
	moveq	&0,%d2
	bsr	Lrec
	bsr	Lrmwr
	bra	Lnext

| d0 nonzero when this DEC r ends nonzero before a JNZ back to it, the
| delay loop the reference finishes in one step
Ldelay:
	bsr	Lrmrd
	subqw	&1,%d0
	tstb	%a4@(I_W)
	bne	1f
	andw	&0xff,%d0
1:	tstw	%d0
	beq	9f
	movel	%a5@(M_SB+4*S_CS),%a0
	addl	&0x8000,%a0
	movew	%a5@(M_IP),%d1
	eorw	&0x8000,%d1
	moveb	%a0@(0,%d1:w),%d0	| the opcode
	moveq	&1,%d2
	cmpib	&0x48,%d0
	bcs	2f
	cmpib	&0x4f,%d0
	bls	3f
2:	andb	&0xfe,%d0		| FE/FF /1, register
	cmpib	&0xfe,%d0
	bne	9f
	addqw	&1,%d1
	moveb	%a0@(0,%d1:w),%d0
	andb	&0xf8,%d0
	cmpib	&0xc8,%d0
	bne	9f
	moveq	&2,%d2
	addqw	&1,%d1
	bra	4f
3:	addqw	&1,%d1
4:	cmpib	&0x75,%a0@(0,%d1:w)	| JNZ
	bne	9f
	addqw	&1,%d1
	moveq	&-2,%d0
	subb	%d2,%d0			| -(len + 2)
	cmpb	%a0@(0,%d1:w),%d0
	bne	9f
	moveq	&1,%d0
	rts
9:	moveq	&0,%d0
	rts

Lh_not:
	bsr	Lgo
	bsr	Lrmrd
	notw	%d0
	bsr	Lrmwr
	bra	Lnext

Lh_neg:
	bsr	Lgo
	bsr	Lrmrd
	movel	%d0,%d4
	moveq	&0,%d3
	moveq	&0,%d0
	subw	%d4,%d0
	moveq	&2,%d1
	moveq	&0,%d2
	bsr	Lrec
	bsr	Lrmwr
	bra	Lnext

Lh_xchg:
	bsr	Lgo
	bsr	Lrmrd
	movel	%d0,%d3
	bsr	Lrgrd
	bsr	Lrmwr
	movel	%d3,%d0
	bsr	Lrgwr
	bra	Lnext

Lh_lea:
	btst	&1,%a4@(I_FL)
	beq	Lfall
	bsr	Lgo
	moveq	&0,%d1
	moveb	%a4@(I_REG),%d1
	addw	%d1,%d1
	movew	%d5,%a5@(0,%d1:w)
	bra	Lnext

Lh_push:
	bsr	Lstack
	bne	Lfall
	btst	&1,%a4@(I_FL)	| PUSH SP pushes the new SP
	bne	1f
	cmpib	&4,%a4@(I_RM)
	beq	Lfall
1:	bsr	Lgo
	bsr	Lrmrd
	bsr	Lpush
	bra	Lnext

Lh_pop:
	bsr	Lstack
	bne	Lfall
	bsr	Lgo
	bsr	Lpop
	bsr	Lrmwr
	bra	Lnext

Lh_call:
	bsr	Lstack
	bne	Lfall
	bsr	Lgo
	movew	%a5@(M_IP),%d0
	bsr	Lpush
	movew	%a4@(I_DISP),%a5@(M_IP)
	bra	Lnext

Lh_ret:
	bsr	Lstack
	bne	Lfall
	bsr	Lgo
	bsr	Lpop
	movew	%d0,%a5@(M_IP)
	btst	&6,%a4@(I_FL)
	beq	Lnext
	movew	%a4@(I_IMM),%d0
	addw	%d0,%a5@(M_SP)
	bra	Lnext

| LOOPNE LOOPE LOOP JCXZ
Lh_loop:
	bsr	Lgo
	moveq	&0,%d0
	moveb	%a4@(I_X),%d0
	cmpib	&3,%d0
	bne	1f
	tstw	%a5@(M_CX)
	bne	Lnext
	bra	Ltake
1:	subqw	&1,%a5@(M_CX)
	beq	Lnext
	cmpib	&2,%d0
	beq	Ltake
	addqb	&4,%d0			| LOOPE: E (4); LOOPNE: NE (5)
	eorb	&1,%d0
	bsr	Lcond
	tstl	%d0
	beq	Lnext
Ltake:	movew	%a4@(I_DISP),%a5@(M_IP)
	bra	Lnext

Lh_flag:
	bsr	Lgo
	bsr	Lfold
	movew	%a4@(I_IMM),%d0
	moveb	%a4@(I_X),%d1
	bne	1f
	notw	%d0
	andw	%d0,%a5@(M_FL)
	bra	Lnext
1:	cmpib	&1,%d1
	bne	2f
	orw	%d0,%a5@(M_FL)
	bra	Lnext
2:	eorw	%d0,%a5@(M_FL)
	bra	Lnext

Lh_cbw:
	bsr	Lgo
	moveb	%a5@(1),%d0
	extw	%d0
	movew	%d0,%a5@(0)
	bra	Lnext

Lh_cwd:
	bsr	Lgo
	tstw	%a5@(0)
	smi	%d0
	extw	%d0
	movew	%d0,%a5@(4)
	bra	Lnext

Lh_nop:
	bsr	Lgo
	bra	Lnext

Lh_lahf:
	bsr	Lgo
	bsr	Lfold
	moveb	%a5@(M_FL+1),%a5@(0)	| AH
	bra	Lnext

Lh_sahf:
	bsr	Lgo
	bsr	Lfold
	moveb	%a5@(0),%d0
	andb	&0xd5,%d0
	orb	&2,%d0
	moveb	%d0,%a5@(M_FL+1)
	bra	Lnext

Lh_pushf:
	bsr	Lstack
	bne	Lfall
	bsr	Lgo
	bsr	Lfold
	movew	%a5@(M_FL),%d0
	orw	&0xf002,%d0
	bsr	Lpush
	bra	Lnext

Lh_popf:
	bsr	Lstack
	bne	Lfall
	bsr	Lgo
	bsr	Lpop
	orw	&0xf002,%d0
	movew	%d0,%a5@(M_FL)
	clrb	%a5@(M_LZ)
	bra	Lnext

| MOVS, STOS and LODS through unbiased segments, a word one from even
| offsets so that none straddles the wrap; the rest is the reference's
Lh_str:
	moveq	&0,%d0
	moveb	%a4@(I_X),%d0
	cmpib	&1,%d0			| CMPS
	beq	Lfall
	cmpib	&3,%d0
	bhi	Lfall			| SCAS
	bne	1f
	moveb	%a4@(I_FL),%d1		| REP LODS
	andb	&IN_REPS,%d1
	bne	Lfall
1:	moveq	&3,%d1
	andb	%a4@(I_SEG),%d1
	addw	%d1,%d1
	tstw	%a5@(M_SO,%d1:w)
	bne	Lfall
	tstw	%a5@(M_SO)		| ES
	bne	Lfall
	tstb	%a4@(I_W)
	beq	2f
	cmpib	&2,%d0			| STOS: DI only
	beq	3f
	btst	&0,%a5@(M_SI+1)
	bne	Lfall
	cmpib	&3,%d0			| LODS: SI only
	beq	2f
3:	btst	&0,%a5@(M_DI+1)
	bne	Lfall
2:	bsr	Lgo
	moveq	&1,%d3			| d3 the unit, d4 how many
	tstb	%a4@(I_W)
	beq	4f
	moveq	&2,%d3
4:	moveq	&1,%d4
	moveb	%a4@(I_FL),%d1
	andb	&IN_REPS,%d1
	beq	5f
	moveq	&0,%d4
	movew	%a5@(M_CX),%d4
	beq	Lnext
	clrw	%a5@(M_CX)
5:	tstb	%a4@(I_X)
	beq	Lsmovs
	cmpib	&2,%a4@(I_X)
	beq	Lsstos
	bsr	Lssrc			| LODS
	cmpib	&2,%d3
	beq	6f
	moveb	%a0@,%a5@(1)
	bra	7f
6:	moveb	%a0@+,%a5@(1)
	moveb	%a0@,%a5@(0)
7:	moveq	&1,%d0
	bsr	Lsstep
	addw	%d0,%a5@(M_SI)
	bra	Lnext

| a chunk at a time, up to where SI or DI wraps
Lsmovs:
	movew	%a5@(M_SI),%d0
	bsr	Lspan16
	movel	%d1,%d7
	movew	%a5@(M_DI),%d0
	bsr	Lspan16
	cmpl	%d1,%d7
	bls	1f
	movel	%d1,%d7
1:	cmpl	%d4,%d7
	bls	2f
	movel	%d4,%d7
2:	bsr	Lssrc
	bsr	Lsdst
	movel	%d7,%d0
	movel	%d3,%d1
	moveq	&0,%d2
	btst	&2,%a5@(M_FL)
	sne	%d2
	jsr	x86c_movs
	movel	%d7,%d0
	bsr	Lsstep
	addw	%d0,%a5@(M_SI)
	addw	%d0,%a5@(M_DI)
	subl	%d7,%d4
	bne	Lsmovs
	bra	Lnext

Lsstos:
	movew	%a5@(M_DI),%d0
	bsr	Lspan16
	movel	%d1,%d7
	cmpl	%d4,%d7
	bls	1f
	movel	%d4,%d7
1:	bsr	Lsdst
	movel	%d3,%d1
	movew	%a5@(0),%d3		| AL, or AX in memory order
	cmpib	&2,%d1
	bne	2f
	rorw	&8,%d3
2:	movel	%d7,%d0
	moveq	&0,%d2
	btst	&2,%a5@(M_FL)
	sne	%d2
	jsr	x86c_stos
	movel	%d1,%d3
	movel	%d7,%d0
	bsr	Lsstep
	addw	%d0,%a5@(M_DI)
	subl	%d7,%d4
	bne	Lsstos
	bra	Lnext

| a0 = the source at SI, a1 = ES:DI
Lssrc:	moveq	&3,%d1
	andb	%a4@(I_SEG),%d1
	lslw	&2,%d1
	movel	%a5@(M_SB,%d1:w),%a0
	moveq	&0,%d1
	movew	%a5@(M_SI),%d1
	addl	%d1,%a0
	rts
Lsdst:	movel	%a5@(M_SB),%a1
	moveq	&0,%d1
	movew	%a5@(M_DI),%d1
	addl	%d1,%a1
	rts

| d1 = units of d3 bytes from offset d0 before it wraps, as DF goes
Lspan16:
	moveq	&0,%d1
	movew	%d0,%d1
	btst	&2,%a5@(M_FL)
	bne	1f
	negl	%d1
	addl	&0x10000,%d1
	cmpib	&2,%d3
	bne	2f
	lsrl	&1,%d1
2:	rts
1:	cmpib	&2,%d3
	bne	3f
	lsrl	&1,%d1
3:	addql	&1,%d1
	rts

| d0 = d0 units of d3 bytes, negative going down
Lsstep:	cmpib	&2,%d3
	bne	1f
	addl	%d0,%d0
1:	btst	&2,%a5@(M_FL)
	beq	2f
	negl	%d0
2:	rts

| byte register n's offset in the word file: AL CL DL BL AH CH DH BH
Lboff:	.byte	1, 3, 5, 7, 0, 2, 4, 6

| class -> handler, in the I_* order of i86.h
Ltab:
	.word	Lfall-Ltab		| BAD
	.word	Lh_alu-Ltab
	.word	Lh_mov-Ltab
	.word	Lh_jcc-Ltab
	.word	Lh_jmp-Ltab
	.word	Lfall-Ltab		| SHIFT
	.word	Lh_not-Ltab
	.word	Lh_inc-Ltab
	.word	Lh_dec-Ltab
	.word	Lh_push-Ltab
	.word	Lh_pop-Ltab		| 10
	.word	Lh_call-Ltab
	.word	Lh_ret-Ltab
	.word	Lh_test-Ltab
	.word	Lh_neg-Ltab
	.word	Lh_xchg-Ltab
	.word	Lh_lea-Ltab
	.word	Lfall-Ltab		| MOVSR
	.word	Lfall-Ltab		| LXS
	.word	Lfall-Ltab		| MULDIV
	.word	Lh_str-Ltab		| STRING 20
	.word	Lh_loop-Ltab
	.word	Lh_flag-Ltab
	.word	Lfall-Ltab		| INT
	.word	Lfall-Ltab		| CALLI
	.word	Lfall-Ltab		| JMPI
	.word	Lfall-Ltab		| PUSHSR
	.word	Lfall-Ltab		| POPSR
	.word	Lh_pushf-Ltab
	.word	Lh_popf-Ltab
	.word	Lfall-Ltab, Lfall-Ltab, Lfall-Ltab, Lfall-Ltab, Lfall-Ltab, Lfall-Ltab	| 30-35
	.word	Lh_cbw-Ltab
	.word	Lh_cwd-Ltab
	.word	Lh_lahf-Ltab
	.word	Lh_sahf-Ltab
	.word	Lfall-Ltab, Lfall-Ltab, Lfall-Ltab, Lfall-Ltab, Lfall-Ltab, Lfall-Ltab	| 40-45
	.word	Lh_nop-Ltab
	.word	Lfall-Ltab		| HLT
