/* CP/M-86 INT 0xe0 bridge: CL selects the function, DX the parameter;
 * return results in AX and BX. Translate guest pointers and random-record
 * byte order for native BDOS calls. */

#include "i86.h"
#include "gdpb.h"
#include "conmode.h"

/* ------------------------------------------------------------------ */
/* what the seam reports to its caller				       */

int	i86bdosfn;		/* function of the last call, for a message */
i32	i86nbdos;		/* calls serviced			*/

i16	i86dmaoff;		/* the guest's DMA offset (fn 26)	*/
i16	i86dmaseg;		/* ... and base paragraph (fn 51)	*/

/*
 * The multi-sector count the native BDOS is currently holding.
 *
 * It is mirrored here because it is HALF of every DMA bound: our BDOS's
 * multio() (src/bdos/bdosrw.c:342) loops the count and adds SECLEN to
 * the DMA address between records, so the window a transfer touches is
 * count * 128 bytes and not the 128 a single record needs.  Function 49
 * is refused here (pmap[] below), so function 44 is the only door a
 * CP/M-86 guest has to it, and bdosmisc.c:171 resets it to one per
 * program -- which is what i86bdosinit() does.
 */
static int	i86mult = 1;

/* Collect consecutive function-2 output into native function 111 blocks.
 * Flush before other calls and when the interpreter stops. */
#define I86OBUF	128		/* pending function 2 characters	*/

static char	obuf[I86OBUF];
static int	onbuf;
static struct {			/* the native CCB for the batch		*/
	char	*a;
	i16	n;
} octl;

/*
 * i86oflush -- send whatever function 2 has collected, as one function
 * 111.  A no-op when there is nothing pending, which is why it can be
 * called on every path without a test at the call site.
 *
 * `octl.a' is a host pointer, and on the target that IS the XADDR our
 * BDOS reads out of a character control block (src/cmd/cpm.h:5-13); the
 * buffer is ours, not the guest's, so there is no segment check to make
 * -- which is also why this is the one thing here that does not go
 * through i86addr().
 *
 * PUBLIC because the seam is not the only way a run can end: the
 * caller's loop also stops on a bad instruction, a HLT or its own step
 * limit, and none of those comes through here.  Every caller calls it
 * once when its loop ends, so a guest that crashes mid-line still gets
 * the line it had written.
 */
int i86oflush()
{
	register int n;

	if ((n = onbuf) == 0)
		return (0);
	onbuf = 0;
	octl.a = obuf;
	octl.n = (i16)n;
	i86sys(111, (i16)0, (char *)&octl);
	return (n);
}

/* Sleep n ticks with BDOS function 141, the guest's output shown first. */
int i86sleep(n)
int n;
{
	i86oflush();
	return (i86sys(141, (i16)n, (char *)0));
}

/* Report CP/M 2.2 compatibility to avoid CP/M 3-specific FCB attributes
 * in CP/M-86 guests.
 *
 * BOTH bytes matter, and for different reasons.  The low byte is 0x22
 * because our BDOS's own 0x31 made our PIP silently truncate a copy.
 * The high byte is ZERO because a guest reads it as the machine type:
 * DRI's TOD.CMD accepts
 * 0x0022 and refuses 0x2022, 0x0122, 0x1422 and 0x0031 outright, so a
 * non-zero high byte is not a harmless decoration here.
 */
i16	i86ver = 0x0022;

/* ------------------------------------------------------------------ */
/* parameter classes						       */

#define P_NONE	0		/* no parameter				*/
#define P_BYTE	1		/* DL					*/
#define P_WORD	2		/* DX, a value				*/
#define P_FCB	3		/* DS:DX, a 36-byte FCB			*/
#define P_STR	4		/* DS:DX, a `$'-terminated string	*/
#define P_BUF	5		/* DS:DX, a console read buffer		*/
#define P_NO	6		/* not mapped				*/
#define P_DPB	7		/* fns 27 and 31: the answer is an address */

#define I86FCB	36		/* sizeof(struct fcb)			*/
#define I86REN	52		/* fn 23: old FCB at 0, new at 16	*/
#define I86SEQ	33		/* an FCB without the random record	*/
#define I86DMA	128		/* one CP/M record			*/

/* Parameter classes for supported calls. Refuse native-pointer APIs and
 * environment operations without a guest representation. */
