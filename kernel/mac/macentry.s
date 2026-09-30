| macentry.s -- Macintosh entry shim, early exception catcher, final stop.
|
| Entered with the MMU off, supervisor, stack undefined.  Two hand-offs:
|   direct -kernel boot: bootinfo record at the first even address
|     past the loaded image;
|   A/UX launch: d0 = 0x536D72xx, a0 = info block; converted by aux_entry,
|     which re-enters here as a direct boot.
| Leaves for stext with d0 = 'MacB', d1 = copied boot record.

	.text
	.globl	mac_entry
mac_entry:
	movew	&0x2700,%sr
	movel	%d0,mac_entry_d0
	movel	%a0,mac_entry_a0
	lea	mac_stack_top,%sp

	| catch everything until stext loads M68Kvec
	lea	mac_evec,%a0
	movel	&mac_early_exc,%d0
	movel	&255,%d1
Lvfill:
	movel	%d0,%a0@+
	dbf	%d1,Lvfill
	lea	mac_evec,%a0
	movec	%a0,%vbr

	jsr	mac_scc_init

	movel	mac_entry_d0,%d0
	cmpil	&0x536d7201,%d0
	beq.w	Laux

	| boot record: tag 1 (BI_MACHTYPE), size 8, MACH_MAC
	movel	&end,%d0
	addql	&1,%d0
	andil	&0xfffffffe,%d0
	moveal	%d0,%a0
	movel	&0x8000,%d1
Lscan:
	cmpil	&0x00010008,%a0@
	bne.w	Lnext
	cmpil	&3,%a0@(4)
	beq.w	Lfound
Lnext:
	addql	&2,%a0
	subql	&1,%d1
	bne.w	Lscan
	pea	Lnobi
	jsr	mac_halt

Lfound:
	movel	%a0,%sp@-
	jsr	mac_shim_main
	addql	&4,%sp
	movel	&0x4d616342,%d0
	movel	&mac_bi,%d1
	jmp	stext

Laux:
	moveal	mac_entry_a0,%a0
	movel	mac_entry_d0,%d0
	jmp	aux_entry

| Early exception: print vector and PC, stop.
mac_early_exc:
	movel	%sp,%a2
	pea	Lexc
	jsr	mac_puts
	moveq	&0,%d0
	movew	%a2@(6),%d0
	andiw	&0x0fff,%d0
	lsrl	&2,%d0
	movel	%d0,%sp@-
	jsr	mac_puthex
	pea	Lexcpc
	jsr	mac_puts
	movel	%a2@(2),%sp@-
	jsr	mac_puthex
	pea	Lexcsr
	jsr	mac_puts
	moveq	&0,%d0
	movew	%a2@,%d0
	movel	%d0,%sp@-
	jsr	mac_puthex
	pea	Lnl
	jsr	mac_stop

| mac_stop(msg): MMU, TT and caches off on a physical stack, print, spin.
	.globl	mac_stop
mac_stop:
	movew	&0x2700,%sr
	movel	%sp@(4),%a2
	lea	mac_stack_top,%sp
	bsr.w	Lmmuoff
	jsr	fbcons_unlock		| screen: draw what an interrupted owner queued
	movel	%a2,%sp@-
	jsr	mac_puts
Lspin:
	stop	&0x2700
	bra.w	Lspin

| mac_restart(): MMU, TT and caches off, then the ROM's restart entry.
	.globl	mac_restart
mac_restart:
	movew	&0x2700,%sr
	lea	mac_stack_top,%sp
	bsr.w	Lmmuoff
	moveal	4,%a0
	jmp	%a0@

| Translation and caches off.  The reset vectors at 0 get the ROM's
| restart entry (ROM + $0A), so an external reset restarts through it too.
Lmmuoff:
	moveq	&0,%d0
	.word	0xf4f8			| cpusha bc
	.word	0x4e7b,0x0003		| movec %d0,%tc
	.word	0x4e7b,0x0004		| movec %d0,%itt0
	.word	0x4e7b,0x0005		| movec %d0,%itt1
	.word	0x4e7b,0x0006		| movec %d0,%dtt0
	.word	0x4e7b,0x0007		| movec %d0,%dtt1
	.word	0xf518			| pflusha
	movec	%d0,%cacr
	.word	0xf4d8			| cinva bc: no stale lines once caches are back on
	movel	&0x40800000,%a0
	movel	%a0@,0
	pea	%a0@(10)
	movel	%sp@+,4
	rts

	.data
Lnobi:	.asciz	"entry: no boot record after the image"
Lexc:	.asciz	"\nmac: early exception, vector "
Lexcpc:	.asciz	" pc "
Lexcsr:	.asciz	" sr "
Lnl:	.asciz	"\n"
	.even
	.globl	mac_entry_d0
	.globl	mac_entry_a0
mac_entry_d0:	.long	0
mac_entry_a0:	.long	0
	.balign	4
mac_evec:
	.space	1024
mac_stack:
	.space	4096
mac_stack_top:
	.long	0
	.balign	4

| Control-register readers for the MMU report.
	.text
	.globl	mac_rd_tc
	.globl	mac_rd_itt0
	.globl	mac_rd_dtt0
	.globl	mac_rd_dtt1
	.globl	mac_rd_srp
mac_rd_tc:
	.word	0x4e7a,0x0003
	rts
mac_rd_itt0:
	.word	0x4e7a,0x0004
	rts
mac_rd_dtt0:
	.word	0x4e7a,0x0006
	rts
mac_rd_dtt1:
	.word	0x4e7a,0x0007
	rts
mac_rd_srp:
	.word	0x4e7a,0x0807
	rts
	.balign	4
