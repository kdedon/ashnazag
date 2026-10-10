/* Shared CP/M-86 decoder, executor, loader, and BDOS bridge interfaces.
 * Target C uses 16-bit int; guest words use unsigned short. Access guest
 * memory bytewise for little-endian layout on host and Z8001 builds. */

#define i8	unsigned char		/* a guest byte			*/
#define i16	unsigned short		/* a guest word; 16 bits both ways */
#define i32	unsigned long		/* 32 bits both ways		*/

/* ------------------------------------------------------------------ */
/* guest state							       */

/* Segment slots, in the 8086's own sreg encoding (mod r/m reg field of
 * the MOV Sreg forms, and the two bits of the 26/2E/36/3E prefixes). */
#define S_ES	0
#define S_CS	1
#define S_SS	2
#define S_DS	3

/* Not a slot: `struct i86grp'.seg for a group that owns a segment but is
 * named by no segment REGISTER at entry.  The auxiliary groups of the
 * compact and large models are exactly that -- the loader gives each one
 * a 64 KB segment and publishes its paragraph in the base page's group
 * table, and the guest reaches it by loading that paragraph itself,
 * which i86resolve() then answers from the table i86place() filled. */
#define S_NONE	4

/* Word registers, in the 8086's reg-field encoding. */
#define R_AX	0
#define R_CX	1
#define R_DX	2
#define R_BX	3
#define R_SP	4
#define R_BP	5
#define R_SI	6
#define R_DI	7

/* FLAGS bits. */
#define F_CF	0x0001
#define F_PF	0x0004
#define F_AF	0x0010
#define F_ZF	0x0040
#define F_SF	0x0080
#define F_TF	0x0100
#define F_IF	0x0200
#define F_DF	0x0400
#define F_OF	0x0800

/* The six the lazy record owns.  Everything else (TF, IF, DF and the
 * bits an 8086 reads back as ones) lives in `fl' at all times. */
#define F_LAZY	(F_CF|F_PF|F_AF|F_ZF|F_SF|F_OF)

/* An 8086 reads bits 1, 3, 5 and 12-15 back as one.  PUSHF and LAHF are
 * the only things that can see it, and period software does compare
 * pushed flag words, so we carry it. */
#define F_ONES	0xF002

/* Lazy-flag operation classes (struct i86 .lz).  LZ_NONE means `fl' is
 * complete and nothing is pending.  See i86flags() for the formulae. */
#define LZ_NONE	0
#define LZ_ADD	1	/* r = a + b + lc     -- CF, PF, AF, ZF, SF, OF	*/
#define LZ_SUB	2	/* r = a - b - lc     -- ditto (CMP, NEG use it)	*/
#define LZ_LOG	3	/* AND/OR/XOR/TEST    -- CF = OF = 0, AF cleared	*/
#define LZ_INC	4	/* r = a + 1          -- CF preserved		*/
#define LZ_DEC	5	/* r = a - 1          -- CF preserved		*/

struct i86 {
	i16	r[8];		/* AX CX DX BX SP BP SI DI		*/
	i16	sr[4];		/* ES CS SS DS: the guest PARAGRAPH value */
	char	*sb[4];		/* ES CS SS DS: the host segment base	*/
	i16	so[4];		/* ES CS SS DS: byte bias INTO that host	*/
				/* segment.  Zero for every paragraph we	*/
				/* handed out, which is every paragraph	*/
				/* the DRI corpus ever writes; nonzero	*/
				/* only after E1s's slow path resolved a	*/
				/* write to a paragraph inside a segment	*/
				/* rather than at its base.  See setsr().*/
	i16	ip;
	i16	fl;		/* materialised flags + the eager bits	*/
	i8	lz;		/* LZ_*: pending lazy record, or LZ_NONE */
	i8	lw;		/* its width: 0 byte, 1 word		*/
	i8	lc;		/* its carry-in (ADC/SBB); 0 otherwise	*/
	i16	la, lb, lr;	/* its operands and result		*/
	i8	halt;		/* set by HLT and by a refusal		*/
	i8	fault;		/* a memory reference left its window	*/
	i16	foff;		/* ... at this guest offset		*/
	i8	fseg;		/* ... through this segment slot		*/
	i16	wseg;		/* the paragraph handed out as ENTRY SS	*/
	i8	wset;		/* ... set by i86place(); see X_WBOOT	*/
};

