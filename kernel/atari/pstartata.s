| pstartata.s -- pstart for the Falcon with a 68040 or 68060, derived from
| the AMIX 040/060 port's pstart040.s (MIT).  Differences from it:
|   * ITT0 matches supervisor fetches below 1 GB only, so user space
|     goes through the page tables;
|   * DTT0 (ST-RAM, ROM, 24-bit I/O) and DTT1 (I/O at 0xFFFFxxxx and
|     IDE) are noncacheable serialized;
|   * FastRAM is identity-mapped by supervisor page tables: copyback,
|     page tables and u-areas noncacheable;
|   * a window onto the VM region at VA = PA | 0x60000000 holds the
|     page array;
|   * on the 060: superscalar dispatch on, FPU on unless the part has
|     none, branch cache and store buffer on.
| The 030 shadow-table build and the tail are kept, since the generic
| kernel reads cpuroot/userroot/st_top1/kuptr/ublksde.

	.text
	.globl	pstart
pstart:
	linkw	%fp,&-40
	moveml	%d2-%d6/%a2-%a3,%sp@-
| ---- 0xd86: build cpuroot / userroot (8-byte 030 root descriptors) ----
Lpbuild:
	movew	&32767,cpuroot
	bfclr	cpuroot+2{&0:&6}
	bfclr	cpuroot+2{&6:&8}
	moveq	&3,%d1
	bfins	%d1,cpuroot+3{&6:&2}
	clrl	cpuroot+4
	movel	cpuroot,userroot
	movel	cpuroot+4,userroot+4

| ---- 0xdc2: allocate + zero the 4-entry root table (tbl), set kuptr ----
	movel	&end,%d0
	addil	&2047,%d0
	moveq	&11,%d1
	lsrl	%d1,%d0
	movel	%d0,%d6
	lsll	%d1,%d6
	movel	%d6,%fp@(-28)
	pea	0x30
	movel	%fp@(-28),%sp@-
	jsr	bzero
	movel	%fp@(-28),cpuroot+4
	movel	%fp@(-28),%fp@(-36)
	moveq	&32,%d1
	addl	%fp@(-28),%d1
	movel	%d1,kuptr
	moveq	&48,%d1
	addl	%d1,%fp@(-28)

| ---- 0xe08: tbl[0] identity cacheable, addr 0 (VA 0x00000000) ----
	moveal	%fp@(-36),%a0
	clrl	%a0@(4)
	moveal	%fp@(-36),%a0
	movew	&32767,%a0@
	moveal	%fp@(-36),%a0
	bfclr	%a0@(2){&0:&6}
	moveal	%fp@(-36),%a0
	movel	&192,%d1
	bfins	%d1,%a0@(2){&6:&8}
	moveal	%fp@(-36),%a0
	moveq	&1,%d1
	bfins	%d1,%a0@(3){&6:&2}

| ---- 0xe3e: tbl[1] paged (VA 0x40000000) -> st_top1 ----
	movel	%fp@(-28),st_top1
	moveal	%fp@(-36),%a0
	addqw	&8,%a0
	movel	st_top1,%a0@(4)
	moveal	%fp@(-36),%a0
	addqw	&8,%a0
	movew	&1279,%a0@
	moveal	%fp@(-36),%a0
	addqw	&8,%a0
	bfclr	%a0@(2){&0:&6}
	moveal	%fp@(-36),%a0
	addqw	&8,%a0
	movel	&192,%d1
	bfins	%d1,%a0@(2){&6:&8}
	moveal	%fp@(-36),%a0
	addqw	&8,%a0
	moveq	&3,%d1
	bfins	%d1,%a0@(3){&6:&2}

| ---- 0xe8a: tbl[2] identity cache-inhibited (VA 0x80000000) ----
	moveq	&16,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	movel	&0x80000000,%a0@(4)
	moveq	&16,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	movew	&32767,%a0@
	moveq	&16,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	bfclr	%a0@(2){&0:&6}
	moveq	&16,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	movel	&208,%d1
	bfins	%d1,%a0@(2){&6:&8}
	moveq	&16,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	moveq	&1,%d1
	bfins	%d1,%a0@(3){&6:&2}

| ---- 0xed8: tbl[3] identity cache-inhibited (VA 0xC0000000) ----
	moveq	&24,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	movel	&0xc0000000,%a0@(4)
	moveq	&24,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	movew	&32767,%a0@
	moveq	&24,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	bfclr	%a0@(2){&0:&6}
	moveq	&24,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	movel	&208,%d1
	bfins	%d1,%a0@(2){&6:&8}
	moveq	&24,%d1
	addl	%fp@(-36),%d1
	moveal	%d1,%a0
	moveq	&1,%d1
	bfins	%d1,%a0@(3){&6:&2}

