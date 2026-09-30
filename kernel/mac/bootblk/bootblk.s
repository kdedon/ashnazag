| bootblk.s -- HFS boot blocks that load and start the Mac kernel.
|
| The ROM reads the first 1024 bytes of the boot partition into a buffer
| on its stack and, because bbVersion's high byte is 0x44, calls bbEntry
| with jsr.  The volume is not mounted yet; the boot drive's driver is
| open.  We read the kernel, a flat image stored contiguously in the
| partition (offset, length and checksum patched in by mkbb), with one
| _Read, build a bootinfo record, then leave through a trampoline
| at the top of free RAM:
|
|   0     SysHeap   stack  code(a6)   free    image   tramp+record  BufPtr
|   |------|--------|<--|----|--------------|=======|====|--------|-------|
|           kernel copied to kload, BSS zeroed, record at `end'
|
| The trampoline turns caches, translation and TT registers off, copies
| the image to its link address, zeroes BSS, puts the record at the first
| even address past `end' and enters the kernel with d0 = 0.  RAM, the
| image and the trampoline must be mapped VA = PA (true on djMEMC Macs).
| Errors call _SysError with the ids below and stop.
|
| Assemble with m68k-elf-as -m68040; the result is position independent.

BI_MAX	= 512			| boot record space above the trampoline
CMDMAX	= 128			| command line, NUL included

| low memory
BufPtr	= 0x10C
MemTop	= 0x108
CPUFlag	= 0x12F
VIA	= 0x1D4
SCCRd	= 0x1D8
BootDrive = 0x210
ROMBase	= 0x2AE
ScrnBase = 0x824
ScreenRow = 0x106
MainDevice = 0x8A4
BtDskRfn = 0xB34
HWCfgFlags = 0xB22
BoxFlag	= 0xCB3
ChunkyDepth = 0xD60
MMU32Bit = 0xCB2

| _SysError ids
E_ROOM	= 101			| image does not fit between code and BufPtr
E_END	= 102			| kernel footprint reaches the trampoline
E_READ	= 103			| _Read failed or came short
E_SUM	= 104			| checksum mismatch
E_CPU	= 105			| not a 68040

_Read	= 0xA002
_Gestalt = 0xA1AD
_SysError = 0xA9C9
_Translate24To32 = 0xA091

	.text
bb:
	.word	0x4C4B			| bbID 'LK'
	bra.w	start			| bbEntry
	.word	0x4418			| bbVersion: execute the code
