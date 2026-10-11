| entry.S -- the MiNT layer's entry points: program start, the shell
| process EmuTOS starts, trap #1 and #2, the 50 Hz tick, context switch,
| host system calls and the thunks behind unserved table slots.

	.globl	__start, _xa_osentry, _xa_trap1, _xa_trap2, _xa_tick
	.globl	_old_trap1, _old_trap2, _old_etv, _xa_swtch, _xa_kstart, _xa_idle
	.globl	_xa_tthunks, _xa_faults
	.globl	_hc, _hc_d1, _xa_thunks, _xa_spl7, _xa_splx, _xa_getsr
	.globl	_trap_13_emu, _trap_14_emu
	.globl	_xa_setusp, _xa_sigtramp
	.globl	_ks_usp, _ks_pc, _ks_sr, _ks_regs, _xa_super, _xa_usp

	.text
| AUTO start: run xa_auto, stay resident if it returns a size
__start:
	move.l	4(sp),a0
	lea	_bootstk+16384,sp
	move.l	a0,-(sp)
	jsr	_xa_auto
	tst.l	d0
	beq.s	1f
	clr.w	-(sp)
	move.l	d0,-(sp)
	move.w	#0x31,-(sp)		| Ptermres
	trap	#1
1:	clr.w	-(sp)
	trap	#1

| exec_os: the shell process; becomes the MiNT root
_xa_osentry:
	move.l	4(sp),a0
	lea	_bootstk+16384,sp
	move.l	a0,-(sp)
	jsr	_xa_boot
2:	bra.s	2b

| Super(0) through EmuTOS, from user mode, keeping this stack
_xa_super:
	clr.l	-(sp)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	rts

| GEMDOS: served from dos_trap[] with the caller's arguments copied raw,
| as the functions are built with 16-bit int; a null slot goes to TOS
	.long	0x58425241, 0x58414d54
_old_trap1:
	.long	0
_xa_trap1:
	movem.l	d1-d7/a0-a6,-(sp)
	btst	#5,56(sp)
	bne.s	1f
	move.l	usp,a0
	bra.s	2f
1:	lea	64(sp),a0
2:	moveq	#0,d1
	move.w	(a0)+,d1
	cmp.w	#0x11a,d1
	beq.s	Lsigret
	cmp.w	#0x190,d1
	bcc.s	Lpass1
	lea	_dos_trap,a1
	move.l	0(a1,d1.l*4),d1
	beq.s	Lpass1
	move.l	d1,a1
	lea	32(a0),a0
	move.l	-(a0),-(sp)
	move.l	-(a0),-(sp)
	move.l	-(a0),-(sp)
	move.l	-(a0),-(sp)
	move.l	-(a0),-(sp)
	move.l	-(a0),-(sp)
	move.l	-(a0),-(sp)
	move.l	-(a0),-(sp)
	jsr	(a1)
	lea	32(sp),sp
	movem.l	(sp)+,d1-d7/a0-a6
	bra	Ltret
Lpass1:	movem.l	(sp)+,d1-d7/a0-a6
	move.l	_old_trap1,-(sp)
	rts

| Psigreturn: the registers and frame to xa_sigreturn, which may put
| back a handler's interrupted context
Lsigret:
	pea	(sp)
	jsr	_xa_sigreturn
	addq.l	#4,sp
	movem.l	(sp)+,d1-d7/a0-a6
	bra	Ltret

| return from a trap: a process going back to user mode yields first
| if the tick asked for it, and takes its pending signals
Ltret:	btst	#5,(sp)
	bne.s	1f
	tst.w	_xa_resched
	bne.s	2f
	move.l	a0,-(sp)
	move.l	_curproc,a0
	add.l	_xa_sigoff,a0
	tst.l	(a0)
	movem.l	(sp)+,a0		| keeps the flags
	beq.s	1f
2:	movem.l	d0-d7/a0-a6,-(sp)
	pea	60(sp)			| the exception frame
	jsr	_xa_trapret
	addq.l	#4,sp
	movem.l	(sp)+,d0-d7/a0-a6
1:	rte

| AES ($C8) to the registered dispatcher in the caller's context,
| appl_yield ($C9) yields, the rest (VDI) to the old vector
	.long	0x58425241, 0x58414d54
_old_trap2:
	.long	0
_xa_trap2:
	cmp.w	#0xc8,d0
	beq.s	1f
	cmp.w	#0xc9,d0
	beq.s	2f
	move.l	_old_trap2,-(sp)
	rts
1:	tst.l	_aes_disp
	bne.s	3f
	move.l	_old_trap2,-(sp)
	rts
3:	movem.l	d1-d7/a0-a6,-(sp)
	move.l	d1,-(sp)
	move.l	_aes_disp,a0
	jsr	(a0)
	addq.l	#4,sp
	movem.l	(sp)+,d1-d7/a0-a6
	bra	Ltret
2:	movem.l	d0-d7/a0-a6,-(sp)
	jsr	_xa_yield
	movem.l	(sp)+,d0-d7/a0-a6
	bra	Ltret

