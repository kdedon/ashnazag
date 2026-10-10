| z80runa.s -- the 8080/Z80 run loop for the 68k.
|
|	int z80run(m, n, k)  struct z80 *m;  long n, *k;
|
| Same answers as z80run.c over z80step(), which stays the reference.
| The frequent classes run here; the rest, and every prefixed opcode,
| are decoded by z80dec() and run by z80exec() in C.  Flags stay lazy:
| a record the code here cannot read is handed to z80flags() and
| z80lcond().
|
| Guest addresses are kept biased, a ^ 0x8000, so that a word index
| off a4 = memory + 0x8000 reaches all 64 KB and wraps with the word.
|
|	a5	m		a4	guest memory + 0x8000
|	a3	handler table	a2	z80tab
|	d7	guest PC, biased	d6	instructions left
|	a1	the opcode's z80tab record on entry to a handler

| struct z80
M_A	=	8
M_F	=	9
M_PC	=	10
M_LZ	=	25
M_LC	=	26
M_LA	=	28
M_LB	=	30
M_LR	=	32
M_MEM	=	36
M_HL	=	4
M_SP	=	6

| z80tab record
T_FL	=	2
T_X	=	3
T_Y	=	4

| the frame: a6 links; in at -12; args at 8, 12, 16
F_IN	=	-12
A_M	=	8
A_N	=	12
A_K	=	16

X_OK	=	0
X_BAD	=	2
F_CY	=	0x01

	.text
	.globl	z80run

z80run:
	link	%a6,&-12
	moveml	%d2-%d7/%a2-%a5,%sp@-
	movel	%a6@(A_M),%a5
	movel	%a5@(M_MEM),%a4
	addl	&0x8000,%a4
	lea	Ltab,%a3
	lea	z80tab,%a2
	movew	%a5@(M_PC),%d7
	eorw	&0x8000,%d7
	movel	%a6@(A_N),%d6
	ble	Llimit

| one instruction: its record, then its class's handler
Ltop:
	moveq	&0,%d0
	moveb	%a4@(0,%d7:w),%d0
	lslw	&3,%d0
	lea	%a2@(0,%d0:w),%a1
	moveq	&0,%d1
	moveb	%a1@(1),%d1
	addw	%d1,%d1
	movew	%a3@(0,%d1:w),%d1
	jmp	%a3@(0,%d1:w)

Lnext:
	subql	&1,%d6
	bne	Ltop
Llimit:
	moveq	&X_OK,%d0
	moveq	&0,%d1
| d0 the answer, d1 1 when the instruction that stopped the loop decoded
Lout:
	movew	%d7,%d2
	eorw	&0x8000,%d2
	movew	%d2,%a5@(M_PC)
	movel	%a6@(A_N),%d2
	tstl	%d2
	bge	L0
	moveq	&0,%d2
L0:	subl	%d6,%d2
	bge	L1
	moveq	&0,%d2
L1:	movel	%a6@(A_K),%a0
	addl	%d2,%a0@
	addl	%d1,%d2
	addl	%d2,z80ninsn
	moveml	%sp@+,%d2-%d7/%a2-%a5
	unlk	%a6
	rts

| ---- helpers

| d0 = register d1 (0-7, 6 the byte at (HL)); keeps d1-d7
Lgetr:
	moveq	&0,%d0
	cmpib	&6,%d1
	beq	1f
	bhi	2f
	moveb	%a5@(0,%d1:w),%d0
	rts
1:	movew	%a5@(M_HL),%d0
	eorw	&0x8000,%d0
	moveb	%a4@(0,%d0:w),%d0
	andl	&0xff,%d0
	rts
2:	moveb	%a5@(M_A),%d0
	rts

| register d1 = d0; d1 is lost
Lsetr:
	cmpib	&6,%d1
	beq	1f
	bhi	2f
	moveb	%d0,%a5@(0,%d1:w)
	rts
1:	movew	%a5@(M_HL),%d1
	eorw	&0x8000,%d1
	moveb	%d0,%a4@(0,%d1:w)
	rts
2:	moveb	%d0,%a5@(M_A)
	rts

| materialise a pending record (z80flags); keeps d2-d7
Lfold:
	tstb	%a5@(M_LZ)
	beq	1f
	movel	%a5,%sp@-
	jsr	z80flags
	addql	&4,%sp
1:	rts

