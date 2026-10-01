| tosml.s -- the container's machine layer: an application cartridge
| at $FA0000 whose init runs before the disk boot.  It serves drive C:
| from a disk image through the BIOS hard-disk vectors, reading and
| writing the image with host system calls (trap #0), and drive U:
| from a host directory (hostfs.c).  Each VBL it hands the mouse and
| keyboard events the display process posts in its last page straight
| to TOS's handlers.
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
	jsr	pvinit
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

| input posted by the display process (struct tospv)
	.set	PV, 0xfbf000
	.set	pv_on, 0
	.set	pv_head, 4
	.set	pv_tail, 8
	.set	pv_xy, 12
	.set	pv_cxy, 16
	.set	pv_btn, 20
	.set	pv_npkt, 24
	.set	pv_nkey, 28
	.set	pv_ev, 32
	.set	PV_NEV, 256
	.set	pv_vbl, pv_ev+PV_NEV*8
	.set	pv_drop, pv_vbl+4

kbv:	.long	0			| Kbdvbase()
kio:	.long	0			| the keyboard's Iorec
ikv:	.long	0			| $118 and ikbdsys as TOS set them
iks:	.long	0
pvk:	.long	0			| PV_KEYS when TOS has kbdvec
pkt:	.byte	0, 0, 0, 0		| relative mouse packet

| the last free VBL queue slot runs pv: GEM takes the first for its cursor
pvinit:	move.w	#34,-(sp)		| Kbdvbase
	trap	#14
	addq.l	#2,sp
	move.l	d0,kbv
	move.l	d0,a0
	move.l	32(a0),iks
	move.l	0x118,ikv
	move.w	#1,-(sp)		| Iorec(1)
	move.w	#14,-(sp)
	trap	#14
	addq.l	#4,sp
	move.l	d0,kio
	move.l	0x4f2,a0		| _sysbase
	move.l	8(a0),a0		| os_beg
	cmp.w	#0x0200,2(a0)		| TOS 2 and later: kbdvec before the vectors
	bcs.s	1f
	move.l	#2,pvk
1:	move.w	0x454,d0		| nvbls
	move.l	0x456,a0		| _vblqueue
	lea	(a0,d0.w*4),a0
	bra.s	3f
2:	tst.l	-(a0)
	bne.s	3f
	move.l	#pv,(a0)
	rts
3:	dbra	d0,2b
	rts

| each VBL: while TOS owns the IKBD vectors, events reach its handlers
| here; when a program takes them, the IKBD carries everything
pv:	movem.l	d0-d7/a0-a6,-(sp)
	lea	PV,a5
	move.l	kbv,a4
	addq.l	#1,pv_vbl(a5)
	move.l	pv_tail(a5),d2
	move.l	pv_drop(a5),d0
	sub.l	d2,d0
	ble.s	0f
	move.l	pv_head(a5),d1
	sub.l	d2,d1
	cmp.l	d1,d0
	bhi.s	0f
	add.l	d0,pv_tail(a5)		| sent by the IKBD instead
0:	moveq	#0,d7
	move.l	0x118,d0
	cmp.l	ikv,d0
	bne.s	1f
	move.l	32(a4),d0
	cmp.l	iks,d0
	bne.s	1f
	moveq	#1,d7
	or.l	pvk,d7
1:	move.l	d7,pv_on(a5)
	move.l	pv_tail(a5),d6
	move.l	pv_xy(a5),d5
	cmp.l	pv_head(a5),d6
	bne.s	2f
	cmp.l	pv_cxy(a5),d5
	beq	9f
2:	tst.l	d7
	bne.s	3f
	move.l	pv_head(a5),pv_tail(a5)	| the IKBD's now: drop these
	move.l	d5,pv_cxy(a5)
	bra.s	9f
3:	move.w	sr,-(sp)		| as from the IKBD interrupt
	move.w	(sp),d0
	and.w	#0xf8ff,d0
	or.w	#0x0600,d0
	move.w	d0,sr
4:	cmp.l	pv_head(a5),d6
	beq.s	8f
	move.l	#PV_NEV-1,d0
	and.l	d6,d0
	lsl.l	#3,d0
	lea	pv_ev(a5,d0.l),a3
	move.l	4(a3),d0
	bsr	mot
	move.w	(a3),d0
	cmp.w	#0x0200,d0
	bcc.s	5f
	btst	#1,d7			| a key
	beq.s	6f
	and.l	#0xff,d0
	move.l	kio,a0
	move.l	-4(a4),a1		| kbdvec
	movem.l	d5-d7/a3-a5,-(sp)
	jsr	(a1)
	movem.l	(sp)+,d5-d7/a3-a5
	addq.l	#1,pv_nkey(a5)
	bra.s	6f
5:	and.l	#3,d0			| buttons
	move.l	d0,pv_btn(a5)
	move.l	d0,d4
	moveq	#0,d1
	moveq	#0,d2
	bsr	pkt3
6:	addq.l	#1,d6
	move.l	d6,pv_tail(a5)
	bra.s	4b
8:	move.l	pv_xy(a5),d0		| motion posted meanwhile too
	bsr	mot
	move.w	(sp)+,sr
9:	movem.l	(sp)+,d0-d7/a0-a6
	rts

| motion up to d0 (x << 16 | y), at most a screen's width, in packets
mot:	move.l	pv_cxy(a5),d1
	move.l	d0,pv_cxy(a5)
	move.w	d0,d3
	sub.w	d1,d3
	ext.l	d3
	swap	d0
	swap	d1
	sub.w	d1,d0
	ext.l	d0
	move.l	#1280,d4
	bsr.s	clip
	exg	d0,d3
	bsr.s	clip
	exg	d0,d3			| d0 dx, d3 dy
1:	move.l	d0,d1
	or.l	d3,d1
	beq.s	2f
	moveq	#127,d4
	move.l	d0,-(sp)
	bsr.s	clip
	move.l	d0,d1
	move.l	d3,d0
	bsr.s	clip
	move.l	d0,d2
	move.l	(sp)+,d0
	sub.l	d1,d0
	sub.l	d2,d3
	move.l	pv_btn(a5),d4
	movem.l	d0/d3,-(sp)
	bsr	pkt3
	movem.l	(sp)+,d0/d3
	bra.s	1b
2:	rts

| d0 within -d4..d4
clip:	cmp.l	d4,d0
	ble.s	1f
	move.l	d4,d0
1:	neg.l	d4
	cmp.l	d4,d0
	bge.s	2f
	move.l	d4,d0
2:	neg.l	d4
	rts

| a relative packet to mousevec: buttons d4, dx d1, dy d2
pkt3:	lea	pkt,a0
	or.b	#0xf8,d4
	move.b	d4,(a0)
	move.b	d1,1(a0)
	move.b	d2,2(a0)
	movem.l	d0-d7/a2-a6,-(sp)
	move.l	a0,-(sp)
	move.l	16(a4),a1		| mousevec
	jsr	(a1)
	addq.l	#4,sp
	movem.l	(sp)+,d0-d7/a2-a6
	addq.l	#1,pv_npkt(a5)
	rts

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
