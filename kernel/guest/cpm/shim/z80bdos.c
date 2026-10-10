/* CP/M-80 CALL 5 and BIOS-hook bridge. C selects the function, DE is the
 * parameter; results use A/HL and B=H. Translate guest pointers, FCB
 * random-record byte order, and character-control blocks for native BDOS. */

#include "z80.h"
#include "gdpb.h"
#include "conmode.h"

int	z80bdosfn;		/* function of the last call, or -1	*/
int	z80biosfn;		/* BIOS vector of the last call, or -1	*/
z32	z80nbdos;		/* calls serviced			*/
z16	z80dma;			/* the guest's DMA address (fn 26)	*/
z16	z80srch;		/* guest FCB of the last search first	*/

/*
 * The multi-sector count the native BDOS is currently holding.
 *
 * It is mirrored here because it is HALF of every DMA bound: our BDOS's
 * multio() (src/bdos/bdosrw.c:342) loops the count and adds SECLEN to
 * the DMA address between records, so the window a transfer touches is
 * count * 128 bytes and not the 128 a single record needs.  A guest
 * reaches the count two ways -- function 44, and function 49 writing
 * the SCB's own copy at SCB_MLTIO, which scbpost() (src/bdos/scb.c:
 * 205-214) copies into GBL.multcnt with function 44's clamp -- so both
 * doors update this, and bdosmisc.c:171 resets it to one per program,
 * which is what z80bdosinit() does here.
 */
static int	z80mult = 1;

/* The guest's copy of the SCB: whether its address has been handed out,
 * and the copy as the last refresh left it. */
static int	scbpub;
static z8	scbsnap[SCBIMGLEN];

#define Z80SCBMLT 0x4a		/* SCB_MLTIO -- src/bdos/scb.h:64	*/

/* Collect consecutive function-2 output into native function 111 blocks.
 * Flush before other calls, BIOS hooks, refusals, and execution-loop exit. */
#define Z80OBUF	128		/* pending function 2 characters	*/

static char	obuf[Z80OBUF];
static int	onbuf;
static struct {			/* the native CCB for the batch		*/
	char	*a;
	z16	n;
} octl;

/* Report CP/M 3.1 with the 8080 machine byte for CP/M 3 guest utilities. */
z16	z80ver = 0x0031;

/* ------------------------------------------------------------------ */
/* parameter classes						       */

#define P_NONE	0		/* no parameter				*/
#define P_BYTE	1		/* E					*/
#define P_WORD	2		/* DE, a value				*/
#define P_FCB	3		/* DE, a 36-byte FCB			*/
#define P_STR	4		/* DE, a `$'-terminated string		*/
#define P_BUF	5		/* DE, a console read buffer		*/
#define P_A4	6		/* DE, four bytes (fns 104, 105)	*/
#define P_A6	7		/* DE, six bytes (fn 107)		*/
#define P_CCB	8		/* DE, a character control block	*/
#define P_SCB	9		/* DE, function 49's parameter block	*/
#define P_NO	10		/* not mapped				*/
#define P_DPB	11		/* fns 27 and 31: the answer is an address */

#define Z80BPB	8		/* fn 50: {func, A, BC, DE, HL}		*/
#define Z80FCB	36		/* sizeof(struct fcb) -- src/cmd/cpm.h	*/
#define Z80REN	52		/* fn 23: old FCB at 0, new at 16	*/
#define Z80SEQ	33		/* an FCB without the random record	*/
#define Z80DMA	128		/* one CP/M record			*/

/* Parameter classes for supported calls through 112. Functions returning
 * native system pointers and nested pointer APIs without translation are
 * refused. SCB address fields are filtered; CCB pointers are rebuilt. */