| d0 = the word at biased address d1, which becomes d1 + 2; d2 is lost
Lrw:
	moveq	&0,%d0
	moveb	%a4@(0,%d1:w),%d0
	addqw	&1,%d1
	moveb	%a4@(0,%d1:w),%d2
	addqw	&1,%d1
	lslw	&8,%d2
	orw	%d2,%d0
	rts

| the word d0 at biased address d1; d1 becomes d1 + 2; d2 is lost
Lww:
	moveb	%d0,%a4@(0,%d1:w)
	addqw	&1,%d1
	movew	%d0,%d2
	lsrw	&8,%d2
	moveb	%d2,%a4@(0,%d1:w)
	addqw	&1,%d1
	rts

| d0 = the word after the opcode; d7 moves past it; d2 is lost
Limm:
	addqw	&1,%d7
	moveq	&0,%d0
	moveb	%a4@(0,%d7:w),%d0
	addqw	&1,%d7
	moveb	%a4@(0,%d7:w),%d2
	addqw	&1,%d7
	lslw	&8,%d2
	orw	%d2,%d0
	rts

| push d0
Lpush:
	movew	%a5@(M_SP),%d1
	subqw	&2,%d1
	movew	%d1,%a5@(M_SP)
	eorw	&0x8000,%d1
	bra	Lww

| d0 = popped word
Lpop:
	movew	%a5@(M_SP),%d1
	eorw	&0x8000,%d1
	bsr	Lrw
	addqw	&2,%a5@(M_SP)
	rts

| d0 nonzero when condition d3 holds (z80lcond); keeps d2-d7
Lcond:
	moveq	&0,%d0
	moveb	%a5@(M_LZ),%d0
	beq	Lcf
	cmpib	&6,%d0
	bhi	Lcc
	movew	%d3,%d1
	lsrw	&1,%d1
	beq	Lcz
	subqw	&2,%d1
	bmi	Lccy
	beq	Lcpe
	movew	&0x80,%d1		| M
	andw	%a5@(M_LR),%d1
	bra	Lct
Lcz:	tstw	%a5@(M_LR)		| Z
	seq	%d1
	bra	Lct
Lcpe:	moveq	&0,%d1			| PE: parity of the result
	moveb	%a5@(M_LR+1),%d1
	lea	z80par,%a0
	moveb	%a0@(0,%d1:w),%d1
	bra	Lct
Lccy:	cmpib	&2,%d0			| CY
	bhi	Lccl
	movew	%a5@(M_LA),%d1
	cmpib	&1,%d0
	beq	Lcadd
	movew	%a5@(M_LB),%d0		| SUB: lc ? la <= lb : la < lb
	tstb	%a5@(M_LC)
	bne	1f
	cmpw	%d0,%d1
	scs	%d1
	bra	Lct
1:	cmpw	%d0,%d1
	sls	%d1
	bra	Lct
Lcadd:	movew	%a5@(M_LR),%d0		| ADD: lc ? lr <= la : lr < la
	tstb	%a5@(M_LC)
	bne	1f
	cmpw	%d1,%d0
	scs	%d1
	bra	Lct
1:	cmpw	%d1,%d0
	sls	%d1
	bra	Lct
Lccl:	cmpib	&4,%d0
	bhi	1f
	moveq	&0,%d1			| ANA, ORA, XRA clear it
	bra	Lct
1:	moveq	&F_CY,%d1		| INR, DCR keep it
	andb	%a5@(M_F),%d1
Lct:	tstb	%d1
	sne	%d1
	btst	&0,%d3
	bne	1f
	notb	%d1
1:	moveq	&1,%d0
	andb	%d1,%d0
	rts
Lcf:	moveb	%a5@(M_F),%d0		| complete flags
	movew	%d3,%d1
	lsrw	&1,%d1
	lea	Lcmask,%a0
	andb	%a0@(0,%d1:w),%d0
	moveb	%d0,%d1
	bra	Lct
Lcc:	movel	%d3,%sp@-
	movel	%a5,%sp@-
	jsr	z80lcond
	addql	&8,%sp
	rts
Lcmask:	.byte	0x40,0x01,0x04,0x80

| ---- the handlers, one per class; d7 at the opcode

Lh_nop:
	addqw	&1,%d7
	bra	Lnext

Lh_ldrr:
	moveq	&0,%d1
	moveb	%a1@(T_Y),%d1
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	bsr	Lgetr
	movel	%d3,%d1
	bsr	Lsetr
	addqw	&1,%d7
	bra	Lnext