static i8 pmap[53] = {
	P_NONE,		/*  0 system reset -- handled before this table	*/
	P_NONE,		/*  1 console input				*/
	P_BYTE,		/*  2 console output -- collected, never reached	*/
	P_NONE,		/*  3 reader input				*/
	P_BYTE,		/*  4 punch output				*/
	P_BYTE,		/*  5 list output				*/
	P_BYTE,		/*  6 direct console i/o -- handled before this table */
	P_NONE,		/*  7 get i/o byte				*/
	P_BYTE,		/*  8 set i/o byte				*/
	P_STR,		/*  9 print string				*/
	P_BUF,		/* 10 read console buffer			*/
	P_NONE,		/* 11 console status				*/
	P_NONE,		/* 12 version -- handled before this table	*/
	P_NONE,		/* 13 reset disk system				*/
	P_BYTE,		/* 14 select disk				*/
	P_FCB,		/* 15 open file					*/
	P_FCB,		/* 16 close file					*/
	P_FCB,		/* 17 search first				*/
	P_FCB,		/* 18 search next				*/
	P_FCB,		/* 19 delete file				*/
	P_FCB,		/* 20 read sequential				*/
	P_FCB,		/* 21 write sequential				*/
	P_FCB,		/* 22 make file					*/
	P_FCB,		/* 23 rename file -- I86REN bytes, see below	*/
	P_NONE,		/* 24 login vector				*/
	P_NONE,		/* 25 current disk				*/
	P_WORD,		/* 26 set DMA offset -- handled before this table */
	P_DPB,		/* 27 get addr(alloc)				*/
	P_NONE,		/* 28 write protect disk				*/
	P_NONE,		/* 29 get read-only vector			*/
	P_FCB,		/* 30 set file attributes				*/
	P_DPB,		/* 31 get addr(disk parms)			*/
	P_BYTE,		/* 32 get/set user code				*/
	P_FCB,		/* 33 read random				*/
	P_FCB,		/* 34 write random				*/
	P_FCB,		/* 35 compute file size				*/
	P_FCB,		/* 36 set random record				*/
	P_WORD,		/* 37 reset drive				*/
	P_NO,		/* 38 MP/M access drive				*/
	P_NO,		/* 39 MP/M free drive				*/
	P_FCB,		/* 40 write random with zero fill		*/
	P_NO,		/* 41 does not exist				*/
	P_FCB,		/* 42 lock record				*/
	P_FCB,		/* 43 unlock record				*/
	P_BYTE,		/* 44 set multi-sector count			*/
	P_BYTE,		/* 45 set BDOS error mode -- PIP's fourth call	*/
	P_BYTE,		/* 46 get disk free space -> the DMA buffer	*/
	P_NO,		/* 47 chain to program				*/
	P_NONE,		/* 48 flush buffers				*/
	P_NO,		/* 49 get system data -- handled before this table */
	P_NO,		/* 50 direct BIOS call -- handled before this table */
	P_WORD,		/* 51 set DMA base -- handled before this table	*/
	P_NO		/* 52 get DMA base				*/
};

/* Reason codes for the refusals, so a caller can print one sentence. */
#define BR_NONE	0
#define BR_FN	1		/* a function the shim does not map	*/
#define BR_ADDR	2		/* the parameter left its segment	*/
#define BR_SEG	3		/* the DMA base is a paragraph we lack	*/
#define BR_VEC	4		/* an interrupt with no handler and no seam */
#define BR_DIV	5		/* vector 0: the guest divided by zero	*/

static int breason;

char *i86berr()
{
	switch (breason) {
	case BR_NONE:	return ("ok");
	case BR_FN:	return ("BDOS function not mapped in stage one");
	case BR_ADDR:	return ("parameter runs past the end of its segment");
	case BR_SEG:	return ("DMA base is a paragraph we did not assign");
	case BR_VEC:	return ("interrupt with no handler and no seam");
	case BR_DIV:	return ("divide error (INT 0)");
	}
	return ("unknown");
}

/* ------------------------------------------------------------------ */

/*
 * How many bytes are still addressable at slot:off.  A slot the loader
 * bound has the full 64 KB from its base; one E1s's slow path had to
 * bias has less, and the difference is exactly the bias (i86exec.c).
 */
static i32 room(m, s, off)
struct i86 *m;
int s;
i16 off;
{
	return (0x10000L - ((i32)(m->so[s & 3] & 0xffff) + (i32)(off & 0xffff)));
}

/*
 * Re-issue the native set-DMA with whatever the guest's two halves now
 * say.  Called from load time and from functions 26 and 51, so there is
 * one place where a DMA address is formed and one place where it is
 * checked.
 */
static int setdma(m)
struct i86 *m;
{
	register char *p;
	register int i;

	/* The base is a paragraph, and it is subject to exactly the check
	 * a segment-register write is subject to: we can only form an
	 * address inside a segment we hold.  A guest DMA base we never
	 * handed out is the same failure as a bad segment-register write,
	 * arriving through a different door, and is reported as one. */
	p = (char *)0;
	for (i = 0; i < i86nseg; i++) {
		if (i86dmaseg == i86spar[i]) {
			p = i86sbase[i];
			break;
		}
	}
	if (p == (char *)0) {
		i86segbad = i86dmaseg;
		breason = BR_SEG;
		return (0);
	}
	if ((i32)(i86dmaoff & 0xffff) + (i32)I86DMA > 0x10000L) {
		breason = BR_ADDR;
		return (0);
	}
	i86sys(26, (i16)0, p + (i86dmaoff & 0xffff));
	return (1);
}

/*
 * dmafits -- does the DMA window hold the whole of the transfer the
 * native BDOS is about to make?
 *
 * setdma() above checks ONE record, because that is what a DMA address
 * has to be good for to be a DMA address at all, and a guest is
 * entitled to place a high one while the count is large and then lower
 * the count before any I/O.  This is the check that goes at the five
 * functions multio() shells: they are the only calls whose length is
 * the count rather than a record.
 */
static int dmafits()
{
	return ((i32)(i86dmaoff & 0xffff) + (i32)i86mult * (i32)I86DMA
		<= 0x10000L);
}