static z8 pmap[113] = {
	P_NONE,		/*  0 system reset -- handled before this table	*/
	P_NONE,		/*  1 console input				*/
	P_BYTE,		/*  2 console output -- collected, never reached	*/
	P_NONE,		/*  3 reader input				*/
	P_BYTE,		/*  4 punch output				*/
	P_BYTE,		/*  5 list output				*/
	P_BYTE,		/*  6 direct console i/o				*/
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
	P_FCB,		/* 23 rename file -- Z80REN bytes, see below	*/
	P_NONE,		/* 24 login vector				*/
	P_NONE,		/* 25 current disk				*/
	P_WORD,		/* 26 set DMA address -- handled before this table */
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
	P_BYTE,		/* 45 set BDOS error mode			*/
	P_BYTE,		/* 46 get disk free space -> the DMA buffer	*/
	P_NO,		/* 47 chain to program				*/
	P_NONE,		/* 48 flush buffers				*/
	P_SCB,		/* 49 get/set SCB				*/
	P_NO,		/* 50 direct BIOS call -- handled before this table */
	P_NO,		/* 51 set DMA base: CP/M-86's, not CP/M-80's	*/
	P_NO,		/* 52 get DMA base: ditto			*/
	P_NO,		/* 53 get max memory				*/
	P_NO,		/* 54 get absolute max				*/
	P_NO,		/* 55 alloc memory				*/
	P_NO,		/* 56 alloc absolute				*/
	P_NO,		/* 57 free memory				*/
	P_NO,		/* 58 free all memory				*/
	P_NO,		/* 59 program load				*/
	P_NO,		/* 60 call RSX -- handled before this table	*/
	P_NO,		/* 61 set exception vector			*/
	P_NO,		/* 62 (setsupf: ours, and not the guest's)	*/
	P_NO,		/* 63 get/set TPA limits				*/
	P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO,	/* 64-71	*/
	P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO,	/* 72-79	*/
	P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO,	/* 80-87	*/
	P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO, P_NO,	/* 88-95	*/
	P_NO,		/* 96						*/
	P_NO,		/* 97						*/
	P_NONE,		/* 98 free blocks				*/
	P_FCB,		/* 99 truncate file				*/
	P_FCB,		/* 100 set directory label			*/
	P_BYTE,		/* 101 return directory label			*/
	P_FCB,		/* 102 read file date stamps			*/
	P_FCB,		/* 103 write file XFCB (answers 0xFF)		*/
	P_A4,		/* 104 set date and time				*/
	P_A4,		/* 105 get date and time				*/
	P_NO,		/* 106 set default password			*/
	P_A6,		/* 107 return serial number			*/
	P_WORD,		/* 108 get/set program return code		*/
	P_WORD,		/* 109 get/set console mode -- DUMP's second call */
	P_WORD,		/* 110 get/set output delimiter			*/
	P_CCB,		/* 111 print block to console			*/
	P_CCB		/* 112 print block to list			*/
};

#define Z80MAXFN 112		/* the last entry in pmap[]		*/
#define Z80PARSE 152		/* parse filename: mapped past the table */

/*
 * The SCB offsets whose CONTENTS are an address.  The native side holds
 * a Z8000 address in each, which is no use to a guest, so none of them
 * passes through function 49 unchanged.  Four of the six name something
 * the shim knows the guest address of and are answered here; the other
 * two name buffers that only exist on the native side and stay refused.
 *
 * Offsets from src/bdos/scb.h; a word access at `off' also touches
 * `off + 1', so a two-byte field is entered under both its bytes.
 */
static z8 scbaddr[] = {
	0x15, 0x16,		/* ccp$conbuff				*/
	0x1e, 0x1f,		/* console buffer address		*/
	0x35, 0x36,		/* banked-BIOS 128-byte buffer address	*/
	0x3a, 0x3b,		/* address of the SCB image itself	*/
	0x3c, 0x3d,		/* current DMA address			*/
	0x47, 0x48,		/* address of the search FCB		*/
	0x62, 0x63		/* @MXTPA, the top of the user TPA	*/
};

#define Z80SCBADD 0x3a
#define Z80SCBDMA 0x3c
#define Z80SCBSRCH 0x47
#define Z80SCBMXTPA 0x62

/*
 * The SCB offsets holding a device assignment vector.  Our BDOS holds
 * zero in all five and never reads one (src/bdos/scb.c:48-52): character
 * I/O goes to the console, the list device follows the console and the
 * auxiliary line is the AUX channel, with no redirection anywhere.  So
 * these words are the shim's to answer, and a guest writing one is
 * refused rather than told a change took.
 *
 * Offsets from src/bdos/scb.h; both bytes of each word are entered, and
 * only the even one has an answer, exactly as scbaddr[] above.
 */
static z8 scbdev[] = {
	0x22, 0x23,		/* @CIVEC				*/
	0x24, 0x25,		/* @COVEC				*/
	0x26, 0x27,		/* @AIVEC				*/
	0x28, 0x29,		/* @AOVEC				*/
	0x2a, 0x2b		/* @LOVEC				*/
};

#define Z80SCBCIVEC 0x22
#define Z80SCBCOVEC 0x24
#define Z80SCBAIVEC 0x26
#define Z80SCBAOVEC 0x28
#define Z80SCBLOVEC 0x2a

static int inlist(off, t, n)
int off;
z8 *t;
int n;
{
	register int i;

	for (i = 0; i < n; i++)
		if (off == (int)t[i])
			return (1);
	return (0);
}

static int scbaddrfld(off)
int off;
{
	return (inlist(off, scbaddr,
		(int)(sizeof scbaddr / sizeof scbaddr[0])));
}

static int scbdevfld(off)
int off;
{
	return (inlist(off, scbdev,
		(int)(sizeof scbdev / sizeof scbdev[0])));
}

/*
 * The assignment this shim's character path actually makes, or -1 when
 * `off' is not the low byte of a vector.  CONIN, CONOUT and LIST are the
 * console, because BIOS vectors 2, 3, 4 and 5 all end at it; the two
 * auxiliary vectors are the AUX channel, which is where functions 3 and
 * 4 go.
 */