Lh_ldri:
	moveq	&0,%d1
	moveb	%a1@(T_X),%d1
	addqw	&1,%d7
	moveb	%a4@(0,%d7:w),%d0
	addqw	&1,%d7
	bsr	Lsetr
	bra	Lnext

Lh_alu:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3		| the operation
	btst	&0,%a1@(T_FL)
	beq	1f
	addqw	&1,%d7
	moveq	&0,%d2
	moveb	%a4@(0,%d7:w),%d2	| b, an immediate
	bra	2f
1:	moveq	&0,%d1
	moveb	%a1@(T_Y),%d1
	bsr	Lgetr
	movel	%d0,%d2			| b, a register
2:	addqw	&1,%d7
	moveq	&0,%d4
	moveb	%a5@(M_A),%d4		| a
	moveq	&0,%d5			| carry in
	addw	%d3,%d3
	lea	Lalut,%a0
	movew	%a0@(0,%d3:w),%d0
	jmp	%a0@(0,%d0:w)
Lalut:	.word	Ladd-Lalut, Ladc-Lalut, Lsub-Lalut, Lsbb-Lalut
	.word	Lana-Lalut, Lxra-Lalut, Lora-Lalut, Lcmp-Lalut
Ladc:	bsr	Lfold
	moveq	&F_CY,%d5
	andb	%a5@(M_F),%d5
Ladd:	movel	%d4,%d0
	addl	%d2,%d0
	addl	%d5,%d0
	moveq	&1,%d1
	bra	Lrec
Lsbb:	bsr	Lfold
	moveq	&F_CY,%d5
	andb	%a5@(M_F),%d5
Lsub:	movel	%d4,%d0
	subl	%d2,%d0
	subl	%d5,%d0
	moveq	&2,%d1
	bra	Lrec
Lana:	movel	%d4,%d0
	andl	%d2,%d0
	moveq	&3,%d1
	bra	Lrec
Lxra:	movel	%d4,%d0
	eorl	%d2,%d0
	moveq	&4,%d1
	bra	Lrec
Lora:	movel	%d4,%d0
	orl	%d2,%d0
	moveq	&4,%d1
Lrec:	andw	&0xff,%d0
	moveb	%d1,%a5@(M_LZ)
	moveb	%d5,%a5@(M_LC)
	movew	%d4,%a5@(M_LA)
	movew	%d2,%a5@(M_LB)
	movew	%d0,%a5@(M_LR)
	moveb	%d0,%a5@(M_A)
	bra	Lnext
Lcmp:	movel	%d4,%d0
	subl	%d2,%d0
	andw	&0xff,%d0
	moveb	&2,%a5@(M_LZ)
	clrb	%a5@(M_LC)
	movew	%d4,%a5@(M_LA)
	movew	%d2,%a5@(M_LB)
	movew	%d0,%a5@(M_LR)
	bra	Lnext

| INR and DCR keep CY: fold a record that does not
Lh_inr:
	moveq	&5,%d4
	bra	1f
Lh_dcr:
	moveq	&6,%d4
1:	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	moveb	%a5@(M_LZ),%d0
	beq	2f
	cmpib	&5,%d0
	beq	2f
	cmpib	&6,%d0
	beq	2f
	movel	%a5,%sp@-
	jsr	z80flags
	addql	&4,%sp
2:	movel	%d3,%d1
	bsr	Lgetr
	movel	%d0,%d2			| v
	moveq	&1,%d0
	cmpib	&5,%d4
	beq	3f
	moveq	&-1,%d0
3:	addl	%d2,%d0
	andl	&0xff,%d0
	moveb	%d4,%a5@(M_LZ)
	clrb	%a5@(M_LC)
	movew	%d2,%a5@(M_LA)
	movew	&1,%a5@(M_LB)
	movew	%d0,%a5@(M_LR)
	movel	%d0,%d5			| r
	movel	%d3,%d1
	bsr	Lsetr
	movew	%d7,%d2			| pc0, biased
	addqw	&1,%d7
	cmpib	&6,%d4
	bne	Lnext
	tstb	%d5			| a DCR that may close a delay loop
	beq	Lnext
	cmpib	&6,%d3
	beq	Lnext
	tstl	z80fast
	beq	Lnext
	movew	%d7,%d0
	eorw	&0x8000,%d0
	movew	%d0,%a5@(M_PC)
	eorw	&0x8000,%d2
	movel	%d5,%sp@-
	movel	%d2,%sp@-
	movel	%d3,%sp@-
	movel	%a5,%sp@-
	jsr	z80delay
	lea	%sp@(16),%sp
	movew	%a5@(M_PC),%d7
	eorw	&0x8000,%d7
	bra	Lnext