/* ------------------------------------------------------------------ */
/* decoder							       */

/* Operations.  One id per executable class, not per opcode: the ALU
 * eight, the shift eight and the two unary groups carry their variant
 * in .x, exactly as the 8086 encodes them, so the executor's switch is
 * over classes and its inner dispatch is over a field the hardware
 * already gave us.
 *
 * The values are ordered by how often the corpus runs each class, not
 * alphabetically and not by the encoding: the executor's switch becomes
 * a compare chain in ASCENDING CASE VALUE, so a low id is a short walk.
 * The first six are three quarters of everything that executes.
 * Renumbering them means regenerating the assembly decoder's tables and
 * reordering the assembly run loop's jtab. */
#define I_BAD		0	/* not decodable as an 8086 instruction	*/
#define I_ALU		1	/* .x = 0..7 add or adc sbb and sub xor cmp */
#define I_MOV		2
#define I_JCC		3	/* .x = condition 0..15; .disp = target	*/
#define I_JMP		4	/* near relative; .disp = target offset	*/
#define I_SHIFT		5	/* .x = 0..7 rol ror rcl rcr shl shr - sar */
#define I_NOT		6
#define I_INC		7
#define I_DEC		8
#define I_PUSH		9
#define I_POP		10
#define I_CALL		11	/* near relative; .disp = target offset	*/
#define I_RET		12	/* near;  .imm = bytes to pop (0 if none) */
#define I_TEST		13
#define I_NEG		14
#define I_XCHG		15
#define I_LEA		16
#define I_MOVSR		17	/* MOV to/from a segment register	*/
#define I_LXS		18	/* .x = S_ES (LES) or S_DS (LDS)	*/
#define I_MULDIV	19	/* .x = 4 MUL 5 IMUL 6 DIV 7 IDIV	*/
#define I_STRING	20	/* .x = 0 MOVS 1 CMPS 2 STOS 3 LODS 4 SCAS */
#define I_LOOP		21	/* .x = 0 LOOPNE 1 LOOPE 2 LOOP 3 JCXZ	*/
#define I_FLAG		22	/* .imm = the flag bit; .x = 0 clr 1 set 2 cpl */
#define I_INT		23	/* .imm = vector (INT3 decodes to 3)	*/
#define I_CALLI		24	/* through mod r/m; .x 0 = near, 1 = far */
#define I_JMPI		25	/* through mod r/m; .x 0 = near, 1 = far */
#define I_PUSHSR	26	/* .x = segment slot			*/
#define I_POPSR		27
#define I_PUSHF		28
#define I_POPF		29
#define I_CALLF		30
#define I_JMPF		31	/* far direct; .imm = off, .imm2 = seg	*/
#define I_RETF		32
#define I_IRET		33
#define I_INTO		34
#define I_XLAT		35
#define I_CBW		36
#define I_CWD		37
#define I_LAHF		38
#define I_SAHF		39
#define I_DAA		40
#define I_DAS		41
#define I_AAA		42
#define I_AAS		43
#define I_AAM		44
#define I_AAD		45
#define I_NOP		46
#define I_HLT		47
#define I_WAIT		48
#define I_ESC		49	/* 8087 escape: no coprocessor here	*/
#define I_IO		50	/* IN/OUT: no PC hardware here		*/
#define I_NIMPL		51	/* a real 8086 instruction we do not do	*/

/* struct i86in .fl bits */
#define IN_MODRM	0x01	/* a mod r/m byte was consumed		*/
#define IN_MEM		0x02	/* ... and it names memory (mod != 3)	*/
#define IN_SEGOVR	0x04	/* an explicit segment prefix was seen	*/
#define IN_REP		0x08	/* F3 seen (REP / REPE)			*/
#define IN_REPNE	0x10	/* F2 seen (REPNE)			*/
#define IN_LOCK		0x20	/* F0 seen; we are uniprocessor, so a	*/
				/* no-op, but it must not be lost	*/
