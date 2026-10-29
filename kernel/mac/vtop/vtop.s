| vtop(va, p) -- physical address of va, for device transfers and /proc.
|
|   p != 0  va is a user address of p, at any value: walked through p's
|           040 tables, 0 if not resident.  Callers lock the page first.
|   p == 0  kernel address: identity below 1 GB, kernel tables above.
|
| Only physio buffers carry a nonzero b_proc, always their owner with
| B_PHYS; buffer-cache, page-I/O and bounce buffers carry 0.
	.text
	.globl	vtop
vtop:
	linkw	%fp,&0
	movel	%fp@(12),%d0		| p
	beqw	Lvt_kern
	movel	%d0,%sp@-
	movel	%fp@(8),%sp@-
	jsr	uvatopte040		| leaf PTE or 0
	addqw	&8,%sp
	tstl	%d0
	beqw	Lvt_ret
	andil	&0xfffff000,%d0
	movel	%fp@(8),%d1
	andil	&0xfff,%d1
	orl	%d1,%d0
	braw	Lvt_ret
Lvt_kern:
	movel	%fp@(8),%d0
	cmpil	&0x40000000,%d0
	bcsw	Lvt_id
	clrl	%sp@-
	movel	%d0,%sp@-
	jsr	vtop_orig		| kernel tables
	addqw	&8,%sp
Lvt_ret:
	moveal	%d0,%a0
	unlk	%fp
	rts
Lvt_id:
	movel	%d0,%sp@-		| one to one: RAM above the kernel's is elsewhere
	jsr	hr_tobus
	addqw	&4,%sp
	braw	Lvt_ret
	.balign	4

| prmapin(p, va, writing): /proc's kernel address for a locked user page,
| 0 for a frame outside RAM (a device mapping), which has none.
	.globl	prmapin
prmapin:
	movel	%sp@(4),%sp@-		| p
	movel	%sp@(12),%sp@-		| va
	jsr	vtop
	movel	%d0,%sp@
	jsr	hr_tokern
	addqw	&8,%sp
	movel	%d0,%d1
	lsrl	&8,%d1
	lsrl	&4,%d1			| page frame
	cmpl	pages_base,%d1
	bcsw	Lpm_none
	cmpl	pages_end,%d1
	bccw	Lpm_none
	moveal	%d0,%a0
	rts
Lpm_none:
	moveq	&0,%d0
	moveal	%d0,%a0
	rts

| /proc's copy of one page: no kernel address is EFAULT.
	.globl	dbg_uiomove_publish
dbg_uiomove_publish:
	tstl	%sp@(4)
	beqw	Lpu_fault
	jmp	__amix_dbg_uiomove_publish
Lpu_fault:
	moveq	&14,%d0			| EFAULT
	moveal	%d0,%a0
	rts