Lh_lxi:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	bsr	Limm
	addw	%d3,%d3
	movew	%d0,%a5@(0,%d3:w)
	bra	Lnext

Lh_inx:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	addw	%d3,%d3
	addqw	&1,%a5@(0,%d3:w)
	addqw	&1,%d7
	bra	Lnext

Lh_dcx:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	addw	%d3,%d3
	subqw	&1,%a5@(0,%d3:w)
	addqw	&1,%d7
	bra	Lnext

Lh_dad:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	addw	%d3,%d3
	bsr	Lfold
	moveq	&0,%d0
	movew	%a5@(M_HL),%d0
	moveq	&0,%d1
	movew	%a5@(0,%d3:w),%d1
	addl	%d1,%d0
	movew	%d0,%a5@(M_HL)
	swap	%d0			| the carry out of bit 15
	moveb	%a5@(M_F),%d1
	andb	&0xd6,%d1		| ~(CY | bits 5 and 3)
	orb	&2,%d1
	orb	%d0,%d1
	moveb	%d1,%a5@(M_F)
	addqw	&1,%d7
	bra	Lnext

Lh_ldax:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	addw	%d3,%d3
	movew	%a5@(0,%d3:w),%d1
	eorw	&0x8000,%d1
	moveb	%a4@(0,%d1:w),%a5@(M_A)
	addqw	&1,%d7
	bra	Lnext

Lh_lda:
	bsr	Limm
	eorw	&0x8000,%d0
	moveb	%a4@(0,%d0:w),%a5@(M_A)
	bra	Lnext

Lh_sta:
	bsr	Limm
	eorw	&0x8000,%d0
	moveb	%a5@(M_A),%a4@(0,%d0:w)
	bra	Lnext

Lh_lhld:
	bsr	Limm
	movew	%d0,%d1
	eorw	&0x8000,%d1
	bsr	Lrw
	movew	%d0,%a5@(M_HL)
	bra	Lnext

Lh_shld:
	bsr	Limm
	movew	%d0,%d1
	eorw	&0x8000,%d1
	movew	%a5@(M_HL),%d0
	bsr	Lww
	bra	Lnext

Lh_jmp:
	bsr	Limm
	movew	%d0,%d7
	eorw	&0x8000,%d7
	bra	Lnext

Lh_jcc:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	bsr	Lcond
	tstb	%d0
	bne	Lh_jmp
	addqw	&3,%d7
	bra	Lnext

Lh_call:
	bsr	Limm
	movew	%d0,%d4
	movew	%d7,%d0
	eorw	&0x8000,%d0
	bsr	Lpush
	movew	%d4,%d7
	eorw	&0x8000,%d7
	bra	Lnext

Lh_ccc:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	bsr	Lcond
	tstb	%d0
	bne	Lh_call
	addqw	&3,%d7
	bra	Lnext

Lh_ret:
	bsr	Lpop
	movew	%d0,%d7
	eorw	&0x8000,%d7
	bra	Lnext

Lh_rcc:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	bsr	Lcond
	tstb	%d0
	bne	Lh_ret
	addqw	&1,%d7
	bra	Lnext

Lh_pchl:
	movew	%a5@(M_HL),%d7
	eorw	&0x8000,%d7
	bra	Lnext

Lh_xchg:
	movew	%a5@(M_HL),%d0
	movew	%a5@(2),%a5@(M_HL)
	movew	%d0,%a5@(2)
	addqw	&1,%d7
	bra	Lnext

Lh_cma:
	notb	%a5@(M_A)
	addqw	&1,%d7
	bra	Lnext

Lh_push:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	addqw	&1,%d7
	cmpib	&3,%d3
	beq	1f
	addw	%d3,%d3
	movew	%a5@(0,%d3:w),%d0
	bsr	Lpush
	bra	Lnext
1:	bsr	Lfold			| PSW: A over the flags
	moveb	%a5@(M_A),%d0
	lslw	&8,%d0
	moveb	%a5@(M_F),%d0
	bsr	Lpush
	bra	Lnext

