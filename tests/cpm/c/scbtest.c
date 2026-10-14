/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * scbtest.c - Exercise system-control-block access and filename parsing.
 */

#include "cpm.h"

/* SCB offsets (sys/scb.h; CP/M 3 resbdos.asm:632-710) */
#define SCB_VERSION	0x05
#define SCB_ERRCDE	0x10
#define SCB_CONWIDTH	0x1a
#define SCB_CONMODE	0x33
#define SCB_OUTDELIM	0x37
#define SCB_SCBADD	0x3a
#define SCB_CRDMA	0x3c
#define SCB_CRDSK	0x3e
#define SCB_FX		0x43
#define SCB_USRCD	0x44
#define SCB_MLTIO	0x4a
#define SCB_ERMDE	0x4b
#define SCB_BFLGS	0x57
#define SCB_MXTPA	0x62

#define BDOS_SCB	49
#define BDOS_PARSE	152

static char	pb[4];		/* the function-49 parameter block	*/
static char	fcb[36];	/* the function-152 output FCB		*/
static long	pfcb[2];	/* the function-152 parameter block	*/
static char	buf[SECLEN];
static struct fcb openfcb;
static char	hexdig[] = "0123456789ABCDEF";
static int	checks;
static int	fails;

static VOID	puthex();
static VOID	report();

/* get the word at an SCB offset */
static int scbget(off)
int off;
{
	pb[0] = off;
	pb[1] = 0;
	pb[2] = 0;
	pb[3] = 0;
	return (__bdos(BDOS_SCB, (long) pb));
}

/* set one byte of the SCB */
static int scbsetb(off, val)
int off;
int val;
{
	pb[0] = off;
	pb[1] = 0xff;
	pb[2] = val;
	pb[3] = 0;
	return (__bdos(BDOS_SCB, (long) pb));
}

/* set one word of the SCB, low byte first as on the 8080 */
static int scbsetw(off, val)
int off;
unsigned val;
{
	pb[0] = off;
	pb[1] = 0xfe;
	pb[2] = val & 0xff;
	pb[3] = (val >> 8) & 0xff;
	return (__bdos(BDOS_SCB, (long) pb));
}

static VOID check(what, got, want)
char *what;
unsigned got;
unsigned want;
{
	checks++;
	cputs("\r\n");
	cputs(what);
	cputs(" -> ");
	puthex(got);
	if (got == want)
		cputs(" OK");
	else {
		cputs(" BAD, wanted ");
		puthex(want);
		fails++;
	}
}

/* run the parse and print what it made; returns the function value */
static long doparse(s)
char *s;
{
	long	r;
	int	i;

	for (i = 0; i < 36; i++)
		fcb[i] = 0;
	pfcb[0] = (long) s;
	pfcb[1] = (long) fcb;
	r = __bdosl(BDOS_PARSE, (long) pfcb);
	cputs("\r\nparse \"");
	cputs(s);
	cputs("\" drv=");
	putdec((unsigned) (fcb[0] & 0xff));
	cputs(" name=[");
	for (i = 1; i <= 8; i++)
		conout(fcb[i]);
	cputs("] type=[");
	for (i = 9; i <= 11; i++)
		conout(fcb[i]);
	cputs("] pw=[");
	for (i = 16; i <= 23; i++)
		conout(fcb[i]);
	cputs("] pwlen=");
	putdec((unsigned) (fcb[26] & 0xff));
	cputs(" ret=");
	puthex((unsigned) r);
	return (r);
}

/* is the FCB's name and type what we expect?  both blank padded */
static int nameis(name, type)
char *name;
char *type;
{
	int	i;

	for (i = 0; i < 8; i++)
		if (fcb[1 + i] != name[i])
			return (0);
	for (i = 0; i < 3; i++)
		if (fcb[9 + i] != type[i])
			return (0);
	return (1);
}

