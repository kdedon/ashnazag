| tosml.s -- the container's machine layer: an application cartridge
| at $FA0000 whose init runs before the disk boot.  It serves drive C:
| from a disk image through the BIOS hard-disk vectors, reading and
| writing the image with host system calls (trap #0).
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

	.org	0x80
init:
	tst.l	p_fd
	bmi.s	1f
	move.l	0x472,old_bpb		| hdv_bpb
	move.l	#bpb,0x472
	move.l	0x476,old_rw		| hdv_rw
	move.l	#rw,0x476
	move.l	0x47e,old_mc		| hdv_mediach
	move.l	#mc,0x47e
	or.l	#4,0x4c2		| _drvbits: C:
	move.w	#2,0x446		| _bootdev
1:	rts
run:	rts

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
