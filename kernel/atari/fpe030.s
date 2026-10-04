| fpe030.s -- 68030 F-line entry to the FPU emulator.
|
| Without an FPU the 030 takes every FPU instruction at vector 11 with a
| four-word format-0 frame whose PC is the F-line word.  A user F-line goes
| to fpe_trap once fpuinit has armed the emulator; anything else keeps the
| stock handler.  The stack built for fpe_trap is the one nullvect builds:
| USP pseudo-register (u.u_ar0 points at it), D0-D7/A0-A6, then the frame.

	U_AR0	=	0x40000864

	.text
	.globl	ata_fline
ata_fline:
	tstl	fpu_emul
	beqs	Lfl_decline
	btst	&5,%sp@			| S bit in the stacked SR
	bnes	Lfl_decline
	cmpiw	&0x002c,%sp@(6)
	bnes	Lfl_decline
	addql	&1,fpe_entry_n
	moveml	%d0-%d7/%a0-%a6,%sp@-
	movel	sup_cacr,%d0
	.word	0x4e7b,0x0002		| movec %d0,%cacr
	movel	%usp,%a0
	movel	%a0,%sp@-
	movel	%sp,U_AR0
	moveal	%sp,%a0
	pea	%a0@(64)		| exception frame
	pea	%a0@(4)			| register block
	pea	%a0@			| USP slot
	jsr	fpe_trap
	lea	%sp@(12),%sp
	moveal	%sp@+,%a0
	movel	%a0,%usp		| the emulation may move A7
	jmp	ureturn
Lfl_decline:
	jmp	nullvect

| The stock 030 sendsig calls fpu_setup directly; there is no gated copy.
	.globl	fpu_setup_gated_fpe_orig
fpu_setup_gated_fpe_orig:
	jmp	fpu_setup_fpe_orig

| The CPU type, and the one the emulator glue is built to read.
	.data
	.globl	cputype, fpe_glue_cputype
cputype:
	.long	30
fpe_glue_cputype:
	.long	40