Lh_pop:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	addqw	&1,%d7
	bsr	Lpop
	cmpib	&3,%d3
	beq	1f
	addw	%d3,%d3
	movew	%d0,%a5@(0,%d3:w)
	bra	Lnext
1:	movew	%d0,%d1			| PSW
	lsrw	&8,%d1
	moveb	%d1,%a5@(M_A)
	orb	&2,%d0
	andb	&0xd7,%d0
	moveb	%d0,%a5@(M_F)
	clrb	%a5@(M_LZ)
	bra	Lnext

| RLC RRC RAL RAR: CY alone changes
Lh_rot:
	moveq	&0,%d3
	moveb	%a1@(T_X),%d3
	bsr	Lfold
	moveq	&0,%d0
	moveb	%a5@(M_A),%d0
	moveq	&F_CY,%d2
	andb	%a5@(M_F),%d2		| old CY
	cmpib	&1,%d3
	bhi	2f
	beq	1f
	rolb	&1,%d0			| RLC
	bra	4f
1:	rorb	&1,%d0			| RRC
	bra	4f
2:	cmpib	&2,%d3
	bne	3f
	lslb	&1,%d0			| RAL: through carry
	scs	%d1
	orb	%d2,%d0
	bra	5f
3:	lsrb	&1,%d0			| RAR
	scs	%d1
	lslb	&7,%d2
	orb	%d2,%d0
	bra	5f
4:	scs	%d1
5:	moveb	%d0,%a5@(M_A)
	andb	&F_CY,%d1
	moveb	%a5@(M_F),%d0
	andb	&0xd6,%d0
	orb	&2,%d0
	orb	%d1,%d0
	moveb	%d0,%a5@(M_F)
	addqw	&1,%d7
	bra	Lnext

| everything else through z80dec() and z80exec(), as z80step() does
Lslow:
	movew	%d7,%d4
	eorw	&0x8000,%d4		| pc0
	movew	%d4,%a5@(M_PC)
	pea	%a6@(F_IN)
	moveq	&0,%d0
	movew	%d4,%d0
	movel	%d0,%sp@-
	movel	%a5@(M_MEM),%sp@-
	jsr	z80dec
	lea	%sp@(12),%sp
	tstb	%a6@(F_IN+1)
	bne	1f
	moveq	&X_BAD,%d0
	moveq	&0,%d1
	bra	Lout
1:	moveb	%a6@(F_IN),%d0		| PC past it first
	extw	%d0
	addw	%d4,%d0
	movew	%d0,%a5@(M_PC)
	moveq	&0,%d0
	movew	%d4,%d0
	movel	%d0,%sp@-
	pea	%a6@(F_IN)
	movel	%a5,%sp@-
	jsr	z80exec
	lea	%sp@(12),%sp
	movew	%a5@(M_PC),%d7
	eorw	&0x8000,%d7
	tstl	%d0
	beq	Lnext
	moveq	&1,%d1
	bra	Lout

| class -> handler, in the Z_* order of z80.h
Ltab:
	.word	Lslow-Ltab		| BAD
	.word	Lh_alu-Ltab
	.word	Lh_jcc-Ltab
	.word	Lh_push-Ltab
	.word	Lh_pop-Ltab
	.word	Lh_ldrr-Ltab
	.word	Lh_ldri-Ltab
	.word	Lh_jmp-Ltab
	.word	Lh_lhld-Ltab
	.word	Lh_call-Ltab
	.word	Lh_ret-Ltab		| 10
	.word	Lh_lxi-Ltab
	.word	Lh_lda-Ltab
	.word	Lh_inx-Ltab
	.word	Lh_rot-Ltab
	.word	Lslow-Ltab		| HOOK
	.word	Lh_shld-Ltab
	.word	Lh_dad-Ltab
	.word	Lh_sta-Ltab
	.word	Lh_inr-Ltab
	.word	Lh_ldax-Ltab		| 20
	.word	Lh_dcr-Ltab
	.word	Lh_rcc-Ltab
	.word	Lh_dcx-Ltab
	.word	Lh_xchg-Ltab
	.word	Lh_nop-Ltab
	.word	Lh_cma-Ltab
	.word	Lh_pchl-Ltab
	.word	Lh_ccc-Ltab
	.word	Lslow-Ltab, Lslow-Ltab	| 29
	.word	Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
	.word	Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab, Lslow-Ltab
