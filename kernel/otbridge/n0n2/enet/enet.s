| enet.s -- .ENET driver for a virtual Ethernet port: frames go to and
| from the host through a /dev/otbstation station (one frame per Unix
| read or write).  Classic .ENET interface: attach/detach protocol
| handlers, write, multicast, get info; handlers receive through
| ReadPacket/ReadRest from a VBL task that drains the station.
|
| Unix calls are trap #0 with the number in d0 and the arguments on the
| stack; carry set means d0 holds the errno.  Toolbox traps and IODone
| go through the glue table after the 'AUXG' mark, one 8-byte slot each,
| so a test harness can point them at its own code.

	.text
hdr:
	.short	0x4400			| dNeedLock, dCtlEnable
	.short	0, 0, 0
	.short	dopen - hdr
	.short	dprime - hdr
	.short	dctl - hdr
	.short	dstat - hdr
	.short	dclose - hdr
	.byte	5
	.ascii	".ENET"
	.even

| glue: _NewPtr,SYS,CLEAR  _DisposePtr  _VInstall  _VRemove  IODone
	.long	0x41555847		| 'AUXG'
	.short	5
g_newptr:
	.short	0xa71e
	rts
	.short	0, 0
g_disp:
	.short	0xa01f
	rts
	.short	0, 0
g_vinst:
	.short	0xa033
	rts
	.short	0, 0
g_vrem:
	.short	0xa034
	rts
	.short	0, 0
g_iodone:
	move.l	0x8fc.w,-(%sp)		| JIODone
	rts
	.short	0

| driver globals (G)
G_FD	= 0
G_MAC	= 4
G_BUSY	= 10
G_NPH	= 12
G_PH	= 14			| 8 x { type.w, pad.w, handler.l }
NPH	= 8
G_VBL	= 80			| VBL task, 14 bytes
G_HDR	= 96			| 14-byte header for the handler
G_RXLEN	= 112
G_NRX	= 116
G_NTX	= 120
G_NDROP	= 124
G_RX	= 128
G_TX	= 1664
G_ARG	= 3200			| ioctl argument, 8 bytes
G_SIZE	= 3216
BUFSZ	= 1536
MAXFRAME = 1514

| errors
controlErr	= -17
statusErr	= -18
readErr		= -19
openErr		= -23
eMultiErr	= -91
eLenErr		= -92
lapProtErr	= -94

OTB_STATION	= 0x6f20
OTB_STMULTI	= 0x6f22

| ---- Unix calls: args already on the stack; d0 = result or -errno ----

sys_read:
	moveq	#3,%d0
	bra.s	sys
sys_write:
	moveq	#4,%d0
	bra.s	sys
sys_open:
	moveq	#5,%d0
	bra.s	sys
sys_close:
	moveq	#6,%d0
	bra.s	sys
sys_ioctl:
	moveq	#54,%d0
sys:
	trap	#0			| the caller's args are at 4(%sp)
	bcc.s	1f
	neg.l	%d0
1:	rts

path:	.asciz	"/dev/otbstation"
	.even

| ---- Open: a0 = pb, a1 = DCE ----
dopen:
	movem.l	%d3-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a4
	tst.l	20(%a4)			| dCtlStorage: already open
	bne.w	open_ok
	move.l	#G_SIZE,%d0
	bsr	g_newptr
	tst.w	%d0
	bne.w	open_out
	move.l	%a0,%a3
	pea	6			| O_RDWR | O_NDELAY
	pea	path(%pc)
	bsr	sys_open
	addq.l	#8,%sp
	tst.l	%d0
	bmi.s	open_free
	move.l	%d0,G_FD(%a3)
	lea	G_ARG(%a3),%a0		| derived address, bridge mode
	clr.l	(%a0)
	clr.w	4(%a0)
	move.w	#1,6(%a0)
	pea	(%a0)
	pea	OTB_STATION
	move.l	G_FD(%a3),-(%sp)
	bsr	sys_ioctl
	lea	12(%sp),%sp
	tst.l	%d0
	bmi.s	open_close
	move.l	G_ARG(%a3),G_MAC(%a3)
	move.w	G_ARG+4(%a3),G_MAC+4(%a3)
	lea	G_VBL(%a3),%a0
	move.w	#1,4(%a0)		| vType
	lea	vbl(%pc),%a1
	move.l	%a1,6(%a0)
	move.w	#1,10(%a0)
	clr.w	12(%a0)
	bsr	g_vinst
	tst.w	%d0
	bne.s	open_close
	move.l	%a3,20(%a4)
open_ok:
	moveq	#0,%d0
open_out:
	movem.l	(%sp)+,%d3-%d7/%a0-%a4
	rts
open_close:
	move.l	G_FD(%a3),-(%sp)
	bsr	sys_close
	addq.l	#4,%sp
