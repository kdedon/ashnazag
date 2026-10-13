| The loader takes cputype from AttnFlags, which shows a 68060 only once
| 68060.library has run.  Ask the CPU instead: movec %pcr is an illegal
| instruction on the 68040.  A 68060 also gets superscalar dispatch on,
| as 68060.library would leave it.  Runs with the MMU and interrupts off.

	.text
	.globl	pstart
pstart:
	.word	0x4e7a,0x8801		| movec %vbr,%a0
	movel	%a0@(16),%a1		| illegal-instruction vector
	movel	&Lnot060,%a0@(16)
	movel	%sp,%d1
	.word	0x4e7a,0x0808		| movec %pcr,%d0
	bset	&0,%d0			| ESS
	.word	0x4e7b,0x0808		| movec %d0,%pcr
	moveq	&60,%d0
	movel	%d0,cputype
Lnot060:
	movel	%d1,%sp
	movel	%a1,%a0@(16)
	jmp	__amix_pstart

| AGA fetch mode, colour bank and colour offsets back to their reset
| values once the console's copper list has replaced AmigaOS's: that
| list sets them, and the console's lists only know the ECS registers.
	.globl	screeninit
screeninit:
	jsr	__amix_screeninit
	movew	&0,0xdff1fc		| FMODE
	movew	&0x0c00,0xdff106	| BPLCON3
	movew	&0x0011,0xdff10c	| BPLCON4
	rts