/*
 * Where the blocks functions 27 and 31 answer with live: the TOP of the
 * group that holds the guest's base page and stack.  The disk parameter
 * block takes the last 32 bytes of the segment, so its offset is the
 * same on every drive and a guest may keep it; the allocation vector,
 * whose length is the drive's, is laid out below it.
 */
#define I86DPBOFF	0xffe0L

/*
 * dparms -- functions 27 and 31, whose answer is an ADDRESS.
 *
 * Our BDOS never hands one out: function 31 copies the disk parameter
 * block to a buffer the caller names, and function 27 does the same with
 * the allocation vector, because neither structure is addressable from a
 * transient program.  So the buffer is one inside the guest, and `*offp'
 * is the offset the guest is given, with i86dgpar for the segment half.
 *
 * It goes ABOVE the group's own allocation.  i86place() grew that group
 * to the paragraph count its descriptor asked for, the base page states
 * that count and the stack starts at the top of it, so a guest that
 * keeps inside the memory it was given cannot reach these bytes.  A
 * guest whose group filled the whole 64 KB leaves no such room and is
 * refused, which is what it got before.
 */
static int dparms(m, fn, offp)
struct i86 *m;
int fn;
i16 *offp;
{
	struct gdpb d;
	char *b;
	i32 off;

	b = i86resolve(i86dgpar);
	if (b == (char *)0)
		return (0);
	i86sys(31, (i16)0, (char *)&d);
	if (fn == 31) {
		if (I86DPBOFF < i86dgtop)
			return (0);
		gdpbpack(&d, b + I86DPBOFF);
		*offp = (i16)I86DPBOFF;
		return (1);
	}
	off = I86DPBOFF - gdpbalv(&d);
	if (off < i86dgtop)
		return (0);
	i86sys(27, (i16)0, b + off);
	*offp = (i16)off;
	return (1);
}

/* ------------------------------------------------------------------ */
/* functions 57 and 59: the program a guest loads			       */

/*
 * The whole of CP/M-86's memory group that DDT86 reaches for, and no
 * more.  Function 59 is a .CMD load with the GUEST's FCB, answered with
 * the paragraph of the base page it built; function 57 gives the
 * segments back.
 *
 * One loaded program at a time.  That is not a simplification of
 * CP/M-86 so much as a statement of what the pool can hold: seven
 * segments on the machine, of which a run already holds one per group
 * plus paragraph 0, and a debuggee wants one per group again.  A second
 * function 59 therefore frees the first program before it loads
 * anything, so an `E' command repeated cannot leak a segment however the
 * guest keeps its own books.
 */
char	*(*i86segget)();
int	(*i86segput)();

static int	plfirst;		/* first pool slot the program holds */
static int	plnseg;			/* how many; 0 when none is loaded */
static i16	plpar[CMD_NGRP];	/* the paragraphs, for the MCB	*/
static char	*plmem[CMD_NGRP];	/* and the segments behind them	*/
static i16	plbase;			/* its base page paragraph	*/

/*
 * plfree -- the segments back to the platform and the slots off the end
 * of the pool.  The slots ARE off the end: nothing else appends to
 * i86spar[] while a guest runs, so the program's are the last ones and
 * dropping i86nseg back is enough to make them unaddressable again.
 */
static int plfree()
{
	register int i;

	if (plnseg == 0)
		return (0);
	if (i86segput)
		for (i = plnseg - 1; i >= 0; i--)
			(*i86segput)(plmem[i]);
	if (i86nseg == plfirst + plnseg)
		i86nseg = plfirst;
	plnseg = 0;
	plbase = 0;
	return (1);
}

/* Is `par' a paragraph the loaded program was given?  Zero is CP/M-86's
 * "all of it", which a guest with nothing loaded may well ask for. */
static int plowns(par)
i16 par;
{
	register int i;

	if (plnseg == 0)
		return (0);
	if (par == 0 || par == plbase)
		return (1);
	for (i = 0; i < plnseg; i++)
		if (par == plpar[i])
			return (1);
	return (0);
}

/*
 * pload -- function 59, with `f' the guest's FCB.  Answers the base page
 * paragraph, or zero, having left nothing acquired.
 *
 * The file is read a record at a time into a buffer of our own and
 * scattered into the groups by file offset, rather than staged in a
 * whole spare segment the way the gate stages a program at startup: the
 * gate had a segment to spare before the guest existed and this does
 * not.  Record 0 is the header, which is exactly 128 bytes.
 */
