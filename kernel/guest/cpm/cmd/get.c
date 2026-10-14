/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * get.c - GET.68K: console input from a file, on the RSX mechanism.
 *
 *	GET [CONSOLE] [INPUT] [FROM] FILE name [[ECHO]|[NO ECHO]]
 *	GET [CONSOLE] [INPUT] [FROM] CONSOLE
 *
 * GET reads GET.RSX from the drive it was loaded from, patches the file's
 * FCB and the echo flag into it and attaches it with function 60
 * sub-function 127.  The module's sub-functions are CP/M 3's: 129 stops
 * it, 130 asks whether it is there.
 *
 * [ECHO] and [NO ECHO] are implemented; [SYSTEM] is accepted, since the
 * CCP reads through the chain like any program.  [PROGRAM], [FILTERED],
 * [RAW] and AUX: are refused by name.
 */
#include "cpm.h"

#define	RSXMAGIC	0x5253
#define	RSX_ATTACH	127
#define	BDOS_CALLRSX	60

/* the module's sub-functions, CP/M 3's numbers */
#define	GET_KILL	129
#define	GET_FCB		130

/* offsets in the file: its 28-byte header, the module header, then
   the two fields the module leaves for us */
#define	F_HDR		28
#define	H_MAGIC		(F_HDR + 10)
#define	H_ECHO		(F_HDR + 42)
#define	H_FCB		(F_HDR + 44)

#define	MODNAME		"GET.RSX"

struct rsxpb {
	char	rpfunc;
	char	rprsvd;
	long	rporg;
	long	rplen;
	long	rpsrc;
};

char		buf[4096];		/* the module image, as read	*/
struct fcb	filefcb;		/* the file the module reads	*/
struct fcb	tryfcb;			/* a copy, used to prove it opens */
struct rsxpb	pb;

static unsigned wordat(off)
int off;
{
	return (((buf[off] & 0xff) << 8) | (buf[off + 1] & 0xff));
}

/* one BDOS function 60 sub-function call with no other argument */
static long subfn(n)
int n;
{
	pb.rpfunc = (char) n;
	pb.rprsvd = 0;
	return (__bdosl(BDOS_CALLRSX, (long) &pb));
}

/* the module answers 130 with its FCB's address, the BDOS with 0FFh */
static int active()
{
	register long r;

	r = subfn(GET_FCB);
	return (r != 0 && r != 0xff);
}

/* an argument compared against a keyword, with the brackets, commas and
   equals signs a CP/M 3 command line puts around its options taken off
   first: the CCP splits the tail at blanks only, so `[NO ECHO]' arrives
   as the two words `[NO' and `ECHO]'. */
static int kwis(a, k)
register char *a;
register char *k;
{
	while (*a == '[' || *a == ']' || *a == ',' || *a == '=')
		a++;
	while (*k != 0) {
		if (*a++ != *k++)
			return (0);
	}
	while (*a == '[' || *a == ']' || *a == ',' || *a == '=')
		a++;
	return (*a == 0);
}

static VOID say(s)
char *s;
{
	cputs(s);
}

static VOID sayfcb(f)
register struct fcb *f;
{
	char	nm[13];
	register int i, j;

	j = 0;
	for (i = 0; i < 8 && f->fname[i] != ' '; i++)
		nm[j++] = f->fname[i];
	if (f->ftype[0] != ' ') {
		nm[j++] = '.';
		for (i = 0; i < 3 && f->ftype[i] != ' '; i++)
			nm[j++] = f->ftype[i];
	}
	nm[j] = 0;
	say(nm);
}

static VOID usage()
{
	say("usage: GET [CONSOLE] [INPUT] [FROM] FILE name [[ECHO]|[NO ECHO]]\r\n");
	say("       GET [CONSOLE] [INPUT] [FROM] CONSOLE\r\n");
}

