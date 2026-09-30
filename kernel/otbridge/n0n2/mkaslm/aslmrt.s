| aslmrt.s -- library runtime for mkaslm: the entry point the Shared
| Library Manager calls, the export callbacks and Main's first function.
| Link it first; __aslm_blk must end .a5init (mkaslm appends the data
| block there).
|
| Entry: long entry(long sel, long a, long b), C convention.
|   0  a = A5: copy the initial image below A5, zero the rest, add A5
|      to the pointers the block lists
|   1  a = the manager's context for this library
|   2  clears the context's word at 12; returns Main's first entry
|   3  registers the exported sets through the context's object
|      (virtual functions at 76, 80, 88); returns the first cursor
|
| Data block (built by mkaslm): 'AXL1', below, image length, pointer
| count, set count, per set (record, descriptor), pointer offsets, image.
| Offsets are from A5 - below.

	.section .a5init,"ax"
	.globl	__aslm_entry
	.globl	__aslm_blk

__aslm_entry:
	link	%fp,#0
	movem.l	%d2-%d7/%a2-%a4,-(%sp)
	move.l	8(%fp),%d0
	beq.s	init
	subq.l	#1,%d0
	beq.s	setctx
	subq.l	#1,%d0
	beq.s	anchor
	subq.l	#1,%d0
	beq.w	exports
	moveq	#0,%d0
out:
	movem.l	(%sp)+,%d2-%d7/%a2-%a4
	unlk	%fp
	rts

init:
	lea	__aslm_blk(%pc),%a2
	move.l	12(%fp),%d3		| A5
	move.l	%d3,%a3
	sub.l	4(%a2),%a3		| base
	move.l	16(%a2),%d0
	lsl.l	#3,%d0
	lea	20(%a2,%d0.l),%a1	| pointer offsets
	move.l	12(%a2),%d1
	move.l	%d1,%d0
	lsl.l	#2,%d0
	lea	0(%a1,%d0.l),%a0	| image
	move.l	%a3,%a4
	move.l	8(%a2),%d0
	bra.s	2f
1:	move.b	(%a0)+,(%a4)+
2:	subq.l	#1,%d0
	bpl.s	1b
	move.l	4(%a2),%d0
	sub.l	8(%a2),%d0
	bra.s	4f
3:	clr.b	(%a4)+
4:	subq.l	#1,%d0
	bpl.s	3b
	bra.s	6f
5:	move.l	(%a1)+,%d0
	add.l	%d3,0(%a3,%d0.l)
6:	subq.l	#1,%d1
	bpl.s	5b
	move.l	%a3,__aslm_base
	moveq	#0,%d0
	bra.w	out

setctx:
	move.l	12(%fp),__aslm_ctx
	moveq	#0,%d0
	bra.w	out

anchor:
	move.l	__aslm_ctx,%d0
	beq.s	1f
	move.l	%d0,%a0
	clr.l	12(%a0)
1:	lea	__aslm_main0,%a0
	move.l	%a0,%d0
	bra.w	out

| cursor = obj->v76(obj, n); for each set: cursor = obj->v80(obj, cursor,
| callback, record); returns the first cursor
exports:
	move.l	__aslm_ctx,%a0
	move.l	20(%a0),%a2		| object
	lea	__aslm_blk(%pc),%a3
	move.l	16(%a3),%d3		| sets
	move.l	%d3,-(%sp)
	move.l	%a2,-(%sp)
	move.l	(%a2),%a0
	move.l	76(%a0),%a0
	jsr	(%a0)
	addq.l	#8,%sp
	move.l	%d0,%d4			| first cursor
	move.l	%d0,%d5			| cursor
	moveq	#0,%d6			| set
	lea	cbtab(%pc),%a4
	bra.s	2f
1:	move.l	%d6,%d0
	lsl.l	#3,%d0
	move.l	20(%a3,%d0.l),%d0	| record
	add.l	__aslm_base,%d0
	move.l	%d0,-(%sp)
	pea	(%a4)
	move.l	%d5,-(%sp)
	move.l	%a2,-(%sp)
	move.l	(%a2),%a0
	move.l	80(%a0),%a0
	jsr	(%a0)
	lea	16(%sp),%sp
	move.l	%d0,%d5
	addq.l	#6,%a4
	addq.l	#1,%d6
2:	cmp.l	%d3,%d6
	blt.s	1b
	move.l	%d4,%d0
	bra.w	out

| callback for set i: obj->v88(obj, a, descriptor, 0)
cbtab:
	moveq	#0,%d0
	bra.w	cbcom
	moveq	#1,%d0
	bra.w	cbcom
	moveq	#2,%d0
	bra.w	cbcom
	moveq	#3,%d0
	bra.w	cbcom
	moveq	#4,%d0
	bra.w	cbcom
	moveq	#5,%d0
	bra.w	cbcom
	moveq	#6,%d0
	bra.w	cbcom
	moveq	#7,%d0
	bra.w	cbcom
cbcom:
	link	%fp,#0
	movem.l	%d2/%a2,-(%sp)
	move.l	__aslm_ctx,%a0
	move.l	20(%a0),%a2
	lsl.l	#3,%d0
	lea	__aslm_blk(%pc),%a1
	move.l	24(%a1,%d0.l),%d0	| descriptor
	add.l	__aslm_base,%d0
	clr.l	-(%sp)
	move.l	%d0,-(%sp)
	move.l	8(%fp),-(%sp)
	move.l	%a2,-(%sp)
	move.l	(%a2),%a0
	move.l	88(%a0),%a0
	jsr	(%a0)
	lea	16(%sp),%sp
	movem.l	(%sp)+,%d2/%a2
	unlk	%fp
	rts

	.even
__aslm_blk:

	.text
	.globl	__aslm_main0
__aslm_main0:
	rts

	.bss
	.even
__aslm_ctx:	.skip	4
__aslm_base:	.skip	4