open_free:
	move.l	%a3,%a0
	bsr	g_disp
	moveq	#openErr,%d0
	bra.s	open_out

| ---- Close ----
dclose:
	movem.l	%d3-%d7/%a0-%a4,-(%sp)
	move.l	%a1,%a4
	move.l	20(%a4),%d0
	beq.s	close_out
	move.l	%d0,%a3
	lea	G_VBL(%a3),%a0
	bsr	g_vrem
	move.l	G_FD(%a3),-(%sp)
	bsr	sys_close
	addq.l	#4,%sp
	move.l	%a3,%a0
	bsr	g_disp
	clr.l	20(%a4)
close_out:
	moveq	#0,%d0
	movem.l	(%sp)+,%d3-%d7/%a0-%a4
	rts

dprime:
	moveq	#readErr,%d0
	bra.s	finish_imm
dstat:
	moveq	#statusErr,%d0
	bra.s	finish_imm

| ---- Control: csCode at 26(a0), parameters from 28(a0) ----
dctl:
	movem.l	%d3-%d7/%a0-%a4,-(%sp)
	move.l	20(%a1),%d0
	beq.s	ctl_bad
	move.l	%d0,%a3
	move.l	%a0,%a4
	move.w	26(%a4),%d1
	cmp.w	#246,%d1
	beq.w	ewrite
	cmp.w	#248,%d1
	beq.w	eattach
	cmp.w	#249,%d1
	beq.w	edetach
	cmp.w	#252,%d1
	beq.w	egetinfo
	cmp.w	#253,%d1
	beq.s	ctl_ok			| ESetGeneral: large frames are the default
	cmp.w	#245,%d1
	beq.w	eaddmulti
	cmp.w	#247,%d1
	beq.w	edelmulti
ctl_bad:
	moveq	#controlErr,%d0
	bra.s	ctl_done
ctl_ok:
	moveq	#0,%d0
ctl_done:
	movem.l	(%sp)+,%d3-%d7/%a0-%a4
| immediate calls return; queued ones complete through IODone
finish_imm:
	move.w	%d0,16(%a0)		| ioResult
	btst	#1,6(%a0)		| ioTrap noQueueBit (bit 9)
	bne.s	1f
	bra	g_iodone
1:	tst.w	%d0
	rts

| EWrite: 30(a4) = write data structure { length.w, pointer.l }..., 0
ewrite:
	move.l	30(%a4),%a0
	lea	G_TX(%a3),%a1
	moveq	#0,%d2			| total
1:	moveq	#0,%d0
	move.w	(%a0)+,%d0
	beq.s	3f
	move.l	(%a0)+,%a2
	add.l	%d0,%d2
	cmp.l	#MAXFRAME,%d2
	bhi.s	elen
	bra.s	2f
4:	move.b	(%a2)+,(%a1)+
2:	dbra	%d0,4b
	bra.s	1b
3:	cmp.l	#14,%d2
	blt.s	elen
	lea	G_TX(%a3),%a0
	move.l	G_MAC(%a3),6(%a0)	| our source address
	move.w	G_MAC+4(%a3),10(%a0)
	move.l	%d2,-(%sp)
	pea	(%a0)
	move.l	G_FD(%a3),-(%sp)
	bsr	sys_write
	lea	12(%sp),%sp
	tst.l	%d0
	bmi.s	1f
	addq.l	#1,G_NTX(%a3)
	bra.w	ctl_ok
1:	addq.l	#1,G_NDROP(%a3)		| lost, as on a busy wire
	bra.w	ctl_ok
elen:
	moveq	#eLenErr,%d0
	bra.s	ctl_done

| EAttachPH: 28(a4) protocol type (0: 802.3 frames), 30(a4) handler
eattach:
	move.w	28(%a4),%d1
	bsr	findph
	bpl.s	1f			| already attached
	move.w	G_NPH(%a3),%d0
	cmp.w	#NPH,%d0
	bge.s	1f
	lsl.w	#3,%d0
	lea	G_PH(%a3,%d0.w),%a0
	move.w	%d1,(%a0)
	move.l	30(%a4),4(%a0)
	addq.w	#1,G_NPH(%a3)
	bra.w	ctl_ok
1:	moveq	#lapProtErr,%d0
	bra.w	ctl_done

| EDetachPH: 28(a4) protocol type
edetach:
	move.w	28(%a4),%d1
	bsr	findph
	bmi.s	3f
	subq.w	#1,G_NPH(%a3)
	lsl.w	#3,%d0
	lea	G_PH(%a3,%d0.w),%a0
	move.w	G_NPH(%a3),%d1
	lsl.w	#3,%d1
	lea	G_PH(%a3,%d1.w),%a1	| last entry into the hole
	move.l	(%a1),(%a0)
	move.l	4(%a1),4(%a0)
	bra.w	ctl_ok