static long scbdevvec(off)
int off;
{
	switch (off) {
	case Z80SCBCIVEC:
	case Z80SCBCOVEC:
	case Z80SCBLOVEC:	return ((long)DEVBIT(DEV_CRT));
	case Z80SCBAIVEC:
	case Z80SCBAOVEC:	return ((long)DEVBIT(DEV_SIO));
	}
	return (-1L);
}

/*
 * The guest's answer for an address-valued offset, or -1 when there is
 * none to give.  @MXTPA is v3's own form -- the first address above the
 * TPA, which is the BDOS entry -- and the other three are the addresses
 * the guest itself named.
 */
static long scbguestaddr(off)
int off;
{
	switch (off) {
	case Z80SCBADD:		return ((long)FAKESCB);
	case Z80SCBDMA:		return ((long)z80dma);
	case Z80SCBSRCH:	return ((long)z80srch);
	case Z80SCBMXTPA:	return ((long)FAKEBDOS);
	}
	return (-1L);
}

/* Every word in the copy the shim fills in itself rather than from the
 * native SCB. */
static long scbown(off)
int off;
{
	register long v;

	if ((v = scbguestaddr(off)) >= 0)
		return (v);
	return (scbdevvec(off));
}

/* Reason codes, so a caller can print one sentence. */
#define BR_NONE	0
#define BR_FN	1
#define BR_ADDR	2
#define BR_BIOS	3
#define BR_HOOK	4
#define BR_SCB	5
#define BR_DEV	6

static int breason;

char *z80berr()
{
	switch (breason) {
	case BR_NONE:	return ("ok");
	case BR_FN:	return ("BDOS function not mapped in stage one");
	case BR_ADDR:	return ("parameter runs off the top of the guest's 64 KB");
	case BR_BIOS:	return ("BIOS vector not mapped in stage one");
	case BR_HOOK:	return ("a hook number this shim never planted");
	case BR_SCB:	return ("an SCB field whose value is a host address");
	case BR_DEV:	return ("a device assignment this BIOS cannot redirect");
	}
	return ("unknown");
}

/* ------------------------------------------------------------------ */

/*
 * z80addr -- the host address of `len' guest bytes at `off', or 0 when
 * they do not all fit below 0x10000.
 *
 * The arithmetic is done in z32 on purpose.  `off + len' in 16 bits is
 * exactly the wrap the guest is entitled to and the native BDOS is not,
 * so a check written that way would compute 0x0040 for the very case it
 * exists to catch and pass it.
 */
char *z80addr(m, off, len)
struct z80 *m;
z16 off;
z32 len;
{
	if ((z32)(off & 0xffff) + len > 0x10000L)
		return ((char *)0);
	return (m->m + (off & 0xffff));
}

/*
 * z80oflush -- send whatever function 2 has collected, as one function
 * 111.  A no-op when there is nothing pending, which is why it can be
 * called on every path without a test at the call site.
 *
 * `octl.a' is a host pointer, and on the target that IS the XADDR our
 * BDOS reads out of a character control block (src/cmd/cpm.h:5-13); the
 * buffer is ours, not the guest's, so there is no range check to make.
 *
 * PUBLIC because the seam is not the only way a run can end: the caller's
 * loop also stops on a bad instruction, a HALT or its own step limit, and
 * none of those comes through here.  Every caller calls it once when its
 * loop ends, so a guest that crashes mid-line still gets the line it had
 * written.
 */
int z80oflush()
{
	register int n;

	if ((n = onbuf) == 0)
		return (0);
	onbuf = 0;
	octl.a = obuf;
	octl.n = (z16)n;
	z80sys(111, (z16)0, (char *)&octl);
	return (n);
}

/* Sleep n ticks with BDOS function 141, the guest's output shown first. */
int z80sleep(n)
int n;
{
	z80oflush();
	return (z80sys(141, (z16)n, (char *)0));
}

/*
 * Re-issue the native set-DMA.  One place where a DMA address is formed
 * and one place where it is checked, called from load time and from
 * function 26.
 */
static setdma(m)
struct z80 *m;
{
	register char *p;