| ---- 0xf26: allocate + zero st_top1 segment table, then the u-area pages ----
	pea	0x2800
	movel	st_top1,%sp@-
	jsr	bzero
	movel	st_top1,%d2
	addil	&12287,%d2
	moveq	&11,%d1
	lsrl	%d1,%d2
	movel	%d2,%d3
	asll	%d1,%d3
	pea	0x2000
	movel	%d3,%sp@-
	jsr	bzero
	moveal	kuptr,%a2
	clrl	%d5
	addaw	&24,%sp

| ---- 0xf62: fill kuptr[0..3] with 2KB page descriptors at d3 ----
Lploop:
	moveq	&3,%d1
	cmpl	%d5,%d1
	blt.w	Lpuarea
	movel	%d5,%d0
	moveq	&11,%d1
	asll	%d1,%d0
	addl	%d3,%d0
	addil	&2047,%d0
	lsrl	%d1,%d0
	lsll	%d1,%d0
	moveq	&1,%d1
	orl	%d0,%d1
	movel	%d1,%a2@
	addql	&1,%d5
	addqw	&4,%a2
	bra.w	Lploop

| ---- 0xf8a: st_top1[ (u>>17)&0x1fff ] -> kuptr (DT=2), set ublksde ----
Lpuarea:
	addql	&4,%d2
	movel	&u,%d0
	moveq	&17,%d1
	lsrl	%d1,%d0
	andil	&8191,%d0
	asll	&3,%d0
	moveal	%d0,%a3
	addal	st_top1,%a3
	moveq	&64,%d1
	bfins	%d1,%a3@(2){&6:&8}
	movew	&3,%a3@
	moveq	&2,%d1
	bfins	%d1,%a3@(3){&6:&2}
	movel	kuptr,%a3@(4)
	movel	%a3,%d1
	addql	&4,%d1
	movel	%d1,ublksde


| ---- 68040 MMU enable ----
	movel	&ata_mmu_buf,%d0
	addil	&4095,%d0
	andil	&0xfffff000,%d0		| d0 = u_phys = uarea040 (4KB-aligned, 8KB)
	movel	%d0,%d1
	addil	&8192,%d1
	addil	&511,%d1
	andil	&0xfffffe00,%d1
	movel	%d1,%a1			| a1 = root040 = uarea040+8KB (512-aligned)
	movel	%d1,kroot040		| export the 040 kernel root for kvm_init (kas@0x14)
	movel	%d1,%d3
	addil	&512,%d3		| d3 = kptr040 = root040 + 512  (512-aligned)
	movel	%d3,kptr040		| export the pointer-table base for kvm_init
	movel	%d3,%d6
	addil	&16384,%d6		| d6 = uarea_pt = kptr040 + 16KB (512 => 256-aligned)

	| uarea_pt[0] = u_phys | S(0x80)|nocache(0x60)|PDT(0x01) = | 0xE1
	moveal	%d6,%a0
	movel	%d0,%d1
	oril	&0xe1,%d1
	movel	%d1,%a0@
	| uarea_pt[1] = (u_phys+0x1000) | 0xE1
	movel	%d0,%d1
	addil	&0x1000,%d1
	oril	&0xe1,%d1
	movel	%d1,%a0@(4)

	| kptr040[0] = uarea_pt | UDT(0x02)   (root32/ptr0 -> the u-area page table)
	moveal	%d3,%a0
	movel	%d6,%d1
	oril	&0x02,%d1
	movel	%d1,%a0@

	| root040[32..63] = kptr040[k] | UDT(0x02)   (k=0..31; VA 0x40000000>>25 = 32)
	lea	%a1@(128),%a0		| a0 = &root040[32]  (32 * 4)
	movel	%d3,%d4			| d4 = running pointer-table addr
	moveq	&31,%d5			| 32 entries (k=0..31)