static i16 pload(m, f)
struct i86 *m;
char *f;
{
	static char fcb[I86FCB];
	static char rec[I86DMA];
	static char hdr[CMD_HDR];
	static struct i86cmd c;
	static struct i86 pm;
	register struct i86grp *g;
	char *savbase[I86NSEG];
	i16 savpar[I86NSEG];
	char *base[CMD_NGRP];
	i16 par[CMD_NGRP], sdgpar, mx, bpar;
	i32 sdgtop, flen, off, lo, hi, k;
	int savnseg, i, rc, ng, slot;

	plfree();
	bpar = 0;
	for (i = 0; i < I86FCB; i++)
		fcb[i] = f[i];
	fcb[12] = fcb[13] = fcb[14] = fcb[15] = 0;
	fcb[32] = 0;
	/* The length first, because i86hdr() checks the header against it
	 * and a truncated .CMD is the one thing a directory can hand us
	 * that looks like a program and is not.  Our BDOS leaves the
	 * record count in the random field high byte first. */
	if ((i86sys(35, (i16)0, fcb) & 0xff) == 0xff)
		return (0);
	if (i86ranbe)
		flen = ((i32)(fcb[33] & 0xff) << 16) | ((i32)(fcb[34] & 0xff) << 8)
			| (i32)(fcb[35] & 0xff);
	else
		flen = ((i32)(fcb[35] & 0xff) << 16) | ((i32)(fcb[34] & 0xff) << 8)
			| (i32)(fcb[33] & 0xff);
	flen *= (i32)I86DMA;
	fcb[12] = fcb[13] = fcb[14] = fcb[15] = 0;
	fcb[32] = 0;
	fcb[33] = fcb[34] = fcb[35] = 0;
	if ((i86sys(15, (i16)0, fcb) & 0xff) == 0xff)
		return (0);

	/* Our reads are single records, whatever count the guest set with
	 * function 44: multio() would otherwise write count * 128 bytes
	 * into a 128-byte buffer. */
	i86sys(44, (i16)1, (char *)0);
	i86sys(26, (i16)0, rec);
	rc = CE_TRUNC;
	if (i86sys(20, (i16)0, fcb) == 0) {
		for (i = 0; i < CMD_HDR; i++)
			hdr[i] = rec[i];
		rc = i86hdr(hdr, flen, &c);
	}
	if (rc != CE_OK)
		goto done;

	ng = c.ng < 1 ? 1 : c.ng;
	if (ng > CMD_NGRP || i86nseg + ng > I86NSEG || i86segget == 0)
		goto done;
	/* The next free paragraphs, one 64 KB apart from the last segment
	 * the pool holds, so that a guest that computes one segment value
	 * from another still lands on a paragraph i86resolve() knows. */
	mx = 0;
	for (i = 0; i < i86nseg; i++)
		if (i86spar[i] > mx)
			mx = i86spar[i];
	if ((i32)mx + (i32)ng * 0x1000L > 0xf000L)
		goto done;
	for (i = 0; i < ng; i++) {
		par[i] = (i16)(mx + (i16)((i + 1) * 0x1000));
		base[i] = (*i86segget)();
		if (base[i] == (char *)0) {
			while (--i >= 0)
				if (i86segput)
					(*i86segput)(base[i]);
			goto done;
		}
	}

	/*
	 * i86place() places into i86spar[0..nseg-1], so the program's
	 * segments are made to BE those slots for the length of the call
	 * and the running guest's pool is put back afterwards.  The same
	 * swap covers i86dgpar/i86dgtop, which name the group functions 27
	 * and 31 answer out of and belong to the guest, not to its
	 * debuggee.
	 */
	savnseg = i86nseg;
	sdgpar = i86dgpar;
	sdgtop = i86dgtop;
	for (i = 0; i < I86NSEG; i++) {
		savpar[i] = i86spar[i];
		savbase[i] = i86sbase[i];
	}
	for (i = 0; i < ng; i++) {
		i86spar[i] = par[i];
		i86sbase[i] = base[i];
		segclr(base[i]);
	}
	i86nseg = ng;
	for (i = 0; i < (int)sizeof pm; i++)
		((char *)&pm)[i] = 0;
	pm.fl = F_ONES;
	pm.lz = LZ_NONE;
	rc = i86place(&c, &pm, ng);
	if (rc == CE_OK) {
		/* The images, out of the file and into the groups.  The
		 * tail is empty: function 59 loads a program, and filling
		 * the base page's tail and default FCBs is the caller's
		 * to do -- DDT86 does it for the program it debugs. */
		off = CMD_HDR;
		while (off < c.need && i86sys(20, (i16)0, fcb) == 0) {
			for (i = 0; i < CMD_NGRP; i++) {
				g = &c.g[i];
				if (g->form == G_NONE || g->form > G_AUX4)
					continue;
				lo = g->foff > off ? g->foff : off;
				hi = g->foff + (i32)g->len * (i32)CMD_PARA;
				if (hi > off + (i32)I86DMA)
					hi = off + (i32)I86DMA;
				for (k = lo; k < hi; k++)
					base[g->sidx][k - g->foff]
						= rec[k - off];
			}
			off += (i32)I86DMA;
		}
		slot = c.model == M_8080 ? S_CS : S_DS;
		i86bpage(&c, &pm, slot, (char *)0);
		bpar = i86dgpar;
	}

	i86dgpar = sdgpar;
	i86dgtop = sdgtop;
	for (i = 0; i < I86NSEG; i++) {
		i86spar[i] = savpar[i];
		i86sbase[i] = savbase[i];
	}
	i86nseg = savnseg;
	if (bpar == 0) {
		for (i = ng - 1; i >= 0; i--)
			if (i86segput)
				(*i86segput)(base[i]);
		goto done;
	}
	/* Appended, never inserted: paragraph 0 keeps its place ahead of
	 * them, so a paragraph inside a group still resolves to that
	 * group, and plfree() can drop them by shortening the pool. */
	plfirst = savnseg;
	for (i = 0; i < ng; i++) {
		i86spar[savnseg + i] = par[i];
		i86sbase[savnseg + i] = base[i];
		plpar[i] = par[i];
		plmem[i] = base[i];
	}
	i86nseg = savnseg + ng;
	plnseg = ng;
	plbase = bpar;
done:
	i86sys(16, (i16)0, fcb);
	i86sys(44, (i16)i86mult, (char *)0);
	setdma(m);
	return (bpar);
}