	p = z80addr(m, z80dma, (z32)Z80DMA);
	if (p == (char *)0) {
		breason = BR_ADDR;
		return (0);
	}
	z80sys(26, (z16)0, p);
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
static int dmafits(m)
struct z80 *m;
{
	return (z80addr(m, z80dma, (z32)z80mult * (z32)Z80DMA)
		!= (char *)0);
}

/*
 * dparms -- functions 27 and 31, whose answer is an ADDRESS.
 *
 * Our BDOS never hands one out: function 31 copies the disk parameter
 * block to a buffer the caller names, and function 27 does the same with
 * the allocation vector, because neither structure is addressable from a
 * transient program.  So the buffer named here is one inside the guest,
 * above GUESTTOP where the guest's own TPA ends, and `*offp' is the
 * offset the guest is given.
 *
 * Zero when the vector of this drive is longer than the furniture, which
 * takes a drive of more than 28,287 blocks -- ours are 2,560 and 2,048
 * (src/bios/bios900.c) -- and which leaves function 27 refused rather
 * than answered with a truncated map.
 */
static int dparms(m, fn, offp)
struct z80 *m;
int fn;
z16 *offp;
{
	struct gdpb d;
	long n;

	z80sys(31, (z16)0, (char *)&d);
	if (fn == 31) {
		gdpbpack(&d, z80addr(m, (z16)FAKEDPB, (z32)GDPB_LEN));
		*offp = FAKEDPB;
		return (1);
	}
	n = gdpbalv(&d);
	if (n > (long)(FAKEBIOS - FAKEALV))
		return (0);
	z80sys(27, (z16)0, z80addr(m, (z16)FAKEALV, (z32)n));
	*offp = FAKEALV;
	return (1);
}

static char	fbuf[Z80REN];		/* the FCB the native BDOS sees	*/
static char	sbuf[Z80FCB];		/* ... and the one it searches with */

/* The functions that use the random record, bytes 33-35. */
static int isrand(fn)
int fn;
{
	return ((fn >= 33 && fn <= 36) || fn == 40 || fn == 99);
}

/* The five functions src/bdos/bdosrw.c multio() shells. */
static int ismulti(fn)
int fn;
{
	return (fn == 20 || fn == 21 || fn == 33 || fn == 34 || fn == 40);
}

/* The console mode the CCP was running in, kept for the guest's exit. */
static int z80conmode = -1;

/*
 * z80bdosinit -- the state CP/M-80 gives a program before its first
 * instruction: the DMA buffer is page zero's own 128-byte tail buffer
 * at 0x0080.  Our BDOS has to be told, because the guest will not tell
 * it until it wants a different one, and a program that reads its
 * command tail with function 10 or copies a record before setting a DMA
 * address of its own is relying on that default.
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
int z80bdosinit(m)
struct z80 *m;
{
	z80conmode = z80sys(109, (z16)0xffff, (char *)0);
	z80sys(109, (z16)CM_NOSTOP, (char *)0);
	z80dma = PZ_DMA;
	z80mult = 1;		/* src/bdos/bdosmisc.c:171		*/
	z80srch = 0;
	scbpub = 0;
	onbuf = 0;
	z80bdosfn = -1;
	z80biosfn = -1;
	breason = BR_NONE;
	z80nbdos = 0;
	return (setdma(m));
}

/* Give the console back however the run ended: a guest must not leave a
 * mode behind for whatever the CCP starts next. */
int z80bdosfini()
{
	if (z80conmode < 0)
		return (0);
	z80sys(109, (z16)z80conmode, (char *)0);
	z80conmode = -1;
	return (1);
}

/* ------------------------------------------------------------------ */
/* the guest-resident SCB image					       */

/*
 * The native SCB is reachable only a word at a time through function
 * 49, so offset 0x3A has no pointer into it to hand out.  The shim
 * instead keeps a copy of the whole image at FAKESCB -- above the TPA,
 * where the guest cannot allocate over it -- and publishes that.
 *
 * scbpull() refills the copy and remembers what it wrote; scbpush()
 * sends back the bytes the guest has changed since.  Both run around
 * every function 49 and after a BIOS TIME, so a field read through the
 * published address is as fresh as the guest's last call and a field
 * written there lands on its next one.  Neither is entered before a
 * guest asks for the address.
 */
#define Z80SCBMAX 99		/* SCBMAX -- src/bdos/scb.h:80		*/
#define Z80SCBPB  4		/* the function 49 parameter block	*/

static int scbpull(m)
struct z80 *m;
{
	char pbk[Z80SCBPB];
	register int off;
	long a;
	int v;

	for (off = 0; off < SCBIMGLEN; off += 2) {
		pbk[0] = (char)off;
		pbk[1] = 0;			/* a get		*/
		pbk[2] = 0;
		pbk[3] = 0;
		v = z80sys(49, (z16)0, pbk);
		m->m[(z16)(FAKESCB + off)] = (char)(v & 0xff);
		m->m[(z16)(FAKESCB + off + 1)] = (char)((v >> 8) & 0xff);
	}
	for (off = 0; off < SCBIMGLEN; off++) {
		a = scbown(off);
		if (a < 0)
			continue;
		m->m[(z16)(FAKESCB + off)] = (char)(a & 0xff);
		m->m[(z16)(FAKESCB + off + 1)] = (char)((a >> 8) & 0xff);
	}
	for (off = 0; off < SCBIMGLEN; off++)
		scbsnap[off] = (z8)(m->m[(z16)(FAKESCB + off)] & 0xff);
	return (0);
}

static int scbpush(m)
struct z80 *m;
{
	char pbk[Z80SCBPB];
	register int off;
	int v;
	z32 n;

	for (off = 0; off < Z80SCBMAX; off++) {
		if (scbaddrfld(off) || scbdevfld(off))
			continue;
		v = m->m[(z16)(FAKESCB + off)] & 0xff;
		if (v == (int)scbsnap[off])
			continue;
		pbk[0] = (char)off;
		pbk[1] = (char)0xff;		/* set a byte		*/
		pbk[2] = (char)v;
		pbk[3] = 0;
		if (z80sys(49, (z16)0, pbk) == 0 && off == Z80SCBMLT) {
			n = (z32)v;
			if (n == 0)
				n = 1;
			if (n > 128)
				n = 128;
			z80mult = (int)n;
		}
		scbsnap[off] = (z8)v;
	}
	return (0);
}

