| auxentry.s -- entry from A/UX Startup's launch (MMU off, IPL 7).
|
| launch loads the COFF sections at their physical addresses, the info
| block ('Pigs') at pstart - 0x400 and 8 KB of Mac low memory at
| pstart - 0x4000, then enters with d0 = 0x536D72xx, a0 = info block.
| The CPU is supervisor, translation and caches off; the TT registers,
| VBR and SP are whatever Mac OS left.
|
| aux_pstart..aux_pstart_end is copied by elf2coff into the COFF pstart
| section (physical 0x4000), so it must be position independent.  It
| checks that .text sits at its link address, then enters aux_entry there.
|
| aux_entry takes the machine to a known state, zeroes BSS (launch does
| not), converts the info block and low memory into a bootinfo record
| at the first even address past `end', and enters mac_entry
| as a direct boot (d0 = 0).

LOWOFF	= 0x3C00		| info block - low-memory copy

| info block ('Pigs') fields
PIGS	= 0x50696773
PG_TEXTV = 0x8C
PG_TEXTP = 0x90
PG_MACH	= 0xB0
PG_FLAGS = 0xB6

| Mac low-memory globals
LM_SCRROW = 0x106
LM_MEMTOP = 0x108
LM_CPUFLAG = 0x12F
LM_VIA	= 0x1D4
LM_SCCRD = 0x1D8
LM_ROMBASE = 0x2AE
LM_SCRNBASE = 0x824
LM_HWCFG = 0xB22

MACH_Q800 = 33			| Gestalt 'mach' 35 - 2
DJMEMC_END = 0x50F0E02C		| end of RAM in 4 MB units (low byte)

VIA1_IER = 0x50F01C00
VIA2_IER = 0x50F03C00
SCC_DEF	= 0x50F0C020
VIA1_DEF = 0x50F00000

	.text
	.globl	aux_pstart
	.globl	aux_pstart_end
aux_pstart:
	movew	&0x2700,%sr
	movel	%a0@(PG_TEXTP),%d1
	cmpl	%a0@(PG_TEXTV),%d1
	bne.b	Lpsbad
	movel	&aux_entry,%a1
	jmp	%a1@
Lpsbad:
	stop	&0x2700			| .text was moved: cannot run
	bra.b	Lpsbad
aux_pstart_end:

	.globl	aux_entry
aux_entry:
	movew	&0x2700,%sr
	movel	%d0,aux_d0
	movel	%a0,aux_a0
	lea	aux_stack_top,%sp

	| caches, TT registers and translation off (68040/68060)
	.word	0xf4f8			| cpusha bc
	moveq	&0,%d0
	movec	%d0,%cacr
	.word	0x4e7b,0x0003		| movec %d0,%tc
	.word	0x4e7b,0x0004		| movec %d0,%itt0
	.word	0x4e7b,0x0005		| movec %d0,%itt1
	.word	0x4e7b,0x0006		| movec %d0,%dtt0
	.word	0x4e7b,0x0007		| movec %d0,%dtt1
	.word	0xf518			| pflusha

	movel	&edata,%a0
	movel	&end,%d1
	subl	%a0,%d1
	bra.b	Lbz
Lbzl:
	clrb	%a0@+
Lbz:
	subql	&1,%d1
	bcc.b	Lbzl

	lea	aux_vec,%a0
	movel	&aux_exc,%d0
	movel	&255,%d1
Lvfill:
	movel	%d0,%a0@+
	dbf	%d1,Lvfill
	lea	aux_vec,%a0
	movec	%a0,%vbr

	moveb	&0x7F,VIA1_IER
	moveb	&0x7F,VIA2_IER

	jsr	mac_scc_init
	movel	aux_a0,%sp@-		| screen console from the low-memory copy
	jsr	fbcons_auxinit
	addql	&4,%sp
	pea	Lban
	jsr	mac_puts
	movel	aux_d0,%sp@
	jsr	mac_puthex
	pea	Lban2
	jsr	mac_puts
	movel	aux_a0,%sp@
	jsr	mac_puthex
	addql	&8,%sp

	movel	aux_d0,%d0
	andil	&0xFFFFFF00,%d0
	cmpil	&0x536D7200,%d0
	beq.b	L1
	pea	Lnosmr
	jsr	mac_stop
L1:
	moveal	aux_a0,%a2		| a2 = info block
	cmpil	&PIGS,%a2@
	beq.b	L2
	pea	Lnopigs
	jsr	mac_stop
L2:
	moveal	%a2,%a3
	subal	&LOWOFF,%a3		| a3 = low-memory copy

	pea	Lmach
	jsr	mac_puts
	moveq	&0,%d0
	movew	%a2@(PG_MACH),%d0
	movel	%d0,%sp@
	jsr	mac_puthex
	pea	Lflags
	jsr	mac_puts
	moveq	&0,%d0
	movew	%a2@(PG_FLAGS),%d0
	movel	%d0,%sp@
	jsr	mac_puthex
	pea	Lcpu
	jsr	mac_puts
	moveq	&0,%d0
	moveb	%a3@(LM_CPUFLAG),%d0
	movel	%d0,%sp@
	jsr	mac_puthex
	lea	%sp@(12),%sp

	cmpiw	&MACH_Q800,%a2@(PG_MACH)
	beq.b	L3
	pea	Lnotq800
	jsr	mac_stop
L3:
	cmpib	&4,%a3@(LM_CPUFLAG)
	beq.b	L4
	pea	Lnot040
	jsr	mac_stop
L4:
	| RAM: one range from 0 to the djMEMC end register
	movel	DJMEMC_END,%d3
	andil	&0xFF,%d3
	moveq	&22,%d0
	lsll	%d0,%d3			| d3 = RAM size
	pea	Lmem
	jsr	mac_puts
	movel	%d3,%sp@
	jsr	mac_puthex
	pea	Lmemtop
	jsr	mac_puts
	movel	%a3@(LM_MEMTOP),%sp@
	jsr	mac_puthex
	pea	Lnl
	jsr	mac_puts
	lea	%sp@(12),%sp
	tstl	%d3
	bne.b	L5
	pea	Lnomem
	jsr	mac_stop
L5:
	| boot record at the first even address past the image
	movel	&end,%d0
	addql	&1,%d0
	andil	&0xFFFFFFFE,%d0
	moveal	%d0,%a4

	movel	&0x00010008,%a4@+	| BI_MACHTYPE: MACH_MAC
	movel	&3,%a4@+
	movel	&0x00020008,%a4@+	| BI_CPUTYPE: CPU_68040
	movel	&4,%a4@+
	movel	&0x00040008,%a4@+	| BI_MMUTYPE: MMU_68040
	movel	&4,%a4@+
	btst	&4,%a3@(LM_HWCFG)	| HWCfgFlags bit 12: FPU present
	beq.b	L6
	movel	&0x00030008,%a4@+	| BI_FPUTYPE: FPU_68040
	movel	&4,%a4@+
L6:
	movel	&0x0005000C,%a4@+	| BI_MEMCHUNK: 0, size
	clrl	%a4@+
	movel	%d3,%a4@+
	movel	&0x80000008,%a4@+	| BI_MAC_MODEL: Gestalt 'mach'
	moveq	&2,%d0
	addw	%a2@(PG_MACH),%d0
	andil	&0xFFFF,%d0
	movel	%d0,%a4@+
	movel	&0x80090008,%a4@+	| BI_MAC_MEMSIZE: MB
	movel	%d3,%d0
	moveq	&20,%d1
	lsrl	%d1,%d0
	movel	%d0,%a4@+
	movel	&0x800B0008,%a4@+	| BI_MAC_ROMBASE
	movel	%a3@(LM_ROMBASE),%a4@+

	movel	&0x80100008,%a4@+	| BI_MAC_VIA1BASE
	movel	%a3@(LM_VIA),%d0
	movel	%d0,%d1
	andil	&0xFFF00000,%d1
	cmpil	&0x50F00000,%d1
	beq.b	L7
	movel	&VIA1_DEF,%d0
L7:
	movel	%d0,%a4@+

	movel	&0x80060008,%a4@+	| BI_MAC_SCCBASE
	movel	%a3@(LM_SCCRD),%d0
	movel	%d0,%d1
	andil	&0xFFFFFFC0,%d1
	cmpil	&0x50F0C000,%d1
	beq.b	L8
	movel	&SCC_DEF,%d0
L8:
	movel	%d0,%a4@+

	movel	%a3@(LM_SCRNBASE),%d0	| video: base and row bytes only
	beq.b	L9
	moveq	&0,%d1
	movew	%a3@(LM_SCRROW),%d1
	andiw	&0x3FFF,%d1
	beq.b	L9
	movel	&0x80010008,%a4@+	| BI_MAC_VADDR
	movel	%d0,%a4@+
	movel	&0x80030008,%a4@+	| BI_MAC_VROW
	movel	%d1,%a4@+
L9:
	movel	&0x8F000010,%a4@+	| BI_MAC_AUXINFO (private):
	movel	%a2,%a4@+		|   info block, hand-off d0,
	movel	aux_d0,%a4@+		|   low-memory copy
	movel	%a3,%a4@+
	clrl	%a4@+			| BI_LAST

	moveal	%a2,%a0
	moveq	&0,%d0
	jmp	mac_entry

| Exception before mac_entry: print vector offset and PC, stop.
aux_exc:
	moveal	%sp,%a2
	lea	aux_stack_top,%sp
	pea	Lexc
	jsr	mac_puts
	moveq	&0,%d0
	movew	%a2@(6),%d0
	andiw	&0x0FFF,%d0
	movel	%d0,%sp@
	jsr	mac_puthex
	pea	Lexcpc
	jsr	mac_puts
	movel	%a2@(2),%sp@
	jsr	mac_puthex
	pea	Lnl
	jsr	mac_stop

	.data
Lban:	.asciz	"\nA/UX Startup hand-off: d0 "
Lban2:	.asciz	" info "
Lmach:	.asciz	"\n  machine "
Lflags:	.asciz	" flags "
Lcpu:	.asciz	" CPUFlag "
Lmem:	.asciz	"\n  djMEMC RAM "
Lmemtop: .asciz	" MemTop "
Lnl:	.asciz	"\n"
Lexc:	.asciz	"\nauxentry: exception, vector offset "
Lexcpc:	.asciz	" pc "
Lnosmr:	.asciz	"\nauxentry: d0 is not an A/UX Startup hand-off\n"
Lnopigs: .asciz	"\nauxentry: no 'Pigs' info block at a0\n"
Lnotq800: .asciz "\nauxentry: machine type is not a Quadra 800 (33)\n"
Lnot040: .asciz	"\nauxentry: CPUFlag is not 68040\n"
Lnomem:	.asciz	"\nauxentry: djMEMC reports no RAM\n"
	.even

	.balign	4
aux_d0:	.space	4
aux_a0:	.space	4
aux_stack: .space 1024
aux_stack_top:

| filled after BSS is zeroed; used only before mac_entry
	.bss
	.balign	4
aux_vec: .space	1024
