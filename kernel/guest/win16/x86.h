/*
 * x86.h -- the Win16 environment's x86 interpreter: an 80386 integer
 * core running 16-bit code segments, in real mode (for the instruction
 * tests) or in protected mode with a local descriptor table the host
 * owns (Win16 selectors).  Guest memory is one byte array, guest
 * byte order (little-endian) whatever the host's.  Floating point is
 * x87.c's.
 *
 * Interrupts, faults and the thunk opcode go to the host's handlers;
 * native code calls guest code with x86_call and is called from it
 * through thunks (0F FF lo hi: host function lo|hi<<8).
 */
#ifndef X86_H
#define X86_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

/* registers, in encoding order */
#define	R_AX	0
#define	R_CX	1
#define	R_DX	2
#define	R_BX	3
#define	R_SP	4
#define	R_BP	5
#define	R_SI	6
#define	R_DI	7

/* segment registers, in encoding order */
#define	S_ES	0
#define	S_CS	1
#define	S_SS	2
#define	S_DS	3
#define	S_FS	4
#define	S_GS	5

/* flags */
#define	F_CF	0x0001
#define	F_PF	0x0004
#define	F_AF	0x0010
#define	F_ZF	0x0040
#define	F_SF	0x0080
#define	F_TF	0x0100
#define	F_IF	0x0200
#define	F_DF	0x0400
#define	F_OF	0x0800
#define	F_IOPL	0x3000
#define	F_NT	0x4000
#define	F_RF	0x10000
#define	F_VM	0x20000
#define	F_ARITH	(F_CF|F_PF|F_AF|F_ZF|F_SF|F_OF)

/* exceptions */
#define	X_DE	0
#define	X_DB	1
#define	X_BP	3
#define	X_OF	4
#define	X_BR	5
#define	X_UD	6
#define	X_NM	7
#define	X_NP	11
#define	X_SS	12
#define	X_GP	13

/* descriptor access byte */
#define	D_P	0x80
#define	D_DPL	0x60
#define	D_S	0x10	/* code or data */
#define	D_CODE	0x08
#define	D_EXPD	0x04	/* data: expand down */
#define	D_CONF	0x04	/* code: conforming */
#define	D_W	0x02	/* data: writable */
#define	D_R	0x02	/* code: readable */
#define	D_A	0x01

#define	LDTSIZE	8192
#define	SEL(i)	((i) << 3 | 7)		/* LDT, RPL 3, as Windows hands them out */
#define	SELIX(s) ((s) >> 3)

struct desc {
	u32	d_base;		/* linear, an offset into x86_mem */
	u32	d_limit;	/* bytes - 1 */
	u8	d_acc;		/* D_*; 0: free */
	u8	d_flags;	/* host's own: DF_* */
	u16	d_owner;	/* host's own: the module or task that owns it */
};
#define	DF_HUGE	0x01	/* not the first of a huge block's selectors */

struct seg {
	u32	base;
	u32	limit;
	u16	sel;
	u8	acc;
	u8	big;		/* B/D bit (always 0 for Win16 code) */
};

struct x86 {
	u32	r[8];
	u32	eip;
	u32	fl;		/* flags, F_ARITH when lf_op is LF_NONE */
	struct seg s[6];
	int	prot;		/* protected mode: selectors through ldt */
	u32	cr0;
	/* lazy arithmetic flags */
	int	lf_op;		/* LF_* */
	int	lf_sz;		/* 0 byte, 1 word, 2 dword */
	u32	lf_dst, lf_src, lf_res;
	/* decoding state of the instruction in hand */
	u32	ip0;		/* its first byte */
	u32	sp0;		/* ESP before it, back on a fault */
	int	ovseg;		/* segment override, -1 none */
	int	o32, a32;	/* operand and address size 32 */
	int	rep;		/* 0, 0xf2, 0xf3 */
	/* the host */
	u8	*mem;
	u32	memsize;
	struct desc *ldt;
	int	(*intr)();	/* (cpu, n): software interrupt; 0 = not handled */
	int	(*fault)();	/* (cpu, n, error): exception, IP at the instruction; 0 = stop */
	void	(*thunk)();	/* (cpu, n): host function n */
	void	(*io)();	/* (cpu, port, size, out, value *) */
	void	*user;
	int	stop;		/* x86_run returns when set */
	void	(*trace)();	/* (cpu): before each instruction, when set (debugging) */
	int	depth;		/* nested x86_call */
	u32	icount;		/* instructions run, wraps */
	int	halted;
	int	fpu;		/* an x87 is there */
	void	*x87;		/* its state (x87.c) */
};

#define	LF_NONE	0
#define	LF_ADD	1
#define	LF_SUB	2
#define	LF_LOGIC 3
#define	LF_INC	4
#define	LF_DEC	5

#define	REG16(c, i)	((c)->r[i] & 0xffff)

extern void x86_init();		/* (cpu, mem, size, ldt) */
extern int x86_step();		/* (cpu): one instruction; 0 ok, else stopped */
extern void x86_run();		/* (cpu): until stop */
extern int x86_call();		/* (cpu, sel, off): far call from the host, until it returns */
extern u32 x86_flags();		/* (cpu): flags, arithmetic ones made */
extern void x86_setflags();	/* (cpu, value) */
extern int x86_loadseg();	/* (cpu, sreg, sel): 0 ok, else exception */
extern void x86_push16();	/* (cpu, v) */
extern u32 x86_pop16();		/* (cpu) */
extern void x86_exception();	/* (cpu, n, error): raise from a host handler */
extern u32 x86_lin();		/* (cpu, sel, off): linear address, or ~0 */
extern int x86_retthunk;	/* thunk number that ends x86_call */
extern void x86_setret();	/* (sel, off): where x86_call returns to (a return thunk) */
extern void x87_init();		/* (cpu) */
extern void x87_exec();		/* (cpu, op, mod, reg, rm, seg, off) */

/* guest memory, little-endian whatever the host */
#define	RD8(m, a)	((m)[a])
#define	RD16(m, a)	((m)[a] | (m)[(a) + 1] << 8)
#define	RD32(m, a)	(RD16(m, a) | (u32)RD16(m, (a) + 2) << 16)
#define	WR8(m, a, v)	((m)[a] = (v))
#define	WR16(m, a, v)	((m)[a] = (v), (m)[(a) + 1] = (v) >> 8)
#define	WR32(m, a, v)	(WR16(m, a, v), WR16(m, (a) + 2, (v) >> 16))

#endif