/* read GET.RSX, patch the FCB and the echo flag into it, and attach it */
static int loadmod(echo)
int echo;
{
	register int	n, i;
	register char	*p;

	mkfcb(MODNAME, &tryfcb);
	tryfcb.drvcode = _base->resvd1[0] + 1;
	if ((__bdos(BDOS_OPEN, (long) &tryfcb) & 0xff) == 0xff) {
		say("GET: cannot find ");
		say(MODNAME);
		say("\r\n");
		return (1);
	}
	for (n = 0; n < sizeof buf; n += SECLEN) {
		setdma(&buf[n]);
		if (__bdos(BDOS_READSEQ, (long) &tryfcb) != 0)
			break;
	}
	__bdos(BDOS_CLOSE, (long) &tryfcb);
	setdma(_base->buff);
	if (n < H_FCB + 36 || wordat(H_MAGIC) != RSXMAGIC) {
		say("GET: ");
		say(MODNAME);
		say(" is not a resident system extension\r\n");
		return (1);
	}
	buf[H_ECHO] = (char) (echo ? 1 : 0);
	p = (char *) &filefcb;
	for (i = 0; i < sizeof (struct fcb); i++)
		buf[H_FCB + i] = p[i];
	pb.rpfunc = RSX_ATTACH;
	pb.rprsvd = 0;
	pb.rporg = 0;
	pb.rplen = n;
	pb.rpsrc = (long) buf;
	if (__bdosl(BDOS_CALLRSX, (long) &pb) < 256) {
		say("GET: no room for the module\r\n");
		return (1);
	}
	return (0);
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i;
	int		echo, tocons, fileat;

	echo = 1;
	tocons = 0;
	fileat = 0;

	for (i = 1; i < argc; i++) {
		if (fileat == i) {		/* the file name itself */
			continue;
		}
		if (kwis(argv[i], "CONSOLE") || kwis(argv[i], "CON:")) {
			/*  Two different CONSOLEs share the word: the one
			    before FROM names what is being redirected and
			    is the only thing this system has, and the one
			    after it means `stop'.  A FILE seen already
			    settles which this is.  */
			if (fileat != 0)
				continue;
			tocons = 1;
			continue;
		}
		if (kwis(argv[i], "INPUT") || kwis(argv[i], "FROM"))
			continue;
		if (kwis(argv[i], "FILE")) {
			if (i + 1 >= argc) {
				say("GET: FILE needs a file name\r\n");
				return (1);
			}
			fileat = i + 1;
			tocons = 0;
			continue;
		}
		if (kwis(argv[i], "ECHO")) {
			continue;		/* NO/NOT already saw it */
		}
		if (kwis(argv[i], "NO") || kwis(argv[i], "NOT")) {
			echo = 0;
			continue;
		}
		if (kwis(argv[i], "SYSTEM"))
			continue;		/* what this GET already is */
		if (kwis(argv[i], "PROGRAM")) {
			say("GET: [PROGRAM] is not implemented.  The CCP is a\r\n");
			say("     transient here and reads through the same RSX\r\n");
			say("     chain as a program, so this GET is always\r\n");
			say("     [SYSTEM] and cannot feed one and not the other\r\n");
			return (1);
		}
		if (kwis(argv[i], "FILTERED") || kwis(argv[i], "RAW")) {
			say("GET: [FILTERED] and [RAW] are not implemented; the\r\n");
			say("     file is served byte for byte as it stands\r\n");
			return (1);
		}
		if (kwis(argv[i], "AUXILIARY") || kwis(argv[i], "AUXIN:")
		    || kwis(argv[i], "AUX:")) {
			say("GET: this system has no AUX: device to read\r\n");
			return (1);
		}
		if (fileat == 0 && !tocons) {
			fileat = i;		/* GET name, the short form */
			continue;
		}
		say("GET: unknown keyword or option: ");
		say(argv[i]);
		say("\r\n");
		return (1);
	}

	if (fileat == 0 && !tocons) {
		usage();
		return (1);
	}

	if (tocons) {			/* GET CONSOLE: stop reading */
		if (!active()) {
			say("GET: no file is being read\r\n");
			return (1);
		}
		subfn(GET_KILL);
		say("Getting console input from console\r\n");
		return (0);
	}

	if (active()) {
		say("GET: a file is already being read; GET CONSOLE first\r\n");
		return (1);
	}

	mkfcb(argv[fileat], &filefcb);
	for (i = 0; i < sizeof (struct fcb); i++)
		((char *) &tryfcb)[i] = ((char *) &filefcb)[i];
	if ((__bdos(BDOS_OPEN, (long) &tryfcb) & 0xff) == 0xff) {
		say("GET: no such file: ");
		say(argv[fileat]);
		say("\r\n");
		return (1);
	}
	__bdos(BDOS_CLOSE, (long) &tryfcb);

	if (loadmod(echo) != 0)
		return (1);

	say("Getting console input from file: ");
	sayfcb(&filefcb);
	say(echo ? " [ECHO]\r\n" : " [NO ECHO]\r\n");
	return (0);
}