#define IN_IMM		0x40	/* .imm holds an immediate operand	*/
#define IN_DIR		0x80	/* the `d' bit: reg is the destination	*/

/* One decoded instruction.  Nothing here depends on register contents,
 * so a decode is reusable and a decode cache is possible later; the
 * effective address is formed at execute time by i86ea(). */
struct i86in {
	i8	len;		/* total length, prefixes included	*/
	i8	op;		/* I_*					*/
	i8	fl;		/* IN_*					*/
	i8	w;		/* 0 = byte operand, 1 = word		*/
	i8	x;		/* per-op variant: see the I_* list	*/
	i8	mod, reg, rm;
	i8	seg;		/* S_*: segment slot for the memory operand */
	i16	disp;		/* displacement, or a branch target offset */
	i16	imm;		/* immediate, already sign-extended	*/
	i16	imm2;		/* far target segment			*/
};

/* i86dec: decode the instruction at cs[ip].  Fills *in and returns its
 * length, or 0 with in->op == I_BAD when the bytes are not an 8086
 * instruction.  Never reads past the instruction it decodes. */
extern int i86dec();

/* ------------------------------------------------------------------ */
/* executor							       */

#define X_OK		0	/* executed; IP advanced		*/
#define X_UNIMP		1	/* decoded, not implemented -- refuse	*/
#define X_BAD		2	/* not an instruction -- refuse		*/
#define X_INT		3	/* an INT the caller must service; the	*/
				/* vector is left in i86intno		*/
#define X_HALT		4	/* HLT					*/
#define X_SEGESC	5	/* a segment-register write named a	*/
				/* paragraph the slow path could not	*/
				/* cover either				*/
#define X_WINDOW	6	/* a reference through a slow-path	*/
				/* segment ran past the end of the host	*/
				/* segment covering it -- see setsr()	*/
#define X_WBOOT		7	/* the guest warm booted: a far transfer	*/
				/* to <entry SS>:0000.  NOT a refusal --	*/
				/* it is CP/M-80's `JMP 0000' in 8086	*/
				/* spelling, and it terminates the guest	*/
				/* exactly as BDOS function 0 does.	*/
				/* See i86exec.c wboot() for why it is	*/
				/* an environment rule and why nothing	*/
				/* but a warm boot can reach it.	*/

extern int i86step();		/* decode + execute one instruction	*/
extern int i86run();		/* i86step() up to i86nrun times	*/
extern int i86runa();		/* i86run() in assembly, on the target	*/
extern i16 i86nrun;
extern i16 i86flags();		/* materialise and return FLAGS		*/
extern int i86cond();		/* (FLAGS, cc 0..15) -> 0 or 1		*/
extern int i86lcond();		/* i86cond() on the pending record	*/
extern int i86intno;		/* vector left by X_INT			*/
extern i16 i86segbad;		/* paragraph that caused X_SEGESC	*/

/* Segment-slot resolution.  The shim hands out a small set of 64 KB
 * paragraphs and every segment-register write is checked against it --
 * on the WRITE, never on the instruction that computed the value, since
 * WordStar stashes an absolute segment in a variable first.  Returns the
 * host base for a known paragraph, or 0. */
extern char *i86resolve();

/* The assigned set itself, filled by whoever acquires the segments.
 * Nine: the eight groups a .CMD header can declare, and paragraph 0.
 *
 * PARAGRAPH 0 is a segment like any other here, entered in this set by
 * whoever acquires it, and it is how the guest reaches the interrupt
 * vector table -- DDT86 plants a handler in it and reads one back with
 * ordinary MOVSB through a zeroed DS and ES.  Registering it also makes
 * the low 64 KB addressable paragraph by paragraph, through setsr()'s
 * interior window, which is what the low paragraphs have always meant.
 * A caller that registers nothing at paragraph 0 has no vector table
 * and no guest handlers: see i86exec.c takeint(). */
#define I86NSEG	9
extern i16 i86spar[];		/* guest paragraph			*/
extern char *i86sbase[];	/* the host segment it names		*/
extern int i86nseg;
extern i32 i86nsegslow;		/* slow-path resolutions		*/
extern i32 i86nsegbad;		/* ... and the ones it could not cover	*/