| filled in by mkbb
P_MAGIC	= 0x8C			| "UxBB"
P_KOFS	= 0x90			| image offset in the partition (bytes)
P_KLEN	= 0x94			| image length, a multiple of 512
P_KLOAD	= 0x98			| link address of the image
P_KEND	= 0x9C			| `end': last BSS byte + 1
P_KENTRY = 0xA0			| entry
P_KSUM	= 0xA4			| sum of the image's longs
	.org	bb+P_MAGIC
	.ascii	"UxBB"
	.long	0, 0, 0, 0, 0, 0

| Copied to the top of RAM and run from there, IPL 7.
| a0 image, a1 kload, d0 length, d1 end, a2 record, d2 its length, a3 entry
	.balign	4
tramp:
	moveq	#0,%d7
	movec	%d7,%cacr
	movec	%d7,%tc
	movec	%d7,%itt0
	movec	%d7,%itt1
	movec	%d7,%dtt0
	movec	%d7,%dtt1
	pflusha
1:	move.l	(%a0)+,(%a1)+
	subq.l	#4,%d0
	bhi.s	1b
2:	cmp.l	%d1,%a1			| BSS; stops at most 3 bytes past end
	bcc.s	3f
	clr.l	(%a1)+
	bra.s	2b
3:	addq.l	#1,%d1
	and.w	#0xFFFE,%d1
	move.l	%d1,%a1
4:	move.l	(%a2)+,(%a1)+
	subq.l	#4,%d2
	bhi.s	4b
	moveq	#0,%d0
	sub.l	%a0,%a0
	jmp	(%a3)
	.balign	4
tramp_end:
TSIZE	= tramp_end-tramp

start:
	lea	bb(%pc),%a4

| T (d5) = trampoline below BufPtr, image (d4) below it
	move.l	BufPtr,%d5
	sub.l	#TSIZE+BI_MAX,%d5
	and.w	#0xFFF0,%d5
	move.l	%d5,%d4
	sub.l	P_KLEN(%a4),%d4
	and.w	#0xFFF0,%d4
	lea	0x1000(%a4),%a0		| clear of our code and the ROM's frames
	moveq	#E_ROOM,%d0
	cmp.l	%a0,%d4
	bcs.w	err
	move.l	P_KEND(%a4),%d1
	add.l	#BI_MAX+4,%d1
	moveq	#E_END,%d0
	cmp.l	%d5,%d1
	bhi.w	err

| read the image through the boot drive's driver
	lea	-64(%sp),%sp
	move.l	%sp,%a0
	moveq	#15,%d0
5:	clr.l	(%a0)+
	dbf	%d0,5b
	move.l	%sp,%a0
	move.w	BootDrive,22(%a0)	| ioVRefNum
	move.w	BtDskRfn,24(%a0)	| ioRefNum
	move.l	%d4,32(%a0)		| ioBuffer
	move.l	P_KLEN(%a4),36(%a0)	| ioReqCount
	move.w	#1,44(%a0)		| ioPosMode: fsFromStart
	move.l	P_KOFS(%a4),46(%a0)	| ioPosOffset
	.word	_Read
	move.l	40(%a0),%d1		| ioActCount
	lea	64(%sp),%sp
	tst.w	%d0
	bne.s	6f
	cmp.l	P_KLEN(%a4),%d1
	beq.s	7f
6:	moveq	#E_READ,%d0
	bra.w	err
7:	move.l	%d4,%a0
	moveq	#0,%d2
8:	add.l	(%a0)+,%d2
	subq.l	#4,%d1
	bhi.s	8b
	moveq	#E_SUM,%d0
	cmp.l	P_KSUM(%a4),%d2
	bne.w	err

| boot record at T + TSIZE
	move.l	%d5,%a1
	lea	TSIZE(%a1),%a1
	move.l	#0x00010008,(%a1)+	| BI_MACHTYPE: MACH_MAC
	move.l	#3,(%a1)+
	moveq	#E_CPU,%d0
	cmp.b	#4,CPUFlag
	bne.w	err
	moveq	#4,%d0
	move.l	#0x00020008,(%a1)+	| BI_CPUTYPE: 68040
	move.l	%d0,(%a1)+
	move.l	#0x00040008,(%a1)+	| BI_MMUTYPE: 68040
	move.l	%d0,(%a1)+
	btst	#4,HWCfgFlags		| bit 12: FPU present
	beq.s	1f
	move.l	#0x00030008,(%a1)+	| BI_FPUTYPE: 68040
	move.l	%d0,(%a1)+
1:	move.l	#0x72616D20,%d0		| Gestalt 'ram ', else MemTop
	.word	_Gestalt
	tst.w	%d0
	beq.s	2f
	move.l	MemTop,%a0
2:	move.l	#0x0005000C,(%a1)+	| BI_MEMCHUNK: 0, size
	clr.l	(%a1)+
	move.l	%a0,(%a1)+
	move.l	#0x80090008,(%a1)+	| BI_MAC_MEMSIZE: MB
	move.l	%a0,%d0
	moveq	#20,%d1
	lsr.l	%d1,%d0
	move.l	%d0,(%a1)+
	move.l	#0x6D616368,%d0		| Gestalt 'mach', else BoxFlag + 6
	.word	_Gestalt
	tst.w	%d0
	beq.s	3f
	moveq	#0,%d0
	move.b	BoxFlag,%d0
	addq.l	#6,%d0
	move.l	%d0,%a0
3:	move.l	#0x80000008,(%a1)+	| BI_MAC_MODEL
	move.l	%a0,(%a1)+
	move.l	#0x800B0008,(%a1)+	| BI_MAC_ROMBASE
	move.l	ROMBase,(%a1)+
	move.l	#0x80100008,(%a1)+	| BI_MAC_VIA1BASE
	move.l	VIA,(%a1)+
	move.l	#0x80060008,(%a1)+	| BI_MAC_SCCBASE
	move.l	SCCRd,(%a1)+

| video: the main GDevice's PixMap, else ScrnBase/ScreenRow/ChunkyDepth
	move.l	MainDevice,%d0
	beq.s	5f
	move.l	%d0,%a0
	move.l	(%a0),%d0
	beq.s	5f
	move.l	%d0,%a0
	move.l	22(%a0),%d0		| gdPMap
	beq.s	5f
	move.l	%d0,%a0
	move.l	(%a0),%d0
	beq.s	5f
	move.l	%d0,%a0
	move.l	(%a0),%d0		| baseAddr
	beq.s	5f
	bsr.w	vaddr
	move.w	4(%a0),%d1		| rowBytes
	move.w	32(%a0),%d2		| pixelSize
	move.l	#0x80040008,(%a1)+	| BI_MAC_VDIM: height << 16 | width
	move.w	10(%a0),%d0
	sub.w	6(%a0),%d0
	swap	%d0
	move.w	12(%a0),%d0
	sub.w	8(%a0),%d0
	move.l	%d0,(%a1)+
	bra.s	6f
5:	move.l	ScrnBase,%d0
	beq.s	7f
	bsr.w	vaddr
	move.w	ScreenRow,%d1
	move.w	ChunkyDepth,%d2
6:	moveq	#0,%d0
	move.w	%d1,%d0
	and.w	#0x3FFF,%d0
	move.l	#0x80030008,(%a1)+	| BI_MAC_VROW
	move.l	%d0,(%a1)+
	moveq	#0,%d0
	move.w	%d2,%d0
	beq.s	7f
	move.l	#0x80020008,(%a1)+	| BI_MAC_VDEPTH
	move.l	%d0,(%a1)+
7:

| command line, padded to a long
	lea	cmdline(%pc),%a0
	tst.b	(%a0)
	beq.s	2f
	move.l	%a1,%a2
	move.l	#0x00070000,(%a1)+	| BI_COMMAND_LINE
1:	move.b	(%a0)+,(%a1)+
	bne.s	1b
	move.l	%a1,%d0
	addq.l	#3,%d0
	and.w	#0xFFFC,%d0
	move.l	%d0,%a1
	sub.l	%a2,%d0
	move.w	%d0,2(%a2)
2:	clr.l	(%a1)+			| BI_LAST

| trampoline to T, then go
	lea	tramp(%pc),%a0
	move.l	%d5,%a2
	moveq	#TSIZE/4-1,%d0
3:	move.l	(%a0)+,(%a2)+
	dbf	%d0,3b
	move.l	%d5,%a2
	lea	TSIZE(%a2),%a2
	move.l	%a1,%d2
	sub.l	%a2,%d2
	move.l	%d4,%a0
	move.l	P_KLOAD(%a4),%a1
	move.l	P_KLEN(%a4),%d0
	move.l	P_KEND(%a4),%d1
	move.l	P_KENTRY(%a4),%a3
	move.w	#0x2700,%sr
	cpusha	%bc
	move.l	%d5,%a4
	jmp	(%a4)

| BI_MAC_VADDR: d0 if it is video space, else ScrnBase.  In 24-bit mode
| the value is translated only when it is not already a 32-bit video
| address (the PixMap can hold 0xF9001000 there too, which would
| translate to 0x1000).
vaddr:
	move.l	#0x80010008,(%a1)+
	bsr.s	1f
	beq.s	2f
	move.l	ScrnBase,%d0
	bsr.s	1f
2:	move.l	%d0,(%a1)+
	rts
1:	bsr.s	4f			| Z: d0 (maybe translated) is video
	beq.s	3f
	tst.b	MMU32Bit
	bne.s	3f
	move.l	%d0,-(%sp)
	.word	_Translate24To32	| d0 in and out
	bsr.s	4f
	beq.s	5f
	move.l	(%sp)+,%d0
	andi.b	#0xFB,%ccr
	rts
5:	addq.l	#4,%sp
3:	rts
4:	move.l	%d0,%d3			| built-in video or NuBus slot space?
	sub.l	#0xF9000000,%d3
	cmp.l	#0x00800000,%d3
	bcs.s	6f
	sub.l	#0x01000000,%d3
	cmp.l	#0x05000000,%d3
	bcs.s	6f
	andi.b	#0xFB,%ccr
	rts
6:	ori.b	#4,%ccr
	rts

err:
	.word	_SysError
9:	bra.s	9b

code_end:
	.org	bb+0x400-CMDMAX
cmdline:
	.space	CMDMAX
