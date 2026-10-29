| Boot progress kept in chip RAM until the MMU and memory map are up, so
| it can be read back after a reset.  Two copies, at 0x1000 and 0x1FF000:
|
|	+0	'AMXB'
|	+4	marks so far
|	+8	last mark
|	+12	the mark before it
|	+16	the last mark's first argument (an address or size)
|	+20	last mem[] index seen
|	+24	its lower bound
|
| Writes need no registers, so the wrappers below keep every register.
| Marking stops once pstart returns.

	.set	MK0,0x1000
	.set	MK1,0x1FF000

	.data
	.globl	bootmk_on
bootmk_on:
	.byte	1
	.even

	.text
	.globl	bootmark
| bootmark(code, arg)
bootmark:
	tstb	bootmk_on
	beqs	1f
	addql	&1,MK0+4
	movel	MK0+8,MK0+12
	movel	%sp@(4),MK0+8
	movel	%sp@(8),MK0+16
	addql	&1,MK1+4
	movel	MK1+8,MK1+12
	movel	%sp@(4),MK1+8
	movel	%sp@(8),MK1+16
1:	rts

| mark1: called as pea code; jsr mark1 from a function's entry: records
| code and the function's first argument
mark1:
	tstb	bootmk_on
	beqs	1f
	addql	&1,MK0+4
	movel	MK0+8,MK0+12
	movel	%sp@(4),MK0+8
	movel	%sp@(12),MK0+16
	addql	&1,MK1+4
	movel	MK1+8,MK1+12
	movel	%sp@(4),MK1+8
	movel	%sp@(12),MK1+16
1:	rts

	.macro	MARK code
	pea	\code
	jsr	mark1
	lea	%sp@(4),%sp
	.endm

	.macro	WRAP name code
	.globl	\name
\name:
	MARK	\code
	jmp	__amix_\name
	.endm

	WRAP	sysseginit 0x11
	WRAP	mfree 0x12
	WRAP	sptalloc 0x13
	WRAP	hat_init 0x14
	WRAP	page_init 0x15
	WRAP	memialloc 0x16
	WRAP	kmem_init 0x17
	WRAP	segmap_create 0x19
	WRAP	segu_create 0x1a
	WRAP	dispinit 0x1b
	WRAP	hrtinit 0x1c
	WRAP	itinit 0x1d
	WRAP	pid_init 0x1e
	WRAP	mlsetup 0x21
	WRAP	figuredisplaytype 0x22

| seg_alloc(as, base, size): segmap ends where the u-areas start
	.globl	seg_alloc
seg_alloc:
	MARK	0x18
	cmpil	&kvsegmap,%sp@(8)
	bnes	1f
	movel	&kvsegu,%d0
	subl	&kvsegmap,%d0
	cmpl	%sp@(12),%d0
	bccs	1f
	movel	%d0,%sp@(12)
1:	jmp	__amix_seg_alloc

| svirtophys(va): the bus address
	.globl	svirtophys
svirtophys:
	MARK	0x1f
	movel	%sp@(4),%sp@-
	jsr	__amix_svirtophys
	movel	%d0,%sp@
	jsr	hr_tobus
	addql	&4,%sp
	moveal	%d0,%a0
	rts

| vstart: RAM above the kernel's region is mapped first
	.globl	vstart
vstart:
	MARK	0x20
	moveml	%d0-%d1/%a0-%a1,%sp@-
	jsr	amiga_hrmap
	moveml	%sp@+,%d0-%d1/%a0-%a1
	jmp	__amix_vstart

| the pstart trace letters ('A' entry, 'B' tables built, 'C' MMU on,
| 'D' caches on, 'E'-'H' vstart, mlsetup, svirtophys)
	.globl	btrace_mark
btrace_mark:
	tstb	bootmk_on
	beqs	1f
	addql	&1,MK0+4
	movel	MK0+8,MK0+12
	movel	%sp@(4),MK0+8
	addql	&1,MK1+4
	movel	MK1+8,MK1+12
	movel	%sp@(4),MK1+8
1:	jmp	__amix_btrace_mark

| bzero, mark 0x30, in 64 KB steps while marking: +16 follows the steps
	.globl	bzero
bzero:
	tstb	bootmk_on
	bnes	5f
	jmp	__amix_bzero
5:	MARK	0x30
	moveml	%d0-%d1/%a0-%a1,%sp@-
	movel	%sp@(20),%a0
	movel	%sp@(24),%d1
2:	tstl	%d1
	bles	4f
	movel	&0x10000,%d0
	cmpl	%d0,%d1
	bccs	3f
	movel	%d1,%d0
3:	movel	%a0,MK0+16
	movel	%a0,MK1+16
	moveml	%d0-%d1/%a0,%sp@-
	movel	%d0,%sp@-
	movel	%a0,%sp@-
	jsr	__amix_bzero
	addql	&8,%sp
	moveml	%sp@+,%d0-%d1/%a0
	addl	%d0,%a0
	subl	%d0,%d1
	bras	2b
4:	moveml	%sp@+,%d0-%d1/%a0-%a1
	rts
