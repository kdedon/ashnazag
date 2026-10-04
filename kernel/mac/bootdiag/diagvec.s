| diagvec.s -- exception counting and the idle loop for the boot status lines.
|
| diag_vecinit points VBR at a table whose every entry (except the reset
| pair) is diag_vstub.  The stub counts the vector, notes the last one and
| the last non-interrupt, non-syscall one with its PC, then jumps through
| the previous table, read at each exception.

	.text

	.globl	diag_vecinit
diag_vecinit:
	movec	%vbr,%a0
	cmpal	&diag_vtab,%a0
	beq.s	Lvi_done
	movel	%a0,diag_ovbr
	lea	diag_vtab,%a1
	movel	%a0@,%a1@+
	movel	%a0@(4),%a1@+
	movel	&diag_vstub,%d0
	movel	&253,%d1
Lvi_fill:
	movel	%d0,%a1@+
	dbf	%d1,Lvi_fill
	lea	diag_vtab,%a0
	movec	%a0,%vbr
Lvi_done:
	rts

	.globl	diag_vstub
diag_vstub:
	subql	&4,%sp			| room for the handler address
	movel	%d0,%sp@-
	movel	%a0,%sp@-
	movew	%sp@(18),%d0		| format/vector word of the frame
	andiw	&0x3fc,%d0
	lea	diag_vcnt,%a0
	addql	&1,%a0@(0,%d0:w)
	movew	%d0,diag_vlast
	cmpiw	&0x60,%d0		| autovectors and trap #0 are routine
	blt.s	Lvs_flt
	cmpiw	&0x80,%d0
	ble.s	Lvs_go
Lvs_flt:
	movew	%d0,diag_fvec
	movel	%sp@(14),diag_fpc
Lvs_go:
	moveal	diag_ovbr,%a0
	movel	%a0@(0,%d0:w),%sp@(8)
	movel	%sp@+,%a0
	movel	%sp@+,%d0
	rts

| idle: STOP, or with nostop a short spin with interrupts open.
	.globl	idle
idle:
	addql	&1,diag_nidle
	jsr	diag_idle
	movel	diag_opts,%d0
	btst	&1,%d0
	bne.s	Lid_spin
	movel	&1,diag_instop
	stop	&0x2000
	clrl	diag_instop
	addql	&1,diag_nstopret
	rts
Lid_spin:
	movew	&0x2000,%sr
	jsr	diag_spin
	rts

	.data
	.balign	4
diag_ovbr:	.long	0
	.globl	diag_vlast, diag_fvec, diag_fpc
diag_vlast:	.word	0
diag_fvec:	.word	0
diag_fpc:	.long	0

	.bss
	.balign	4
	.globl	diag_vcnt
diag_vtab:	.space	1024
diag_vcnt:	.space	1024
