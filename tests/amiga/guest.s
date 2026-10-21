| guest.s -- t_amiga's synthetic guest, loaded at 0x1000 in chip RAM.
|
| A table of entry points heads the image; the C side calls them in
| guest mode with the AMIX calling convention.  Vectors use VBR 0.
|   getsr()             SR the guest sees
|   setsr(sr)           with S kept set
|   wr16(addr, v)
|   rd16(addr)
|   urte(usp)           0, or the failing check (below)
|   stopit()            STOP #$2000, then IPL 7
|   vblon()             VERTB handler at level 3
|   &vblcount
|   hexit(code)         exit through a host call, as the session's Log Out
|   hhalt(fd)           AMIGAIOC_HALT through a host call, as Shut Down: errno
|   zdiv()              a divide by zero here; zero divides seen by the guest
|   m16(dst, src, n)    n 16-byte lines with MOVE16

	.text
	.globl	start
start:
	.long	getsr, setsr, wr16, rd16, urte, stopit, vblon, vblcount, dis, trp
	.long	hexit, hhalt, zdiv, m16

getsr:
	moveq	#0,d0
	move.w	sr,d0
	rts

setsr:
	move.w	6(sp),d0
	move.w	d0,sr
	rts

wr16:
	move.l	4(sp),a0
	move.w	10(sp),d0
	move.w	d0,(a0)
	rts

rd16:
	move.l	4(sp),a0
	moveq	#0,d0
	move.w	(a0),d0
	rts

| RTE into user mode on the given stack; there a privileged instruction
| reflects to vector 8, whose handler resumes after it; trap #1 returns
| to supervisor.  Codes: 1 user SP, 2 privilege frame PC, 3 privilege
| frame format, 4 trap SSP, 5 trap frame SR has S, 6 trap frame format,
| 7 SR after the trap.
urte:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	move.l	a0,usp
	move.l	a0,d7
	move.l	sp,savessp
	move.l	#uprv,0x20
	move.l	#utrap,0x84
	moveq	#0,d6
	clr.w	-(sp)			| format 0
	pea	ucode
	clr.w	-(sp)			| user mode, IPL 0
	rte
ucode:
	cmp.l	sp,d7
	beq.s	1f
	moveq	#1,d6
1:
pinsn:	move.w	#0x2700,sr		| privileged in user mode
	trap	#1
uprv:
	cmp.l	#pinsn,2(sp)
	beq.s	1f
	moveq	#2,d6
1:	cmp.w	#0x0020,6(sp)
	beq.s	1f
	moveq	#3,d6
1:	addq.l	#4,2(sp)
	rte
utrap:
	move.l	savessp,d0
	subq.l	#8,d0
	cmp.l	sp,d0
	beq.s	1f
	moveq	#4,d6
1:	btst	#5,(sp)
	beq.s	1f
	moveq	#5,d6
1:	cmp.w	#0x0084,6(sp)
	beq.s	1f
	moveq	#6,d6
1:	move.w	sr,d0
	btst	#13,d0
	bne.s	1f
	moveq	#7,d6
1:	move.l	savessp,sp
	move.w	#0x2700,sr
	move.l	d6,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts

| dis(n): n Disable/Enable pairs on the master word; trp(n): n trap #2 round trips
dis:
	move.l	4(sp),d0
1:	move.w	#0x4000,0x20001f00
	move.w	#0xc000,0x20001f00
	subq.l	#1,d0
	bne.s	1b
	rts

trp:
	move.l	#trpret,0x88
	move.l	4(sp),d0
1:	trap	#2
	subq.l	#1,d0
	bne.s	1b
	rts
trpret:
	rte

hexit:
	move.l	4(sp),-(sp)
	clr.l	-(sp)
	moveq	#1,d0
	trap	#0
	addq.l	#8,sp
	rts

hhalt:
	clr.l	-(sp)
	move.l	#0x4108,-(sp)
	move.l	12(sp),-(sp)
	clr.l	-(sp)
1:	moveq	#54,d0
	trap	#0
	bcc.s	2f
	cmp.l	#4,d0			| EINTR: an interrupt's signal
	beq.s	1b
	bra.s	3f
2:	moveq	#0,d0
3:	lea	16(sp),sp
	rts

| the guest's zero divide handler counts; the frame's PC is past divu
zdiv:
	move.l	#zhit,0x14
	moveq	#0,d0
	moveq	#1,d1
	divu.w	d0,d1
	move.l	zcount,d0
	rts
zhit:
	addq.l	#1,zcount
	rte

m16:
	move.l	4(sp),a1
	move.l	8(sp),a0
	move.l	12(sp),d0
1:	move16	(a0)+,(a1)+
	subq.l	#1,d0
	bne.s	1b
	rts

stopit:
	stop	#0x2000
	move.w	#0x2700,sr
	rts

vblon:
	move.l	#vbl,0x6c
	rts

| level 3 autovector: count, acknowledge VERTB in the INTREQ word
vbl:
	addq.l	#1,vblcount
	andi.w	#0xffdf,0x20001f08
	rte

	.data
vblcount: .long	0
savessp: .long	0
zcount:	.long	0