/* ------------------------------------------------------------------ */

/*
 * devtbl -- build the character-device table vector 20 answers with.
 *
 * Two entries, because two character devices is what our BIOS has.  CRT
 * is the machine's console, which BIOS vectors 2, 3, 4 and 5 all reach
 * (src/bios/bios900.c cases 2-5), and it is neither serial nor
 * flow-controlled from a guest's side: the ROM console is a keyboard and
 * a screen, and a console bound to a spare serial line is bound by BIOS
 * function 28 and not from here.  SIO is the auxiliary line functions 3
 * and 4 go to (src/bios/bios900.c cases 6-7), which IS a serial channel;
 * it is not called AUX because AUX is one of CP/M 3's own LOGICAL device
 * names, and a physical device sharing it is read as the logical one.
 *
 * Both baud codes are zero, which is CP/M 3's "no rate to report".  The
 * serial channel runs at 38,400 (boot900.c sccinit) and CP/M 3's codes
 * stop at 19,200, so there is no honest code to put there; DEVM_SOFTBAUD
 * is off on both because the rate is the BIOS's and a guest cannot
 * change it, which is also why vector 21 stays refused by name.
 */
static devtbl(m)
struct z80 *m;
{
	static z8 dtb[FAKEDEVLEN] = {
		'C', 'R', 'T', ' ', ' ', ' ', DEVM_IN | DEVM_OUT, 0,
		'S', 'I', 'O', ' ', ' ', ' ',
			DEVM_IN | DEVM_OUT | DEVM_SERIAL, 0,
		0, 0, 0, 0, 0, 0, 0, 0
	};
	register int i;

	for (i = 0; i < FAKEDEVLEN; i++)
		m->m[(z16)(FAKEDEV + i)] = (char)dtb[i];
	return (0);
}

/* Function 6: E = FFh answers a key or 0 without waiting, FEh the
 * status, FDh waits for a key, anything else is output.  Our FFh is
 * the one that waits. */
static int dconio(e)
int e;
{
	if (e == 0xfd)
		return (z80sys(6, (z16)0xff, (char *)0) & 0xff);
	if (e != 0xfe && e != 0xff)
		return (z80sys(6, (z16)e, (char *)0));
	if ((z80sys(6, (z16)0xfe, (char *)0) & 0xff) == 0)
		return (0);
	if (e == 0xfe)
		return (0xff);
	return (z80sys(6, (z16)0xff, (char *)0) & 0xff);
}

/* Set when the vector just serviced answers in HL rather than in A. */
static int bioshl;

/* Map character BIOS vectors onto native BDOS character calls.
 * Disk BIOS vectors are refused because their DPH/translation pointers
 * would require a guest representation of native disk structures. */
static int biosv(m, v)
struct z80 *m;
int v;
{
	register int r;
	char tbuf[4];

	z80biosfn = v;
	bioshl = 0;
	switch (v) {
	case 0:				/* cold boot			*/
	case 1:				/* warm boot			*/
		z80rsxwboot(m);
		return (B_EXIT);
	case 2:				/* CONST			*/
		r = z80sys(11, (z16)0, (char *)0);
		/* CP/M-80's CONST answers 0xFF for "a character is
		 * waiting" and 0x00 for "none"; our function 11 answers
		 * 1 and 0.  The widening is the whole of the mapping. */
		m->a = (z8)(r ? 0xff : 0x00);
		return (B_RUN);
	case 3:				/* CONIN			*/
		m->a = (z8)dconio(0xfd);
		return (B_RUN);
	case 4:				/* CONOUT			*/
		z80sys(6, (z16)(z80getr(m, R_C) & 0xff), (char *)0);
		return (B_RUN);
	case 5:				/* LIST				*/
		z80sys(5, (z16)(z80getr(m, R_C) & 0xff), (char *)0);
		return (B_RUN);
	case 6:				/* PUNCH			*/
		z80sys(4, (z16)(z80getr(m, R_C) & 0xff), (char *)0);
		return (B_RUN);
	case 7:				/* READER			*/
		m->a = (z8)(z80sys(3, (z16)0, (char *)0) & 0xff);
		return (B_RUN);
	case 15:			/* LISTST			*/
		m->a = 0xff;		/* the list device is always ready */
		return (B_RUN);
	case 20:			/* DEVTBL			*/
		/* The answer is an address, so the table is built inside
		 * the guest for the reason the DPB and the SCB copy are. */
		devtbl(m);
		m->rp[P_HL] = (z16)FAKEDEV;
		m->a = (z8)(FAKEDEV & 0xff);
		bioshl = 1;
		return (B_RUN);
	case 26:			/* TIME				*/
		/* C = 0 reads the clock; C = 0FFh would set it, and the
		 * guest's copy of the SCB is not where our clock takes
		 * a setting from.  Function 105 refreshes the native
		 * SCB's own stamp bytes, so the copy is refilled. */
		if ((z80getr(m, R_C) & 0xff) != 0)
			break;
		m->a = (z8)(z80sys(105, (z16)0, tbuf) & 0xff);
		if (scbpub)
			scbpull(m);
		return (B_RUN);
	}
	breason = BR_BIOS;
	return (B_BIOS);
}

