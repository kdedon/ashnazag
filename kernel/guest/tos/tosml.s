| tosml.s -- the container's machine layer: an application cartridge
| at $FA0000 whose init runs before the disk boot.  It serves drive C:
| from a disk image through the BIOS hard-disk vectors, reading and
| writing the image with host system calls (trap #0), and drive U:
| from a host directory (hostfs.c).
|
|   m68k-elf-as -m68040 tosml.s; ld -Ttext=0xfa0000; objcopy -O binary

	.text
	.globl	_start
_start:
	.long	0xabcdef42
	.long	0			| CA_NEXT
	.long	0x08000000 + init	| CA_INIT: just before the disk boot
	.long	run			| CA_RUN
	.word	0, 0			| CA_TIME, CA_DATE
	.long	end - _start		| CA_SIZE
	.asciz	"TOSML"

| filled in by the launcher
	.org	0x40
p_fd:	.long	-1			| disk image, a host descriptor
p_bpb:	.space	18			| its BPB (512-byte sectors)
	.balign	4
old_bpb: .long	0
old_rw:	.long	0
old_mc:	.long	0

	.org	0x64
	.globl	p_tz, p_root
p_tz:	.long	0			| seconds east of UTC
	.org	0x80
p_root:	.space	256			| directory of drive U:, empty for none

init:
	tst.b	p_root
	beq.s	2f
	jsr	hinit
	move.l	0x84,old_gd		| GEMDOS
	move.l	#gd,0x84
	or.l	#0x100000,0x4c2		| _drvbits: U:
2:	tst.l	p_fd
	bmi.s	1f
	move.l	0x472,old_bpb		| hdv_bpb
	move.l	#bpb,0x472
	move.l	0x476,old_rw		| hdv_rw
	move.l	#rw,0x476
	move.l	0x47e,old_mc		| hdv_mediach
	move.l	#mc,0x47e
	or.l	#4,0x4c2		| _drvbits: C:
	move.w	#2,0x446		| _bootdev
	move.l	0x4f2,a0		| _sysbase
	move.l	8(a0),a0		| os_beg
	move.l	0x28(a0),a0		| os_run
	move.l	(a0),d0			| the running basepage
	beq.s	1f
	cmp.l	0x42e,d0		| phystop
	bcc.s	1f
	move.l	d0,a0
	move.b	#2,0x37(a0)		| p_defdrv: AUTO from C:
1:	rts
run:	rts

| GEMDOS: calls for U: to gemdos(args, &result), others to TOS
	.long	0x58425241		| XBRA
	.long	0x41555855		| AUXU
old_gd:	.long	0
gd:	movem.l	d1-d7/a0-a6,-(sp)
	lea	56+6(sp),a0		| a supervisor caller's arguments
	tst.w	0x59e			| _longframe
	beq.s	1f
	addq.l	#2,a0
1:	btst	#5,56(sp)
	bne.s	2f
	move.l	usp,a0
2:	clr.l	-(sp)
	pea	(sp)
	move.l	a0,-(sp)
	jsr	gemdos
	addq.l	#8,sp
	tst.l	d0
	beq.s	3f
	move.l	(sp)+,d0
	movem.l	(sp)+,d1-d7/a0-a6
	rte
3:	addq.l	#4,sp
	movem.l	(sp)+,d1-d7/a0-a6
	move.l	old_gd,-(sp)
	rts

| GEMDOS from C: arguments as longs
	.globl	Pexec, Mfree
Pexec:	movem.l	d2/a2,-(sp)		| (mode, name, cmd, env)
	move.l	24(sp),-(sp)
	move.l	24(sp),-(sp)
	move.l	24(sp),-(sp)
	move.w	26(sp),-(sp)
	move.w	#0x4b,-(sp)
	trap	#1
	lea	16(sp),sp
	movem.l	(sp)+,d2/a2
	rts
Mfree:	movem.l	d2/a2,-(sp)
	move.l	12(sp),-(sp)
	move.w	#0x49,-(sp)
	trap	#1
	addq.l	#6,sp
	movem.l	(sp)+,d2/a2
	rts

| host system calls: the result, or -errno
	.macro	sys name, num
	.globl	\name
\name:	move.l	#\num,d0
	trap	#0
	bcc.s	1f
	cmp.l	#4,d0			| EINTR: the carrier signal interrupted it
	beq.s	\name
	neg.l	d0
1:	rts
	.endm
	sys	sys_read, 3
	sys	sys_write, 4
	sys	sys_open, 5
	sys	sys_close, 6
	sys	sys_unlink, 10
	sys	sys_chmod, 15
	sys	sys_lseek, 19
	sys	sys_utime, 30
	sys	sys_access, 33
	sys	sys_rmdir, 79
	sys	sys_mkdir, 80
	sys	sys_getdents, 81
	sys	sys_readlink, 90
	sys	sys_statvfs, 103
	sys	_xstat, 123
	sys	_lxstat, 124
	sys	_fxstat, 125
	sys	_xmknod, 126
	sys	sys_rename, 134

| LONG hdv_bpb(WORD dev)
bpb:	cmp.w	#2,4(sp)
	beq.s	1f
	move.l	old_bpb,-(sp)
	rts
1:	move.l	#p_bpb,d0
	rts

| LONG hdv_mediach(WORD dev)
mc:	cmp.w	#2,4(sp)
	beq.s	1f
	move.l	old_mc,-(sp)
	rts
1:	moveq	#0,d0
	rts

| LONG hdv_rw(WORD rw, UBYTE *buf, WORD cnt, WORD recno, WORD dev, LONG lrecno)
rw:	cmp.w	#2,14(sp)
	beq.s	1f
	move.l	old_rw,-(sp)
	rts
1:	movem.l	d1-d3/a0-a1,-(sp)
	moveq	#0,d1
	move.w	20+12(sp),d1		| recno, or lrecno when it is -1
	cmp.w	#-1,d1
	bne.s	2f
	move.l	20+16(sp),d1
2:	moveq	#9,d2
	lsl.l	d2,d1			| byte offset
	moveq	#0,d2
	move.w	20+10(sp),d2
	moveq	#9,d3
	lsl.l	d3,d2			| byte count
	move.l	20+6(sp),a0		| buffer
	moveq	#3,d3			| read
	btst	#0,20+5(sp)
	beq.s	3f
	moveq	#4,d3			| write
3:	clr.l	-(sp)			| lseek(fd, off, 0)
	move.l	d1,-(sp)
	move.l	p_fd,-(sp)
	clr.l	-(sp)
	moveq	#19,d0
	trap	#0
	lea	16(sp),sp
	bcc.s	4f
	cmp.l	#4,d0			| EINTR: the carrier signal interrupted it
	beq.s	3b
	bra.s	9f
4:	move.l	d2,-(sp)		| read or write (fd, buf, n)
	move.l	a0,-(sp)
	move.l	p_fd,-(sp)
	clr.l	-(sp)
	move.l	d3,d0
	trap	#0
	lea	16(sp),sp
	bcc.s	5f
	cmp.l	#4,d0
	beq.s	3b
	bra.s	9f
5:	cmp.l	d2,d0
	bne.s	9f
	moveq	#0,d0
	bra.s	8f
9:	moveq	#-11,d0			| read fault
	cmp.w	#4,d3
	bne.s	8f
	moveq	#-10,d0			| write fault
8:	movem.l	(sp)+,d1-d3/a0-a1
	rts
end:
