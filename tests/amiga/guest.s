| guest.s -- t_amiga's synthetic guest, loaded at 0x1000 in chip RAM.
|
| A table of entry points heads the image; the C side calls them in
| guest mode with the AMIX calling convention.  Vectors use VBR 0.
|   getsr()             SR the guest sees
|   setsr(sr)           with S kept set
|   wr16(addr, v)       custom/CIA register writes through the fault path
|   rd16(addr)
|   urte(usp)           0, or the failing check (below)
|   stopit()            STOP #$2000, then IPL 7
|   vblon()             VERTB handler at level 3
|   &vblcount

	.text
	.globl	start
start:
	.long	getsr, setsr, wr16, rd16, urte, stopit, vblon, vblcount, dis, trp

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

| dis(n): n Disable/Enable pairs; trp(n): n trap #2 round trips
dis:
	move.l	4(sp),d0
1:	move.w	#0x4000,0xdff09a
	move.w	#0xc000,0xdff09a
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

stopit:
	stop	#0x2000
	move.w	#0x2700,sr
	rts

vblon:
	move.l	#vbl,0x6c
	rts

| level 3 autovector: count, acknowledge VERTB
vbl:
	addq.l	#1,vblcount
	move.l	a0,-(sp)
	lea	0xdff000,a0
	move.w	#0x0020,0x9c(a0)
	move.l	(sp)+,a0
	rte

	.data
vblcount: .long	0
savessp: .long	0