/* ------------------------------------------------------------------ */

/*
 * z80bdos -- service the hook z80step() just refused to service.
 *
 * Returns B_RUN to resume the guest, B_EXIT when it has terminated, or
 * one of the refusals, with z80berr() naming it.  PC is already past the
 * three-byte hook, and the byte after it is a RET, so a resumed guest
 * returns to its own CALL site with no further help.
 */
/* Guest FCB random records are little-endian; a native BDOS with z80ranbe
 * set keeps big-endian bytes at offsets 33..35. Swap around relevant calls,
 * excluding rename because those bytes belong to its second name. */
int	z80ranbe = 1;

static ranswap(p, fn)
char *p;
int fn;
{
	register int t;

	if (!z80ranbe)
		return (0);
	switch (fn) {
	case 33:	/* read random			*/
	case 34:	/* write random			*/
	case 35:	/* compute file size		*/
	case 36:	/* set random record		*/
	case 40:	/* write random with zero fill	*/
	case 99:	/* truncate file		*/
		t = p[33];
		p[33] = p[35];
		p[35] = (char)t;
	}
	return (0);
}

int z80bdos(m)
struct z80 *m;
{
	register int fn, cls;
	register char *p;
	z16 de, dpboff;
	int r, off, set, k;
	long ga;
	z32 n;
	/* The native character control block for functions 111 and 112.
	 * `a' is a host pointer, which on the target IS the XADDR our
	 * BDOS reads out of it (src/cmd/cpm.h:5-13). */
	static struct {
		char	*a;
		z16	n;
	} nccb;

	breason = BR_NONE;
	/* Not a BDOS call, so it cannot join the batch: a BIOS vector
	 * writes to the console itself, the exit hook ends the run, and
	 * a hook we never planted is a refusal somebody has to read. */
	if (z80hookno != HOOK_BDOS)
		z80oflush();
	if (z80hookno >= HOOK_BIOS && z80hookno < HOOK_BIOS + NBIOSV)
		return (biosv(m, z80hookno - HOOK_BIOS));
	if (z80hookno == HOOK_EXIT) {
		z80bdosfn = -1;
		/* A plain RET goes back to the CCP, and the CCP warm
		 * starts, so the temporary half of the chain goes here
		 * too and not only at the BIOS vector. */
		z80rsxwboot(m);
		return (B_EXIT);
	}
	if (z80hookno != HOOK_BDOS) {
		breason = BR_HOOK;
		return (B_HOOKNO);
	}

	fn = z80getr(m, R_C) & 0xff;
	de = m->rp[P_DE];
	z80bdosfn = fn;
	z80biosfn = -1;
	z80nbdos++;

	/* ---- console output, collected rather than passed on. */

	if (fn == 2) {
		if (onbuf >= Z80OBUF)
			z80oflush();
		obuf[onbuf++] = (char)(de & 0xff);
		/* Our function 2 falls out of the switch to the default
		 * return value (src/bdos/bdosmain.c:239), so a deferred
		 * one answers what an immediate one would have: zero in
		 * all three of CP/M-80's result places. */
		m->rp[P_HL] = 0;
		m->a = 0;
		z80setr(m, R_B, 0);
		return (B_RUN);
	}
	z80oflush();		/* everything below is observable	*/

	/* ---- the three functions that never reach the native BDOS. */

	if (fn == 0) {
		/* System reset.  Our function 0 is warmboot() and does
		 * not return (src/bdos/bdosmain.c:234); the shim is an
		 * ordinary TPA program and must return to ITS caller,
		 * so this is the one function the seam answers itself. */
		z80rsxwboot(m);
		return (B_EXIT);
	}
	if (fn == 12) {
		m->a = (z8)(z80ver & 0xff);
		m->rp[P_HL] = z80ver;
		z80setr(m, R_B, (z80ver >> 8) & 0xff);
		return (B_RUN);
	}
	if (fn == 26) {
		/* A REFUSED function 26 must leave z80dma holding the
		 * address the native BDOS was actually told.  It used to
		 * commit first and refuse afterwards, so a guest whose
		 * DMA was rejected went on with a z80dma nothing agreed
		 * about -- which the multi-sector bound below then reads. */
		z16 sav;

		sav = z80dma;
		z80dma = de;
		if (!setdma(m)) {
			z80dma = sav;
			return (B_ADDR);
		}
		return (B_RUN);
	}