3:	moveq	#lapProtErr,%d0
	bra.w	ctl_done

| d1 = type; d0 = index or -1 (flags from d0)
findph:
	moveq	#0,%d0
	lea	G_PH(%a3),%a0
	bra.s	2f
1:	cmp.w	(%a0),%d1
	beq.s	3f
	addq.l	#8,%a0
	addq.w	#1,%d0
2:	cmp.w	G_NPH(%a3),%d0
	blt.s	1b
	moveq	#-1,%d0
3:	tst.w	%d0
	rts

| EGetInfo: 30(a4) buffer, 34(a4) size: address, then zero counters
egetinfo:
	move.l	30(%a4),%a0
	move.w	34(%a4),%d0
	cmp.w	#78,%d0
	bls.s	1f
	moveq	#78,%d0
1:	lea	G_MAC(%a3),%a1
	moveq	#0,%d1
	bra.s	3f
2:	cmp.w	#6,%d1
	bcc.s	4f
	move.b	(%a1)+,(%a0)+
	bra.s	5f
4:	clr.b	(%a0)+
5:	addq.w	#1,%d1
3:	cmp.w	%d0,%d1
	blt.s	2b
	bra.w	ctl_ok

eaddmulti:
	moveq	#1,%d2
	bra.s	multi
edelmulti:
	moveq	#0,%d2
multi:
	lea	G_ARG(%a3),%a0
	move.l	28(%a4),(%a0)
	move.w	32(%a4),4(%a0)
	move.w	%d2,6(%a0)
	pea	(%a0)
	pea	OTB_STMULTI
	move.l	G_FD(%a3),-(%sp)
	bsr	sys_ioctl
	lea	12(%sp),%sp
	tst.l	%d0
	bpl.w	ctl_ok
	moveq	#eMultiErr,%d0
	bra.w	ctl_done

| ---- receive: VBL task (a0 = task) drains the station each tick ----
vbl:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	lea	-G_VBL(%a0),%a3
	move.w	#1,10(%a0)
	tas	G_BUSY(%a3)
	bne.s	9f
	moveq	#31,%d7			| frames per tick at most
1:	pea	BUFSZ
	pea	G_RX(%a3)
	move.l	G_FD(%a3),-(%sp)
	bsr	sys_read
	lea	12(%sp),%sp
	tst.l	%d0
	ble.s	8f
	cmp.l	#14,%d0
	blt.s	2f
	move.l	%d0,G_RXLEN(%a3)
	addq.l	#1,G_NRX(%a3)
	bsr	deliver
2:	dbra	%d7,1b
8:	clr.b	G_BUSY(%a3)
9:	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	rts

| Hand the frame in G_RX to its handler: a3 just past a copy of the
| header, d1 bytes left, a4 ReadPacket (ReadRest at 2(a4)), a0/a1 ours.
deliver:
	move.w	G_RX+12(%a3),%d1
	cmp.w	#1500,%d1
	bhi.s	1f
	moveq	#0,%d1			| 802.3: length, not type
1:	bsr	findph
	bmi.s	9f
	lsl.w	#3,%d0
	move.l	G_PH+4(%a3,%d0.w),%d0
	beq.s	9f
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	move.l	%d0,%a2
	lea	G_RX(%a3),%a0
	lea	G_HDR(%a3),%a1
	moveq	#13,%d0
2:	move.b	(%a0)+,(%a1)+
	dbra	%d0,2b
	move.l	%a1,%a4			| header end
	move.l	G_RXLEN(%a3),%d1
	sub.l	#14,%d1
	move.w	G_RX+12(%a3),%d0
	move.l	%a3,%a1
	move.l	%a4,%a3
	lea	rpacket(%pc),%a4
	jsr	(%a2)
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
9:	rts

| ReadPacket: a3 buffer, d3.w count; d1 left, a0 our pointer.
| Out: d3 = 0 and Z, or d0 = eLenErr (d1 < d3).  d0 kept on success.
| ReadRest (2 bytes on): a3 buffer, d3.w size.  Out: d3 = size - left,
| d0 = 0 and Z.  Both keep d2, a1, a2 and advance a3.
rpacket:
	bra.s	rpk
rrest:
	move.w	%d3,%d0
	sub.w	%d1,%d0
	cmp.w	%d1,%d3
	bls.s	1f
	move.w	%d1,%d3
1:	sub.w	%d3,%d1
	bra.s	3f
2:	move.b	(%a0)+,(%a3)+
3:	dbra	%d3,2b
	move.w	%d0,%d3
	moveq	#0,%d0
	rts
rpk:
	cmp.w	%d3,%d1
	bcs.s	9f
	sub.w	%d3,%d1
	bra.s	3f
2:	move.b	(%a0)+,(%a3)+
3:	dbra	%d3,2b
	moveq	#0,%d3
	rts
9:	moveq	#eLenErr,%d0
	rts