/* ------------------------------------------------------------------ */
/* function 49: the system data block				       */

/*
 * CP/M-86 1.1 answers function 49 with ES:BX at the BIOS's system data.
 * Guests read two things there: the clock, kept as ASCII that the BIOS
 * rewrites every tick and TOD rewrites to set the time, and the console
 * width.  The block lives in paragraph 0, above the vector table.
 *
 *	+20	"MM/DD/YY,HH:MM:SS"
 *	+40	console width, 80
 *
 * The clock is refilled from function 105 when the guest asks for the
 * block or polls the console; a string the guest changed is handed to
 * function 104 first.  Our clock keeps no seconds across a set.
 */
#define I86SDOFF	0x400		/* offset in paragraph 0	*/
#define I86SDTOD	0x20
#define I86SDLEN	0x50
#define I86TODLEN	17

static char	*sdat;			/* the block, once handed out	*/
static char	sdtod[I86TODLEN];	/* its clock as last written	*/
static char	mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int bcd(v)
int v;
{
	return (((v >> 4) & 0xf) * 10 + (v & 0xf));
}

static int dig2(p)
char *p;
{
	if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9')
		return (-1);
	return ((p[0] - '0') * 10 + p[1] - '0');
}

static void put2(p, v)
char *p;
int v;
{
	p[0] = (char)('0' + v / 10);
	p[1] = (char)('0' + v % 10);
}

/* Refill the clock string from function 105: day 1 is 1 January 1978. */
static void sdget()
{
	char t[4];
	int s, y, mo, n, k;
	long d;

	s = bcd(i86sys(105, (i16)0, t) & 0xff);
	d = (long)((t[0] & 0xff) | ((t[1] & 0xff) << 8));
	for (y = 1978; d > (k = (y % 4 == 0) ? 366 : 365); y++)
		d -= k;
	for (mo = 0; mo < 11; mo++) {
		n = mdays[mo] + (mo == 1 && y % 4 == 0);
		if (d <= n)
			break;
		d -= n;
	}
	put2(sdat + I86SDTOD, mo + 1);
	sdat[I86SDTOD + 2] = '/';
	put2(sdat + I86SDTOD + 3, (int)d);
	sdat[I86SDTOD + 5] = '/';
	put2(sdat + I86SDTOD + 6, y % 100);
	sdat[I86SDTOD + 8] = ',';
	put2(sdat + I86SDTOD + 9, bcd(t[2] & 0xff));
	sdat[I86SDTOD + 11] = ':';
	put2(sdat + I86SDTOD + 12, bcd(t[3] & 0xff));
	sdat[I86SDTOD + 14] = ':';
	put2(sdat + I86SDTOD + 15, s);
	for (k = 0; k < I86TODLEN; k++)
		sdtod[k] = sdat[I86SDTOD + k];
}

/* Hand a clock string the guest rewrote to function 104.  Years 78-99
 * are 19xx, the rest 20xx.  A malformed string sets nothing. */
static void sdput()
{
	char t[4], *p;
	int y, mo, dd, hh, mi, k;
	long d;

	p = sdat + I86SDTOD;
	mo = dig2(p);
	dd = dig2(p + 3);
	y = dig2(p + 6);
	hh = dig2(p + 9);
	mi = dig2(p + 12);
	if (mo < 1 || mo > 12 || dd < 1 || y < 0 || hh < 0 || hh > 23
	 || mi < 0 || mi > 59)
		return;
	y += y < 78 ? 2000 : 1900;
	if (dd > mdays[mo - 1] + (mo == 2 && y % 4 == 0))
		return;
	d = dd;
	for (k = 1978; k < y; k++)
		d += (k % 4 == 0) ? 366 : 365;
	for (k = 0; k < mo - 1; k++)
		d += mdays[k] + (k == 1 && y % 4 == 0);
	t[0] = (char)(d & 0xff);
	t[1] = (char)((d >> 8) & 0xff);
	t[2] = (char)((hh / 10) << 4 | hh % 10);
	t[3] = (char)((mi / 10) << 4 | mi % 10);
	i86sys(104, (i16)0, t);
}

/* Called on every seam entry; `fn' is the function being made. */
static void sdsync(fn)
int fn;
{
	int k, moved;

	if (sdat == (char *)0)
		return;
	moved = 0;
	for (k = 0; k < I86TODLEN; k++)
		if (sdat[I86SDTOD + k] != sdtod[k])
			moved = 1;
	if (moved)
		sdput();
	if (moved || fn == 11)
		sdget();
}

static char	fbuf[I86REN];		/* the FCB the native BDOS sees	*/
static char	sbuf[I86FCB];		/* ... and the one it searches with */

