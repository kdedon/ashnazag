| Kernel entry, before config().  The loader takes cputype from AttnFlags,
| which shows a 68060 only once 68060.library has run, so ask the CPU:
| movec %pcr is an illegal instruction on the 68040.  A 68060 gets
| superscalar dispatch on and its branch cache cleared: the cache may
| still predict branches of the code the kernel was copied over.
| Interrupts are off, the MMU is off and VBR is the kernel's.
|
| Boot progress shows in COLOR00 with copper and bitplane DMA off, so the
| whole native screen takes the colour until the console starts:
| red at entry, yellow while pstart builds the MMU tables, green with the
| MMU on, blue as the console starts.
|
| Paula's interrupt sources start off: AmigaOS leaves its own enabled, and
| the drivers enable what they use.  After config() the boards' interrupts
| go off too.

	.set	DMACON,0xdff096
	.set	INTENA,0xdff09a
	.set	INTREQ,0xdff09c
	.set	COLOR00,0xdff180

	.text
	.globl	config_cachefix
config_cachefix:
	movel	&0x414d5842,0x1000	| 'AMXB': boot marks, see bootmark.s
	movel	&0x414d5842,0x1ff000
	moveq	&6,%d0
	lea	0x1004,%a0
	lea	0x1ff004,%a1
1:	clrl	%a0@+
	clrl	%a1@+
	dbf	%d0,1b
	movel	&1,0x1008
	movel	&1,0x1ff008
	movel	&1,0x1004
	movel	&1,0x1ff004
	movew	&0x0180,DMACON		| copper and bitplane DMA off
	movew	&0x0f00,COLOR00		| red
	movew	&0x7fff,INTENA
	movew	&0x7fff,INTREQ
	movew	&0x7fff,INTREQ
	.word	0x4e7a,0x8801		| movec %vbr,%a0
	movel	%a0@(16),%a1		| illegal-instruction vector
	movel	&Lnot060,%a0@(16)
	movel	%sp,%d1
	.word	0x4e7a,0x0808		| movec %pcr,%d0
	bset	&0,%d0			| ESS
	.word	0x4e7b,0x0808		| movec %d0,%pcr
	movel	&0x00400000,%d0		| CABC
	.word	0x4e7b,0x0002		| movec %d0,%cacr
	moveq	&60,%d0
	movel	%d0,cputype
Lnot060:
	movel	%d1,%sp
	movel	%a1,%a0@(16)
	movel	%sp@(8),%sp@-
	movel	%sp@(8),%sp@-
	jsr	amiga_bootmem
	jsr	__amix_config_cachefix
	addql	&8,%sp
	movel	%a0,%sp@-
	pea	3
	jsr	bootmark
	addql	&8,%sp
	movew	0xdff004,%d0		| VPOSR: Alice is AGA, Agnus the A3000
	andw	&0x0e00,%d0
	bne	Laga
	bset	&7,0xde0002		| bus timeout, an A3000 Gary register
Laga:
	jmp	amiga_quiesce

	.globl	pstart
pstart:
	movew	&0x0ff0,COLOR00		| yellow
	clrl	%sp@-
	pea	5
	jsr	bootmark
	addql	&8,%sp
	jsr	__amix_pstart
	movew	&0x00f0,COLOR00		| green
	movel	%a0,%sp@-
	pea	6
	jsr	bootmark
	addql	&8,%sp
	clrb	bootmk_on
	rts

| AGA fetch mode, colour bank and colour offsets back to their reset
| values once the console's copper list has replaced AmigaOS's: that
| list sets them, and the console's lists only know the ECS registers.
	.globl	screeninit
screeninit:
	movew	&0x000f,COLOR00		| blue
	jsr	__amix_screeninit
	movew	&0,0xdff1fc		| FMODE
	movew	&0x0c00,0xdff106	| BPLCON3
	movew	&0x0011,0xdff10c	| BPLCON4
	rts
