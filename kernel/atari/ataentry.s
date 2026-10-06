| ataentry.s -- Atari entry shim, early exception catcher, final stop.
|
| Entered at the load address with the MMU off, supervisor, IPL 7; the
| Linux/m68k boot record lies at the first even address past the image.
| Leaves for stext with d0 = 'AtrB', d1 = copied boot record.

	.text
	.globl	atari_entry
atari_entry:
	movew	&0x2700,%sr
	lea	ata_stack_top,%sp

	| catch everything until stext loads M68Kvec
	lea	ata_evec,%a0
	movel	&ata_early_exc,%d0
	movel	&255,%d1
Lvfill:
	movel	%d0,%a0@+
	dbf	%d1,Lvfill
	lea	ata_evec,%a0
	movec	%a0,%vbr
	moveq	&0,%d0
	movec	%d0,%cacr

	| boot record: tag 1 (BI_MACHTYPE), size 8, MACH_ATARI
	movel	&end,%d0
	addql	&1,%d0
	andil	&0xfffffffe,%d0
	moveal	%d0,%a0
	movel	&0x8000,%d1
Lscan:
	cmpil	&0x00010008,%a0@
	bne.w	Lnext
	cmpil	&2,%a0@(4)
	beq.w	Lfound
Lnext:
	addql	&2,%a0
	subql	&1,%d1
	bne.w	Lscan
	suba	%a0,%a0
Lfound:
.ifdef ATA060
	| copied to FastRAM: enter the copy, which finds the record past its end
	moveal	%a0,%a2
	movel	%a0,%sp@-
	jsr	ata_reloc
	addql	&4,%sp
	tstl	%d0
	beq.w	Lstay
	addl	&atari_entry,%d0
	moveal	%d0,%a0
	jmp	%a0@
Lstay:
	moveal	%a2,%a0
.endif
	movel	%a0,%sp@-
	jsr	ata_shim_main
	addql	&4,%sp
	movel	&0x41747242,%d0
	movel	&ata_bi,%d1
	jmp	stext

| Early exception: print vector, PC and SR, stop.
ata_early_exc:
	movel	%sp,%a2
	pea	Lexc
	jsr	ata_puts
	moveq	&0,%d0
	movew	%a2@(6),%d0
	andiw	&0x0fff,%d0
	lsrl	&2,%d0
	movel	%d0,%sp@-
	jsr	ata_puthex
	pea	Lexcpc
	jsr	ata_puts
	movel	%a2@(2),%sp@-
	jsr	ata_puthex
	pea	Lexcsr
	jsr	ata_puts
	moveq	&0,%d0
	movew	%a2@,%d0
	movel	%d0,%sp@-
	jsr	ata_puthex
.ifdef ATA060
	| format 4 (060 access error): fault address and FSLW
	movew	%a2@(6),%d0
	andiw	&0xf000,%d0
	cmpiw	&0x4000,%d0
	bne.w	Lexcend
	pea	Lexcea
	jsr	ata_puts
	movel	%a2@(8),%sp@-
	jsr	ata_puthex
	pea	Lexcfs
	jsr	ata_puts
	movel	%a2@(12),%sp@-
	jsr	ata_puthex
Lexcend:
.endif
	pea	Lnl
	jsr	ata_stop

| ata_stop(msg): MMU and caches off on a physical stack, print, spin.
	.globl	ata_stop
ata_stop:
	movew	&0x2700,%sr
	movel	%sp@(4),%a2
	lea	ata_stack_top,%sp
	bsr.w	Lmmuoff
	jsr	fbcons_unlock		| screen: draw what an interrupted owner queued
	movel	%a2,%sp@-
	jsr	ata_puts
Lspin:
	stop	&0x2700
	bra.w	Lspin

| ata_restart(): MMU and caches off, then the ROM's reset entry.
	.globl	ata_restart
ata_restart:
	movew	&0x2700,%sr
	lea	ata_stack_top,%sp
	bsr.w	Lmmuoff
	moveal	0x00e00004,%a0
	jmp	%a0@

Lmmuoff:
.ifdef ATA060
	.word	0xf4f8			| cpusha bc
	moveq	&0,%d0
	movec	%d0,%cacr
	.word	0x4e7b,0x0003		| movec %d0,%tc
	.word	0x4e7b,0x0004		| movec %d0,%itt0
	.word	0x4e7b,0x0005		| movec %d0,%itt1
	.word	0x4e7b,0x0006		| movec %d0,%dtt0
	.word	0x4e7b,0x0007		| movec %d0,%dtt1
	.word	0xf518			| pflusha
.else
	moveq	&0,%d0
	movec	%d0,%cacr
	lea	Lzero,%a0
	.word	0xf010,0x4000		| pmove (%a0),%tc
.endif
	rts

	.data
Lexc:	.asciz	"\natari: early exception, vector "
Lexcpc:	.asciz	" pc "
Lexcsr:	.asciz	" sr "
Lnl:	.asciz	"\n"
.ifdef ATA060
Lexcea:	.asciz	" ea "
Lexcfs:	.asciz	" fslw "
.endif
	.even
Lzero:	.long	0
	.balign	4
ata_evec:
	.space	1024
ata_stack:
	.space	4096
ata_stack_top:
	.long	0
	.balign	4

| Debug output channel (boot option nfcons): native-feature calls, which
| trap as illegal instructions where there is no host to take them.
	.text
	.globl	ata_nfid
	.globl	ata_nfcall
ata_nfid:				| ata_nfid(name): id, 0 if none
	.word	0x4e7a,0x8801		| movec %vbr,%a0
	movel	%a0@(16),%sp@-
	movel	&Lnfill,%a0@(16)
	moveq	&0,%d0
	movel	%sp@(8),%sp@-
	bsr.w	Lnfid
	addql	&4,%sp
	.word	0x4e7a,0x8801
	movel	%sp@+,%a0@(16)
	rts
Lnfid:
	.word	0x7300
	rts
Lnfill:
	addql	&2,%sp@(2)
	moveq	&0,%d0
	rte
| The host reads the arguments at the physical stack address, so the
| call runs on a stack that is mapped VA = PA, with interrupts off.
ata_nfcall:				| ata_nfcall(id, arg)
	movew	%sr,%sp@-
	oriw	&0x0700,%sr
	movel	%sp@(6),%d0
	movel	%sp@(10),%d1
	movel	%sp,%a1
	lea	ata_nfstk,%sp
	movel	%d1,%sp@-
	movel	%d0,%sp@-
	clrl	%sp@-
.ifdef ATA060
	.word	0xf478			| cpusha dc: the host reads memory, not the copyback cache
.endif
	.word	0x7301
	movel	%a1,%sp
	movew	%sp@+,%sr
	rts
	.data
	.space	64
ata_nfstk:
	.text