/* The functions that use the random record, bytes 33-35. */
static int isrand(fn)
int fn;
{
	return ((fn >= 33 && fn <= 36) || fn == 40);
}

/* The five functions src/bdos/bdosrw.c multio() shells. */
static int ismulti(fn)
int fn;
{
	return (fn == 20 || fn == 21 || fn == 33 || fn == 34 || fn == 40);
}

/* Function 6: E = FFh answers a key or 0 without waiting, FEh the
 * status, anything else is output.  Our FFh waits for a key. */
static int dconio(e)
int e;
{
	if (e != 0xfe && e != 0xff)
		return (i86sys(6, (i16)e, (char *)0));
	if ((i86sys(6, (i16)0xfe, (char *)0) & 0xff) == 0)
		return (0);
	if (e == 0xfe)
		return (0xff);
	return (i86sys(6, (i16)0xff, (char *)0) & 0xff);
}

/* Function 50's console vectors; the rest are refused. */
static int biosv(v, c, r)
int v, c;
int *r;
{
	switch (v) {
	case 2:				/* CONST			*/
		*r = i86sys(11, (i16)0, (char *)0) ? 0xff : 0;
		return (1);
	case 3:				/* CONIN			*/
		*r = i86sys(6, (i16)0xff, (char *)0) & 0xff;
		return (1);
	case 4:				/* CONOUT			*/
		i86sys(6, (i16)(c & 0xff), (char *)0);
		*r = 0;
		return (1);
	}
	return (0);
}

/* The console mode the CCP was running in, kept for the guest's exit. */
static int i86conmode = -1;

/*
 * i86bdosinit -- the state CP/M-86 gives a program before its first
 * instruction: the DMA buffer is the base page's own 128-byte tail
 * buffer, DS:0080.  Our BDOS has to be told, because the guest will
 * not tell it until it wants a different one, and PIP reads its command
 * tail through that buffer.
 *
 * Stop-scroll goes off for the run.  The native BDOS polls the console
 * every eight output characters and KEEPS what it finds: ^S and ^Q are
 * swallowed and anything else is held in one byte per console, so a
 * second key typed during a burst of output overwrites the first.  A
 * guest that does its own key handling loses keystrokes that way.  With
 * the poll off, every key stays in the BIOS until the guest asks for it.
 *
 * ^C is deliberately left terminating.  Nothing polls once the poll is
 * off, so the native line input is the only ^C left, and it is the only
 * way off a guest that never returns.
 */
int i86bdosinit(m)
struct i86 *m;
{
	i86conmode = i86sys(109, (i16)0xffff, (char *)0);
	i86sys(109, (i16)CM_NOSTOP, (char *)0);
	i86dmaseg = m->sr[S_DS];
	i86dmaoff = 0x80;
	i86mult = 1;		/* src/bdos/bdosmisc.c:171		*/
	plfree();
	sdat = (char *)0;
	i86bdosfn = -1;
	breason = BR_NONE;
	onbuf = 0;
	i86nbdos = 0;
	return (setdma(m));
}

/* Give the console back however the run ended: a guest must not leave a
 * mode behind for whatever the CCP starts next. */
int i86bdosfini()
{
	if (i86conmode < 0)
		return (0);
	i86sys(109, (i16)i86conmode, (char *)0);
	i86conmode = -1;
	return (1);
}

/*
 * i86bdos -- service the interrupt i86step() just refused to service.
 *
 * Returns B_RUN to resume the guest, B_EXIT when it has terminated, or
 * one of the refusals, with i86berr() naming it and i86bdosfn holding
 * the function that asked.  IP is already past the INT, so a resumed
 * guest continues at the instruction after it; a refusal leaves it
 * there too, because there is no sense in re-executing an INT whose
 * function we will refuse again.
 */
/* Guest FCB random records are little-endian; native BDOS uses big-endian
 * bytes at 33..35. Swap around relevant calls, excluding rename because
 * those bytes belong to its second name. */
int	i86ranbe = 1;	/* the native random record is high byte first	*/

static ranswap(p, fn)
char *p;
int fn;
{
	register int t;

	if (!i86ranbe)
		return (0);
	switch (fn) {
	case 33:	/* read random			*/
	case 34:	/* write random			*/
	case 35:	/* compute file size		*/
	case 36:	/* set random record		*/
	case 40:	/* write random with zero fill	*/
		t = p[33];
		p[33] = p[35];
		p[35] = (char)t;
	}
	return (0);
}

int i86bdos(m)
struct i86 *m;
{
	register int fn, cls;
	register char *p;
	i16 dx, dpboff;
	int r, k;
	i32 n;

	breason = BR_NONE;
	/* 0E1h as well as 0E0h.  DDT86 reads the vector byte out of its
	 * own INT 0E0h instruction, adds one, copies vector 0E0h to the
	 * vector above it and issues its own BDOS calls there, keeping
	 * 0E0h free for the handler it plants for the program under test.
	 * So the interrupt above the seam's IS the seam, for a guest that
	 * moved it, and answering it is what lets DDT86 talk at all. */
	if (i86intno != 0xe0 && i86intno != 0xe1) {
		/* Vector 0 is the divide error i86exec.c raises, and it
		 * is a fault in the guest rather than a hole in us; the
		 * distinction is worth keeping in the message. */
		breason = i86intno == 0 ? BR_DIV : BR_VEC;
		i86oflush();	/* a fault is a refusal somebody reads */
		i86bdosfn = -1;
		return (i86intno == 0 ? B_TRAP : B_VEC);
	}
	fn = (int)(m->r[R_CX] & 0xff);
	dx = m->r[R_DX];
	i86bdosfn = fn;
	i86nbdos++;
	sdsync(fn);