	if (fn == 50) {
		/* The BIOS parameter block: function, A, then BC, DE and
		 * HL as guest words.  Load the registers the vector
		 * reads and let the BIOS seam answer, so a vector it does
		 * not map is still refused by its own name. */
		p = z80addr(m, de, (z32)Z80BPB);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		m->a = (z8)(p[1] & 0xff);
		m->rp[P_BC] = (z16)((p[2] & 0xff) | ((p[3] & 0xff) << 8));
		m->rp[P_DE] = (z16)((p[4] & 0xff) | ((p[5] & 0xff) << 8));
		m->rp[P_HL] = (z16)((p[6] & 0xff) | ((p[7] & 0xff) << 8));
		r = biosv(m, p[0] & 0xff);
		if (r != B_RUN)
			return (r);
		/* Most BIOS vectors answer in A, and the BDOS call they
		 * came through answers in all three places.  A vector
		 * whose answer is an ADDRESS answers in HL, and widening
		 * A over it would hand back its low byte. */
		if (bioshl)
			z80setr(m, R_B, (m->rp[P_HL] >> 8) & 0xff);
		else {
			m->rp[P_HL] = (z16)(m->a & 0xff);
			z80setr(m, R_B, 0);
		}
		return (B_RUN);
	}
	if (fn == 59) {
		/* Program load, reaching the end of the chain.  The shim
		 * loads the one image it was started with, so the answer
		 * is CP/M 3's own "nothing loaded": zero in HL, and a
		 * module that passed the call down gets it. */
		m->a = 0xff;
		m->rp[P_HL] = 0;
		z80setr(m, R_B, 0);
		return (B_RUN);
	}
	if (fn == 60) {
		/* The BDOS is the LAST link in the chain, never the first:
		 * page zero's vector names the lowest module and each
		 * module's prefix either handles the call or jumps to the
		 * link below it, so a function 60 that arrives here has
		 * already been offered to every module.  0FFh is then the
		 * true answer whether a chain is loaded or not. */
		m->a = 0xff;
		m->rp[P_HL] = 0x00ff;
		z80setr(m, R_B, 0);
		return (B_RUN);
	}

