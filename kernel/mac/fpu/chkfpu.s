| chk_fpu(fsave, fregs): FRESTORE a null frame, then FSAVE into fsave.
| A trap means no FPU.  An idle frame also counts; it is stored as null so
| each new process starts from a reset FPU.  Boot option nofpu skips it.
	.text
	.globl	chk_fpu
chk_fpu:
	clrl	fpu_present
	tstl	mac_nofpu
	bnes	Lbye
	movel	&1,fpu_present
	.word	0x4e7a,0x8801		| movec %vbr,%a0
	movel	%a0@(44),%sp@-
	movel	&Ltrap,%a0@(44)
	lea	Lnull,%a1
	frestore %a1@
	moveal	%sp@(8),%a1
	fsave	%a1@
Lcont:
	movel	%sp@+,%a0@(44)
	tstl	fpu_present
	beqs	Lbye
	moveal	%sp@(4),%a1
	tstb	%a1@
	beqs	Lok
	cmpiw	&0x4100,%a1@		| 68040 idle frame
	bnes	Lnone
	clrl	%a1@
	bras	Lok
Lnone:
	clrl	fpu_present
	bras	Lbye
Lok:
	moveal	%sp@(8),%a0
	fmovemx	%fp0-%fp7,%a0@
	fmovel	%fpcr,%a0@(96)
	fmovel	%fpsr,%a0@(100)
	fmovel	%fpiar,%a0@(104)
Lbye:
	rts
Ltrap:
	clrl	fpu_present
	movel	&Lcont,%sp@(2)
	rte

	.data
	.globl	mac_nofpu
mac_nofpu:
	.long	0
Lnull:
	.long	0