| etv_timer, 50 Hz from the 200 Hz timer
	.long	0x58425241, 0x58414d54
_old_etv:
	.long	0
_xa_tick:
	movem.l	d0-d2/a0-a2,-(sp)
	jsr	_xa_tick_c
	movem.l	(sp)+,d0-d2/a0-a2
	move.l	_old_etv,-(sp)
	rts

| xa_swtch(long *save, long sp): callee-saved registers and USP on this
| stack, its pointer to *save; then the other stack's
_xa_swtch:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	usp,a0
	move.l	a0,-(sp)
	move.l	52(sp),a0
	move.l	56(sp),a1
	move.l	sp,(a0)
	move.l	a1,sp
	move.l	(sp)+,a0
	move.l	a0,usp
	movem.l	(sp)+,d2-d7/a2-a6
	rts

| a new process's first switch lands here: a kernel thread runs its
| function; a program enters user mode at ks_pc with ks_regs
_xa_kstart:
	jsr	_xa_kstart_c
	move.w	#0x2700,sr
	move.l	_ks_usp,a0
	move.l	a0,usp
	clr.w	-(sp)
	move.l	_ks_pc,-(sp)
	move.w	_ks_sr,-(sp)
	movem.l	_ks_regs,d0-d7/a0-a6
	rte

| faults: the vector to xa_fault with the registers and frame
_xa_faults:
	move.w	#2,-(sp)
	bra.s	Lfault
	move.w	#3,-(sp)
	bra.s	Lfault
	move.w	#4,-(sp)
	bra.s	Lfault
	move.w	#5,-(sp)
	bra.s	Lfault
	move.w	#8,-(sp)
	bra.s	Lfault
Lfault:
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	sp,-(sp)
	jsr	_xa_fault
	addq.l	#4,sp
	movem.l	(sp)+,d0-d7/a0-a6
	addq.l	#2,sp
	rte

_xa_idle:
	stop	#0x2000
	rts

_xa_spl7:
	move.w	sr,d0
	move.w	#0x2700,sr
	rts
_xa_splx:
	move.w	4(sp),sr
	rts
_xa_getsr:
	move.w	sr,d0
	rts
_xa_usp:
	move.l	usp,a0
	move.l	a0,d0
	rts
_xa_setusp:
	move.l	4(sp),a0
	move.l	a0,usp
	rts

| a user signal handler returns here: Psigreturn
_xa_sigtramp:
	move.w	#0x11a,-(sp)
	trap	#1
1:	bra.s	1b

| trap_13_emu(short fn, ...), trap_14_emu: BIOS and XBIOS with the
| caller's arguments as they lie
_trap_13_emu:
	movem.l	d2/a2,-(sp)
	lea	46(sp),a0
	moveq	#16,d0
1:	move.w	-(a0),-(sp)
	dbra	d0,1b
	trap	#13
	lea	34(sp),sp
	movem.l	(sp)+,d2/a2
	rts
_trap_14_emu:
	movem.l	d2/a2,-(sp)
	lea	46(sp),a0
	moveq	#16,d0
1:	move.w	-(a0),-(sp)
	dbra	d0,1b
	trap	#14
	lea	34(sp),sp
	movem.l	(sp)+,d2/a2
	rts

| hc(n, nargs, args...): host system call n through the cartridge's
| trap #0; the result, or -errno.  d1's result in hc_d1.
_hc:
	link	a6,#0
	movem.l	d2/a2,-(sp)
	move.l	12(a6),d1
	lea	16(a6,d1.l*4),a0
	bra.s	2f
1:	move.l	-(a0),-(sp)
2:	dbra	d1,1b
	pea	3f
	move.l	8(a6),d0
	move.l	0xfa007c,a0
	jmp	(a0)
3:	bcs.s	4f
	move.l	d1,_hc_d1
	bra.s	5f
4:	neg.l	d0
5:	lea	-8(a6),sp
	movem.l	(sp)+,d2/a2
	unlk	a6
	rts

| 1024 thunks of 8 bytes: an unserved slot's number to xa_unserved
_xa_thunks:
	.set	n, 0
	.rept	1024
	move.w	#n,-(sp)
	bra.w	Lthunk
	.set	n, n + 1
	.endr
Lthunk:
	jsr	_xa_unserved
	addq.l	#2,sp
	rts

| tracing thunks of 10 bytes: log slot n, then on to its function
_xa_tthunks:
	.set	n, 0
	.rept	1024
	move.l	#n,-(sp)
	bra.w	Ltrace
	.set	n, n + 1
	.endr
Ltrace:
	movem.l	d0-d2/a0-a2,-(sp)
	move.l	24(sp),-(sp)
	jsr	_xa_trace
	addq.l	#4,sp
	move.l	d0,24(sp)
	movem.l	(sp)+,d0-d2/a0-a2
	rts

	.bss
	.even
_hc_d1:	.space	4
_ks_usp: .space	4
_ks_pc:	.space	4
_ks_sr:	.space	2
	.even
_ks_regs: .space 60
_bootstk: .space 16384