	if (fn == 115) {
		if ((r = z80gsx(m, de)) == B_ADDR)
			breason = BR_ADDR;
		return (r);
	}
	if (fn > Z80MAXFN) {
		/* 152 parse filename would be worth having -- it is how
		 * a CP/M 3 program turns a command tail into an FCB --
		 * but its parameter block is {name, FCB} and both are
		 * XADDRs here where the 8080's are two bytes
		 * (src/bdos/parsefn.c struct pfcb), so it is the CCB problem
		 * again with two pointers instead of one.  Rebuilding it
		 * is a stage-two widening: nothing in the corpus reached
		 * it, and a guess would return an FCB address the guest
		 * cannot use. */
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
	 * record outside the guest's 64 KB altogether.  Refused here,
	 * before the native BDOS is told, because there is no partial
	 * transfer to undo afterwards. */
	if (ismulti(fn) && !dmafits(m)) {
		breason = BR_ADDR;
		return (B_ADDR);
	}

	switch (cls) {
	case P_NONE:
		r = z80sys(fn, (z16)0, (char *)0);
		/* Function 13 is DMA := 0080h, and the native BDOS moved
		 * its own DMA to ITS base page: without this the guest's
		 * next record lands there instead of in the guest. */
		if (fn == 13) {
			z80dma = PZ_DMA;
			if (!setdma(m))
				return (B_ADDR);
		}
		break;
	case P_BYTE:
		if (fn == 6) {
			r = dconio((int)(de & 0xff));
			break;
		}
		r = z80sys(fn, (z16)(de & 0xff), (char *)0);
		/* Function 44 is the first of the two doors to the count
		 * this seam has to watch; the native BDOS answers 0xff
		 * for a count it did not take (src/bdos/bdosmain.c:613)
		 * and leaves its own alone, so this follows it. */
		if (fn == 44 && r == 0)
			z80mult = (int)(de & 0xff);
		break;
	case P_WORD:
		r = z80sys(fn, de, (char *)0);
		break;
	case P_DPB:
		if (!dparms(m, fn, &dpboff)) {
			breason = BR_FN;
			return (B_FN);
		}
		r = (int)dpboff;
		break;
	case P_FCB:
		n = fn == 23 ? (z32)Z80REN : (z32)Z80FCB;
		p = z80addr(m, de, n);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		/* @SEARCHA follows the FCB a search first was given;
		 * search next keeps using that one, so the copy is kept
		 * until the next search first. */
		if (fn == 17 || fn == 18) {
			for (k = 0; fn == 17 && k < Z80FCB; k++)
				sbuf[k] = p[k];
			r = z80sys(fn, (z16)0, sbuf);
			if (fn == 17)
				z80srch = de;
			break;
		}
		/* The native BDOS writes back a whole 36-byte FCB, after
		 * any transfer into the DMA buffer, but a sequential
		 * call's FCB is 33 bytes and a buffer may follow it.  So
		 * the call works on a copy and only the bytes the
		 * function owns go back. */
		for (k = 0; k < (int)n; k++)
			fbuf[k] = p[k];
		ranswap(fbuf, fn);
		r = z80sys(fn, (z16)0, fbuf);
		ranswap(fbuf, fn);
		if (fn != 23 && !isrand(fn))
			n = Z80SEQ;
		for (k = 0; k < (int)n; k++)
			p[k] = fbuf[k];
		break;
	case P_STR:
		/* Function 9's string ends at a `$' the guest put there.
		 * The native BDOS will scan for it; if there is none
		 * before the top of the guest's memory that scan runs
		 * out of the segment, so the scan happens HERE first and
		 * the call is refused if it does not terminate. */
		p = z80addr(m, de, 1L);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		n = 0x10000L - (z32)(de & 0xffff);
		while (n > 0 && *p != '$') {
			p++;
			n--;
		}
		if (n <= 0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		r = z80sys(fn, (z16)0, z80addr(m, de, 1L));
		break;
	case P_BUF:
		/* Byte 0 of a console buffer is the maximum the caller
		 * will accept, so the buffer is that plus the two count
		 * bytes -- and the check has to use the guest's own
		 * number, not a guess. */
		p = z80addr(m, de, 1L);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		n = (z32)(*p & 0xff) + 2;
		p = z80addr(m, de, n);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		r = z80sys(fn, (z16)0, p);
		break;
	case P_A4:
	case P_A6:
		n = cls == P_A4 ? 4L : 6L;
		p = z80addr(m, de, n);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		r = z80sys(fn, (z16)0, p);
		break;
	case P_SCB:
		/* Four bytes: {offset, set flag, value low, value
		 * high}.  No pointer in it, so it goes by reference --
		 * but the offset it names decides whether the ANSWER is
		 * a number or one of our addresses. */
		p = z80addr(m, de, (z32)Z80SCBPB);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		off = p[0] & 0xff;
		set = (p[1] & 0xff) == 0xff || (p[1] & 0xff) == 0xfe;
		if (scbpub)
			scbpush(m);	/* what the guest wrote in the copy */
		ga = scbguestaddr(off);
		if (ga >= 0) {
			/* @SEARCHA is the only address field a guest
			 * writes: ERASE saves it, erases, and puts it
			 * back.  It names a guest FCB, so the shim's own
			 * mirror is where the value belongs. */
			if (set && off == Z80SCBSRCH) {
				z80srch = (z16)((p[2] & 0xff)
					| ((p[3] & 0xff) << 8));
				r = 0;
			} else if (set) {
				breason = BR_SCB;
				return (B_FN);
			} else {
				r = (int)ga;
				if (off == Z80SCBADD)
					scbpub = 1;
			}
		} else if (scbaddrfld(off)) {
			breason = BR_SCB;
			return (B_FN);
		} else if (scbdevfld(off)) {
			/* A read is the assignment this shim's character
			 * path makes; a write would be a redirection our
			 * BIOS has no way to perform, so it is refused
			 * rather than accepted and dropped. */
			ga = scbdevvec(off);
			if (set || ga < 0) {
				breason = BR_DEV;
				return (B_FN);
			}
			r = (int)ga;
		} else {
			r = z80sys(fn, (z16)0, p);
			/* The second door to the multi-sector count.
			 * scbpost() puts the stored byte into
			 * GBL.multcnt through function 44's clamp, which
			 * is the clamp repeated here. */
			if (r == 0 && off == Z80SCBMLT && set) {
				n = (z32)(p[2] & 0xff);
				if (n == 0)
					n = 1;
				if (n > 128)
					n = 128;
				z80mult = (int)n;
			}
		}
		if (scbpub)
			scbpull(m);
		break;
	case P_CCB:
		/* Translate the guest's little-endian {address, length} words into
		 * a native CCB with a far pointer, validating the complete buffer. */
		p = z80addr(m, de, 4L);
		if (p == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		nccb.n = (z16)((p[2] & 0xff) | ((p[3] & 0xff) << 8));
		nccb.a = z80addr(m,
			(z16)((p[0] & 0xff) | ((p[1] & 0xff) << 8)),
			(z32)nccb.n);
		if (nccb.a == (char *)0) {
			breason = BR_ADDR;
			return (B_ADDR);
		}
		r = z80sys(fn, (z16)0, (char *)&nccb);
		break;
	default:
		breason = BR_FN;
		return (B_FN);
	}

	/*
	 * The result, in the three places CP/M-80 leaves it: A for a
	 * byte, HL for a word, and B = H with A = L.  Our BDOS already
	 * returns CP/M 3's word form -- the high byte carries the
	 * physical error code when function 45 put the program in
	 * return mode (src/bdos/bdosmain.c:861-862) -- which is what the H
	 * half of this convention is for, so the word passes straight
	 * through.
	 */
	m->rp[P_HL] = (z16)r;
	m->a = (z8)(r & 0xff);
	z80setr(m, R_B, (r >> 8) & 0xff);
	return (B_RUN);
}