	/* ---- console output, collected rather than passed on. */

	if (fn == 2) {
		if (onbuf >= I86OBUF)
			i86oflush();
		obuf[onbuf++] = (char)(dx & 0xff);
		/* Our function 2 falls out of the switch to the default
		 * return value (src/bdos/bdosmain.c:239), so a deferred
		 * one answers what an immediate one would have: zero in
		 * both of CP/M-86's result places. */
		m->r[R_AX] = 0;
		m->r[R_BX] = 0;
		return (B_RUN);
	}
	i86oflush();		/* everything below is observable	*/

	/* ---- the three functions that never reach the native BDOS. */

	if (fn == 0) {
		/* System reset.  Our function 0 is warmboot() and does
		 * not return (src/bdos/bdosmain.c:234); the shim is an
		 * ordinary program and must return to ITS caller, so
		 * this is the one function the seam answers itself. */
		return (B_EXIT);
	}
	if (fn == 12) {
		m->r[R_AX] = i86ver;
		m->r[R_BX] = i86ver;
		return (B_RUN);
	}
	if (fn == 26 || fn == 51) {
		/* A REFUSED set-DMA must leave both halves holding what
		 * the native BDOS was actually told.  They used to be
		 * committed first and refused afterwards, so a guest
		 * whose DMA was rejected went on with a DMA nothing
		 * agreed about -- which the multi-sector bound below
		 * then reads. */
		i16 savoff, savseg;

		savoff = i86dmaoff;
		savseg = i86dmaseg;
		if (fn == 26)
			i86dmaoff = dx;
		else
			i86dmaseg = dx;
		if (!setdma(m)) {
			i86dmaoff = savoff;
			i86dmaseg = savseg;
			return (breason == BR_SEG ? B_SEG : B_ADDR);
		}
		m->r[R_AX] = 0;
		m->r[R_BX] = 0;
		return (B_RUN);
	}

	if (fn == 6) {
		r = dconio((int)(dx & 0xff));
		m->r[R_AX] = (i16)r;
		m->r[R_BX] = (i16)r;
		return (B_RUN);
	}
	if (fn == 50) {
		/* The parameter block: function byte, then CX and DX. */
		p = i86addr(m, S_DS, dx, 5L);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		if (!biosv(p[0] & 0xff, p[1] & 0xff, &r)) {
			breason = BR_FN;
			return (B_FN);
		}
		m->r[R_AX] = (i16)r;
		m->r[R_BX] = (i16)r;
		return (B_RUN);
	}
	if (fn == 49) {
		/* Get system data.  Without a paragraph 0 there is no
		 * block, and 0FFFFh is what DDT86 reads as "none". */
		p = i86resolve((i16)0);
		if (p == (char *)0) {
			m->r[R_AX] = (i16)0xffff;
			m->r[R_BX] = (i16)0xffff;
			return (B_RUN);
		}
		if (sdat == (char *)0) {
			sdat = p + I86SDOFF;
			for (n = 0; n < I86SDLEN; n++)
				sdat[n] = 0;
			sdat[0x40] = 80;
		}
		sdget();
		m->sr[S_ES] = 0;
		m->sb[S_ES] = p;
		m->so[S_ES] = 0;
		m->r[R_AX] = (i16)I86SDOFF;
		m->r[R_BX] = (i16)I86SDOFF;
		return (B_RUN);
	}
	if (fn == 57) {
		/* Free memory.  The MCB names a base paragraph, a length
		 * and an extent byte; the only memory this seam ever hands
		 * a guest is the program it loaded for it, so the base is
		 * what is read -- zero being CP/M-86's "all of it" -- and
		 * the length is not: the segments go back whole or not at
		 * all.  Freeing nothing is a success, which is what a
		 * guest that has loaded nothing yet gets. */
		p = i86addr(m, S_DS, dx, 5L);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		if (plowns((i16)((p[0] & 0xff) | ((p[1] & 0xff) << 8))))
			plfree();
		m->r[R_AX] = 0;
		m->r[R_BX] = 0;
		return (B_RUN);
	}
	if (fn == 59) {
		/* Program load.  The answer is the base page paragraph,
		 * and 0FFFFh is the failure DDT86 prints INSUFFICIENT
		 * MEMORY for -- which covers a file that is not there, a
		 * header we refuse and a pool with no segment left, all
		 * three being "you cannot have this program" to a guest. */
		p = i86addr(m, S_DS, dx, (i32)I86FCB);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		r = (int)pload(m, p);
		m->r[R_AX] = (i16)(r ? r : 0xffff);
		m->r[R_BX] = m->r[R_AX];
		return (B_RUN);
	}
	if (fn == 0x73) {
		r = i86gsx(m);
		breason = r == B_FN ? BR_FN : r == B_ADDR ? BR_ADDR : BR_NONE;
		return (r);
	}
	if (fn > 52) {
		/* 53-56 and 58 stay refused: sized allocation is quantised
		 * to a whole 64 KB segment here and there is nothing
		 * honest to answer a guest that asks for less. */
		breason = BR_FN;
		return (B_FN);
	}
	cls = pmap[fn];
	if (cls == P_NO) {
		breason = BR_FN;
		return (B_FN);
	}