/* Optional hook for resolving an unowned paragraph. Returns a host base
 * for a covering 64 KB segment, or zero to refuse; it may cache the result
 * in i86spar/i86sbase. Null means no allocation policy is installed. */
extern char *(*i86segnew)();

/* Spare 64 KB segments, for the program a guest loads with BDOS function
 * 59.  i86segget() answers a host base or zero when the pool is empty;
 * i86segput() hands one back.  Only the platform knows where segments
 * come from -- BIOS function 25 on the target, static arrays in the host
 * tests -- so the seam asks through these and refuses when neither is
 * installed.  They are NOT i86segnew: that one is asked about a paragraph
 * the guest named, this one about a segment nobody has named yet. */
extern char *(*i86segget)();
extern int (*i86segput)();

/* Host address of `len' guest bytes at slot:off, or 0 when they do not
 * all lie inside the host segment covering that slot.  The seam hands
 * FCBs and DMA buffers to the native BDOS by address, so this is the
 * check that stands between a guest offset near 0xFFFF and our BDOS
 * writing 128 bytes into the segment next door. */
extern char *i86addr();
extern char *i86far();		/* (m, par, off, len) a far pointer	*/

/* Instructions executed, and lazy records actually materialised.  Their
 * ratio is the flag-read rate. */
extern i32 i86ninsn, i86nflag;

/* Delay loops: a register DEC spinning on a JNZ to itself runs in one
 * step and sleeps its real time through i86wait(ticks).  i86nskip counts
 * the instructions that did not step; i86fast = 0 steps them all. */
extern int i86fast;
extern i32 i86nskip;
extern int (*i86wait)();
extern int i86sleep();		/* i86wait on the target		*/

extern char *i86mnem();		/* mnemonic of a decoded instruction	*/

/* ------------------------------------------------------------------ */
/* the INT 0E0h seam (i86bdos.c)					       */

/* What servicing an interrupt did.  Only B_RUN resumes the guest. */
#define B_RUN	0		/* serviced; carry on			*/
#define B_EXIT	1		/* function 0: the guest terminated	*/
#define B_FN	2		/* a function the shim does not map	*/
#define B_ADDR	3		/* its parameter left the guest segment	*/
#define B_SEG	4		/* a DMA base we never handed out	*/
#define B_VEC	5		/* an interrupt with no guest handler	*/
#define B_TRAP	6		/* vector 0: the guest divided by zero	*/

extern int i86bdos();		/* service the pending i86intno		*/
extern int i86bdosinit();	/* the DMA address a program starts with	*/
extern int i86bdosfini();	/* put the console mode back		*/
extern int i86oflush();		/* send collected fn 2 output as one fn 111 */
extern char *i86berr();		/* one sentence about the last refusal	*/
extern int i86bdosfn;		/* the function that asked, or -1	*/
extern int i86gsx();		/* GSX-86, function 73h			*/
extern int (*i86gsxdev)();	/* its device, or null for none		*/
extern int i86gsxb();		/* the machine's device			*/
extern i32 i86nbdos;		/* calls serviced			*/
extern i16 i86dmaoff, i86dmaseg;
extern i16 i86ver;		/* what function 12 tells the guest	*/
extern int i86ranbe;		/* native random record high byte first	*/

/*
 * The one thing i86bdos.c does not contain: the native BDOS call.
 *
 *	int i86sys(fn, val, addr)
 *	int fn;  i16 val;  char *addr;
 *
 * `addr' is a HOST pointer into a guest segment when the function takes
 * an address and 0 otherwise, in which case `val' is the byte or word
 * parameter.  The target's main() is one line -- our BDOS takes a LONG
 * whose value is the XADDR, which is what a far pointer already is on
 * this pipeline -- and the host tests supply a stub, so the whole
 * mapping can be exercised without a toolchain.
 */
extern int i86sys();

/* ------------------------------------------------------------------ */
/* .CMD loader							       */