int main(argc, argv)
int argc;
char *argv[];
{
	unsigned	v;
	long		r;

	cputs("SCBTEST: system control block (fn 49) and parse (fn 152)");

	/* ---- gets of fixed fields ---- */
	check("version   (05)", (unsigned) (scbget(SCB_VERSION) & 0xff), 0x31);
	check("con width (1a)", (unsigned) (scbget(SCB_CONWIDTH) & 0xff), 79);
	check("bdos flgs (57)", (unsigned) (scbget(SCB_BFLGS) & 0xff), 0x80);

	/* the SCB has no address a program can use: zero */
	v = (unsigned) scbget(SCB_SCBADD);
	cputs("\r\nscb addr  (3a) -> ");
	puthex(v);
	checks++;
	if (v != 0) {
		cputs(" BAD, non-zero (unreachable address published)");
		fails++;
	} else
		cputs(" OK (zero: no addressable image, use fn 49)");

	/* ---- mirrors of live BDOS state ---- */
	/* current disk: fn 25 says which one, the SCB must agree */
	check("cur disk  (3e)", (unsigned) (scbget(SCB_CRDSK) & 0xff),
	      (unsigned) (__bdos(25, 0L) & 0xff));

	/* user number: change it with fn 32 and read it back from the SCB */
	__bdos(32, 3L);
	check("user code (44) after fn 32 set 3",
	      (unsigned) (scbget(SCB_USRCD) & 0xff), 3);
	__bdos(32, 0L);
	check("user code (44) after fn 32 set 0",
	      (unsigned) (scbget(SCB_USRCD) & 0xff), 0);

	/* DMA address: the low half of what fn 26 was given */
	setdma(buf);
	check("cur dma   (3c)", (unsigned) scbget(SCB_CRDMA),
	      (unsigned) ((long) buf & 0xffffL));

	/* the function in progress is function 49 itself, as in v3 */
	check("fx        (43)", (unsigned) (scbget(SCB_FX) & 0xff), 49);

	/* top of the TPA.  The field is the highest offset the TPA
	   contains (see sys/scb.c), so it must be at or above the top
	   the base page hands this program, which has the base page and
	   the stack above it. */
	v = (unsigned) scbget(SCB_MXTPA);
	check("max tpa   (62) >= base page htpa",
	      (unsigned) (v >= (unsigned) (_base->htpa & 0xffffL)), 1);
	cputs(" (");
	puthex(v);
	cputs(" vs ");
	puthex((unsigned) (_base->htpa & 0xffffL));
	cputs(")");

	/* ---- set byte (0FFh): the delimiter really changes fn 9 ---- */
	scbsetb(SCB_OUTDELIM, '#');
	check("delim     (37) after set byte '#'",
	      (unsigned) (scbget(SCB_OUTDELIM) & 0xff), '#');
	printstr("\r\nfn 9 now stops at a hash#");
	scbsetb(SCB_OUTDELIM, '$');
	printstr("  and at a dollar again$");
	check("delim     (37) restored",
	      (unsigned) (scbget(SCB_OUTDELIM) & 0xff), '$');

	/* ---- set word (0FEh): console mode, cross-checked with fn 109 ---- */
	scbsetw(SCB_CONMODE, 0x0102);
	check("conmode   (33) via fn 109",
	      (unsigned) __bdos(BDOS_CONMODE, 0xffffL), 0x0102);
	scbsetw(SCB_CONMODE, 0);
	check("conmode   (33) restored",
	      (unsigned) __bdos(BDOS_CONMODE, 0xffffL), 0);

	/* ---- both directions on the multi-sector count ---- */
	__bdos(BDOS_SETMULTI, 8L);
	check("multcnt   (4a) after fn 44 set 8",
	      (unsigned) (scbget(SCB_MLTIO) & 0xff), 8);
	scbsetb(SCB_MLTIO, 0);		/* out of range: clamped to 1 */
	check("multcnt   (4a) after set byte 0 (clamped)",
	      (unsigned) (scbget(SCB_MLTIO) & 0xff), 1);

	/* ---- return code: fn 108 one way, the SCB the other ---- */
	__bdos(BDOS_RETCODE, 0x55aaL);
	check("errcde    (10) after fn 108 set 55AA",
	      (unsigned) scbget(SCB_ERRCDE), 0x55aa);
	scbsetw(SCB_ERRCDE, 0x1234);
	check("errcde    (10) via fn 108",
	      (unsigned) __bdos(BDOS_RETCODE, 0xffffL), 0x1234);

	/* ---- error mode, set through the SCB and used ---- */
	scbsetb(SCB_ERMDE, 0xff);
	check("ermde     (4b) after set byte FF",
	      (unsigned) (scbget(SCB_ERMDE) & 0xff), 0xff);
	/* with errors returned, an FCB naming a drive that is not there
	   comes back 04FFh instead of ending the program */
	mkfcb("K:NOSUCH.TXT", &openfcb);
	check("open on K: returns the error",
	      (unsigned) __bdos(BDOS_OPEN, (long) &openfcb), 0x04ff);
	scbsetb(SCB_ERMDE, 0);

	/* ---- bounds: 99 and up is refused ---- */
	check("offset 99 refused", (unsigned) scbget(99), 0xffff);

	/* ---- function 152 ---- */
	r = doparse("b:test.txt");
	checks++;
	if (r == 0 && (fcb[0] & 0xff) == 2 && nameis("TEST    ", "TXT"))
		cputs(" OK");
	else {
		cputs(" BAD");
		fails++;
	}

	r = doparse("*.68k");
	checks++;
	if (r == 0 && (fcb[0] & 0xff) == 0 && nameis("????????", "68K"))
		cputs(" OK");
	else {
		cputs(" BAD");
		fails++;
	}

	/* leading blanks skipped, and the delimiter that stopped the scan
	   is reported by offset */
	r = doparse("  hello.c ,tail");
	checks++;
	if (r != 0 && r != 0xffffL && nameis("HELLO   ", "C  ")) {
		cputs(" OK, delimiter is '");
		conout((int) *(char *) r);
		cputs("'");
	} else {
		cputs(" BAD");
		fails++;
	}

	r = doparse("toolongname.txt");
	checks++;
	if (r == 0xffffL)
		cputs(" OK (invalid)");
	else {
		cputs(" BAD, should be FFFF");
		fails++;
	}

	r = doparse("a:x.y;secret");
	checks++;
	if (r == 0 && (fcb[0] & 0xff) == 1 && nameis("X       ", "Y  ")
	    && (fcb[26] & 0xff) == 6)
		cputs(" OK");
	else {
		cputs(" BAD");
		fails++;
	}

	report();
	return (fails != 0);
}


static VOID report()
{
	cputs("\r\nSCBTEST: ");
	cputs(fails == 0 ? "PASS " : "FAIL ");
	putdec((unsigned) checks);
	cputs(" checks, ");
	putdec((unsigned) fails);
	cputs(" bad\r\n");
}


static VOID puthex(n)
unsigned n;
{
	int		i;

	for (i = 12; i >= 0; i -= 4)
		conout(hexdig[(n >> i) & 0xf]);
}