	/* The DMA window has to hold the WHOLE transfer, not its first
	 * record: multio() writes count * 128 bytes starting at the DMA
	 * address, so a count of two at 0xff80 would put its second
	 * record outside the guest's segment altogether.  Refused here,
	 * before the native BDOS is told, because there is no partial
	 * transfer to undo afterwards. */
	if (ismulti(fn) && !dmafits()) {
		breason = BR_ADDR;
		return (B_ADDR);
	}

	switch (cls) {
	case P_NONE:
		r = i86sys(fn, (i16)0, (char *)0);
		/* Function 13 is DMA := 0080h, and the native BDOS moved
		 * its own DMA to ITS base page: without this the guest's
		 * next record lands there instead of in the guest.  The
		 * base paragraph is the guest's to choose, so only the
		 * offset goes back to the default. */
		if (fn == 13) {
			i86dmaoff = 0x80;
			if (!setdma(m))
				return (breason == BR_SEG ? B_SEG : B_ADDR);
		}
		break;
	case P_BYTE:
		r = i86sys(fn, (i16)(dx & 0xff), (char *)0);
		/* Function 44 is the only door a CP/M-86 guest has to the
		 * count; the native BDOS answers 0xff for a count it did
		 * not take (src/bdos/bdosmain.c:613) and leaves its own
		 * alone, so this follows it. */
		if (fn == 44 && r == 0)
			i86mult = (int)(dx & 0xff);
		break;
	case P_WORD:
		r = i86sys(fn, dx, (char *)0);
		break;
	case P_DPB:
		if (!dparms(m, fn, &dpboff)) {
			breason = BR_FN;
			return (B_FN);
		}
		/* CP/M-86 answers an address as ES:BX, so the segment
		 * register goes with the offset; the offset itself lands
		 * in BX through the ordinary result store below. */
		m->sr[S_ES] = i86dgpar;
		m->sb[S_ES] = i86resolve(i86dgpar);
		m->so[S_ES] = 0;
		r = (int)dpboff;
		break;
	case P_FCB:
		n = fn == 23 ? (i32)I86REN : (i32)I86FCB;
		p = i86addr(m, S_DS, dx, n);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		/* Search next uses the FCB search first was given, so
		 * that copy is kept until the next search first. */
		if (fn == 17 || fn == 18) {
			for (k = 0; fn == 17 && k < I86FCB; k++)
				sbuf[k] = p[k];
			r = i86sys(fn, (i16)0, sbuf);
			break;
		}
		/* The native BDOS writes back a whole 36-byte FCB, but a
		 * sequential call's FCB is 33 bytes and ASM86 keeps its
		 * read buffer right after one.  So the call works on a
		 * copy and only the bytes the function owns go back. */
		for (k = 0; k < (int)n; k++)
			fbuf[k] = p[k];
		ranswap(fbuf, fn);
		r = i86sys(fn, (i16)0, fbuf);
		ranswap(fbuf, fn);
		if (fn != 23 && !isrand(fn))
			n = I86SEQ;
		for (k = 0; k < (int)n; k++)
			p[k] = fbuf[k];
		break;
	case P_STR:
		/* Function 9's string ends at a `$' the guest put there.
		 * The native BDOS will scan for it; if there is none
		 * before the end of the guest's segment, that scan runs
		 * out of the segment, so the scan happens HERE first and
		 * the call is refused if it does not terminate. */
		p = i86addr(m, S_DS, dx, 1L);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		n = room(m, S_DS, dx);
		while (n > 0 && *p != '$') {
			p++;
			n--;
		}
		if (n <= 0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		r = i86sys(fn, (i16)0, i86addr(m, S_DS, dx, 1L));
		break;
	case P_BUF:
		/* Byte 0 of a console buffer is the maximum the caller
		 * will accept, so the buffer is that plus the two count
		 * bytes -- and the check has to use the guest's own
		 * number, not a guess. */
		p = i86addr(m, S_DS, dx, 1L);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		n = (i32)(*p & 0xff) + 2;
		p = i86addr(m, S_DS, dx, n);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		r = i86sys(fn, (i16)0, p);
		break;
	default:
		breason = BR_FN;
		return (B_FN);
	}

	/*
	 * The result, in the three places CP/M-86 leaves it: AL for a
	 * byte, AX for a word, BX equal to AX.  Our BDOS already returns
	 * CP/M 3's word form -- the high byte carries the physical error
	 * code when function 45 put the program in return mode
	 * (src/bdos/bdosmain.c:861-862) -- and CP/M-86 uses AH for exactly
	 * that, so the word passes straight through.
	 */
	m->r[R_AX] = (i16)r;
	m->r[R_BX] = (i16)r;
	return (B_RUN);
}