#define CMD_NGRP	8	/* group descriptors in the 128-byte header */
#define CMD_HDR		128
#define CMD_PARA	16	/* bytes per paragraph			*/
#define CMD_MAXPAR	4096	/* paragraphs in one 64 KB host segment	*/

/* G-Form values. */
#define G_NONE		0
#define G_CODE		1
#define G_DATA		2
#define G_EXTRA		3
#define G_STACK		4
#define G_AUX1		5
#define G_AUX2		6
#define G_AUX3		7
#define G_AUX4		8
#define G_SHCODE	9	/* shared code -- CP/M-86 only, refused	*/

/* Group descriptors determine the memory model (DRI System Guide 3.4).
 * Code-only shares CS/DS/ES/SS and enters at 0x100; other models enter at
 * zero. DATA supplies DS, EXTRA ES, STACK SS; auxiliary groups are listed
 * in the base page and have no initial segment register. */
#define M_8080		0	/* one group; CS = DS = ES = SS; IP = 0x100 */
#define M_SMALL		1	/* code + data; IP = 0; base page at DS:0 */
#define M_COMPACT	2	/* + extra and/or stack, each its own	*/
#define M_LARGE		3	/* + one or more auxiliary groups	*/

struct i86grp {
	i8	form;		/* G_*					*/
	i16	len;		/* G-Length: paragraphs SUPPLIED in the file */
	i16	base;		/* A-Base: nonzero means non-relocatable */
	i16	min;		/* G-Min:  paragraphs the group NEEDS	*/
	i16	max;		/* G-Max:  paragraphs it will accept; 0	*/
				/* is not a bound, it is "all you have"	*/
	i32	foff;		/* byte offset of its image in the file	*/
	i16	npar;		/* paragraphs we will allocate for it	*/
	i8	seg;		/* S_* slot it was placed in, or S_NONE	*/
	i8	sidx;		/* its index in i86spar[]/i86sbase[] --	*/
				/* the ONLY handle an aux group has, and	*/
				/* what the caller copies its image with	*/
	i16	par;		/* the guest paragraph it was given	*/
};

struct i86cmd {
	struct i86grp g[CMD_NGRP];
	int	ng;		/* descriptors with a nonzero form	*/
	int	model;		/* M_8080 or M_SMALL			*/
	i32	need;		/* total bytes the image occupies in the file */
	i16	entry;		/* initial IP				*/
};

/* i86hdr() return values, every one a loud refusal.  CE_BASE and CE_BIG
 * never fire on real .CMD files, but the alternative when they do is
 * silent corruption. */
#define CE_OK		0
#define CE_NOCODE	1	/* no code group			*/
#define CE_BASE		2	/* A-Base nonzero -- not relocatable	*/
#define CE_BIG		3	/* a group wants over 64 KB		*/
#define CE_FORM		4	/* a group form we do not place		*/
#define CE_TRUNC	5	/* the file is shorter than its header	*/
#define CE_EMPTY	6	/* a group that needs nothing at all	*/
#define CE_DUP		7	/* two descriptors with the same form	*/
#define CE_NSEG		8	/* more groups than the caller has 64 KB	*/
				/* segments to put them in.  Not a	*/
				/* property of the file: it is what	*/
				/* THIS machine's segment pool can do --	*/
				/* seven segments, one of them staging,	*/
				/* so six groups.  See i86place().	*/

/* The group that holds the base page and the stack, and the first offset
 * in it the guest was NOT given: its allocation is i86dgtop bytes and its
 * stack starts at the top of that, so everything above belongs to the
 * shim.  i86place() decides both, because i86place() is what grows a
 * group to the paragraph count its descriptor asked for; the BDOS seam
 * puts the blocks functions 27 and 31 answer with up there. */
extern i16 i86dgpar;
extern i32 i86dgtop;

extern int i86hdr();		/* parse + validate a 128-byte header	*/
extern int i86place();		/* bind groups to segment slots		*/
extern char *i86cerr();		/* the refusal text for a CE_* code	*/
extern i32 i86have();		/* image bytes a group's file holds	*/
extern int i86bpage();		/* build the 256-byte base page		*/
extern int segclr();		/* zero the 64 KB segment at p		*/