Lkroot:
	movel	%d4,%d1
	oril	&0x02,%d1
	movel	%d1,%a0@+
	addil	&512,%d4
	dbf	%d5,Lkroot

	| kuptr[i] = (uarea040 + i*0x800) | 1 (030 2KB page descriptors): the
	| kernel reaches the u-area physically through kuptr (mlsetup, p_addr)
	| and virtually through VA 0x40000000; both must be the same memory.
	| d0 still = uarea040 here.
	movel	kuptr,%a0
	movel	%d0,%d1
	oril	&1,%d1
	movel	%d1,%a0@
	movel	%d0,%d1
	addil	&0x800,%d1
	oril	&1,%d1
	movel	%d1,%a0@(4)
	movel	%d0,%d1
	addil	&0x1000,%d1
	oril	&1,%d1
	movel	%d1,%a0@(8)
	movel	%d0,%d1
	addil	&0x1800,%d1
	oril	&1,%d1
	movel	%d1,%a0@(12)

	| SRP = URP = root040_phys
	movel	%a1,%d0
	.word	0x4e7b,0x0807		| movec %d0,%srp
	.word	0x4e7b,0x0806		| movec %d0,%urp


	| FastRAM identity map past the bootstrap tables; d2 moves past it
	moveml	%d0-%d1/%a0-%a1,%sp@-
	movel	%d2,%d0
	moveq	&11,%d1
	lsll	%d1,%d0
	movel	%d0,%sp@-
	jsr	ata_idmap_build
	addql	&4,%sp
	moveq	&11,%d1
	lsrl	%d1,%d0
	movel	%d0,%d2
	moveml	%sp@+,%d0-%d1/%a0-%a1

	| RAM window, before translation is enabled
	moveml	%d0-%d6/%a0-%a3,%sp@-
	jsr	ata_iomap_build
	moveml	%sp@+,%d0-%d6/%a0-%a3

	| transparent translation, supervisor only: code below 1 GB, data
	| in the low 16 MB and at 2-4 GB
	movel	&0x003fa000,%d0
	.word	0x4e7b,0x0004		| movec %d0,%itt0  (WT cacheable)
	clrl	%d0
	.word	0x4e7b,0x0005		| movec %d0,%itt1
	movel	&0x0000a040,%d0
	.word	0x4e7b,0x0006		| movec %d0,%dtt0  (noncacheable serialized)
	movel	&0x807fa040,%d0
	.word	0x4e7b,0x0007		| movec %d0,%dtt1  (noncacheable serialized)

	.word	0xf4f8			| cpusha bc
	.word	0xf518			| pflusha
	movel	&0x00008000,%d0
	.word	0x4e7b,0x0003		| movec %d0,%tc  (E=1, 4KB pages)
	.word	0xf458			| cinva dc
	.word	0xf498			| cinva ic
	movel	&0x80008000,%d0		| IC on, DC on (write-through via CM)
	movel	cputype,%d1
	cmpil	&60,%d1
	bnew	Lcache
	| 060: ESS on; FPU on unless PCR bit 16 says the part has none
	.word	0x4e7a,0x1808		| movec %pcr,%d1
	movel	%d1,pcr_boot
	bset	&0,%d1
	btst	&16,%d1
	bnes	Lpcr
	bclr	&1,%d1
Lpcr:	.word	0x4e7b,0x1808		| movec %d1,%pcr
	movel	&0xa0c08000,%d1		| EDC ESB EBC CABC EIC
	.word	0x4e7b,0x1002		| movec %d1,%cacr
	movel	&0xa0808000,%d0		| the same without CABC, for reloads
Lcache:
	movel	%d0,cacr
	movel	%d0,sup_cacr
	.word	0x4e7b,0x0002		| movec %d0,%cacr

| ---- 0xfe6: tail, with v converted to 4KB clicks ----
	jsr	vstart
	| d2 is the bootstrap high-water mark in 2KB clicks; every later click
	| consumer is 4KB (maxclick = memsize>>12, sysseginit v<<12, kvm_init
	| smsegs = maxclick>>6 - (v+63)>>6).  Unconverted, smsegs drops to <= 0:
	| panic "No space for mapping files".  Round up; d2 is dead after this.
	addql	&1,%d2
	lsrl	&1,%d2
	movel	%d2,%sp@-
	jsr	mlsetup
	jsr	ata_ramwin_check
	| kvm_init set kas@(0x14) to the unused 030 root; hat_pteload walks
	| kernel segments through it.  Point it at root040, which the MMU walks,
	| before main()'s first hat_pteload (fork1 -> procdup -> segu_get).
	movel	kroot040,%d0
	movel	%d0,kas+0x14
	moveq	&95,%d0
	addl	proc_sched,%d0
	moveq	&-16,%d1
	andl	%d1,%d0
	movel	%d0,%sp@-
	jsr	svirtophys
	bra.w	Lpepi
Lpepi:
	moveml	%fp@(-68),%d2-%d6/%a2-%a3
	moveal	%d0,%a0
	unlk	%fp
	rts
	nop
	nop
	nop
	nop			| pad .text to a 4-byte multiple (loader copies text+data as one block)

	.data
	.balign	4096			| own pages: mapped noncacheable
| Zeroed table space: 8 KB u-area, root, 16 KB pointer tables, u-area
| page table (carved at run time, as in pstart040).
	.globl	ata_mmu_buf
ata_mmu_buf:
	.space	32768
	.balign 4
