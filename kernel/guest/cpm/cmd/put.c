/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * put.c - PUT.68K: console output into a file, on the RSX mechanism.
 *
 *	PUT [CONSOLE] [OUTPUT] [TO] FILE name [[ECHO]|[NO ECHO]]
 *	PUT [CONSOLE] [OUTPUT] [TO] CONSOLE
 *
 * PUT makes the file empty, reads PUT.RSX from the drive it was loaded
 * from, patches the FCB and the echo flag into it and attaches it.  The
 * module's sub-functions are CP/M 3's: 133 writes the last record and
 * closes the file, 134 asks whether it is there.  PUT CONSOLE must end a
 * PUT, or the last partial record is lost.
 *
 * [ECHO] and [NO ECHO] are implemented; [SYSTEM] is accepted.  [PROGRAM],
 * [FILTERED], [RAW] and the printer are refused by name.  The file must
 * not exist already.
 */
#include "cpm.h"

#define	RSXMAGIC	0x5253
#define	RSX_ATTACH	127
#define	BDOS_CALLRSX	60

/* the module's sub-functions, CP/M 3's numbers */
#define	PUT_KILL	133
#define	PUT_FCB		134

/* offsets in the file: its 28-byte header, the module header, then
   the two fields the module leaves for us */
#define	F_HDR		28
#define	H_MAGIC		(F_HDR + 10)
#define	H_ECHO		(F_HDR + 42)
#define	H_FCB		(F_HDR + 44)

#define	MODNAME		"PUT.RSX"

struct rsxpb {
	char	rpfunc;
	char	rprsvd;
	long	rporg;
	long	rplen;
	long	rpsrc;
};

char		buf[4096];		/* the module image, as read	*/
struct fcb	filefcb;		/* the file the module writes	*/
struct fcb	tryfcb;
struct rsxpb	pb;

static unsigned wordat(off)
int off;
{
	return (((buf[off] & 0xff) << 8) | (buf[off + 1] & 0xff));
}

static long subfn(n)
int n;
{
	pb.rpfunc = (char) n;
	pb.rprsvd = 0;
	return (__bdosl(BDOS_CALLRSX, (long) &pb));
}

static int active()
{
	register long r;

	r = subfn(PUT_FCB);
	return (r != 0 && r != 0xff);
}

/* the CCP splits at blanks only, so the brackets of `[NO ECHO]' arrive
   stuck to the words */
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
	say("usage: PUT [CONSOLE] [OUTPUT] [TO] FILE name [[ECHO]|[NO ECHO]]\r\n");
	say("       PUT [CONSOLE] [OUTPUT] [TO] CONSOLE\r\n");
}

static int	modlen;

/* read PUT.RSX and patch it, before the output file is made */
static int readmod(echo)
int echo;
{
	register int	n, i;
	register char	*p;

	mkfcb(MODNAME, &tryfcb);
	tryfcb.drvcode = _base->resvd1[0] + 1;
	if ((__bdos(BDOS_OPEN, (long) &tryfcb) & 0xff) == 255) {
		say("PUT: cannot find ");
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
		say("PUT: ");
		say(MODNAME);
		say(" is not a resident system extension\r\n");
		return (1);
	}
	buf[H_ECHO] = (char) (echo ? 1 : 0);
	p = (char *) &filefcb;
	for (i = 0; i < sizeof (struct fcb); i++)
		buf[H_FCB + i] = p[i];
	modlen = n;
	return (0);
}

static int attachmod()
{
	pb.rpfunc = RSX_ATTACH;
	pb.rprsvd = 0;
	pb.rporg = 0;
	pb.rplen = modlen;
	pb.rpsrc = (long) buf;
	if (__bdosl(BDOS_CALLRSX, (long) &pb) < 256) {
		say("PUT: no room for the module\r\n");
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
		if (fileat == i)
			continue;		/* the file name itself */
		if (kwis(argv[i], "CONSOLE") || kwis(argv[i], "CON:")
		    || kwis(argv[i], "CONOUT:")) {
			if (fileat != 0)
				continue;
			tocons = 1;
			continue;
		}
		if (kwis(argv[i], "OUTPUT") || kwis(argv[i], "TO"))
			continue;
		if (kwis(argv[i], "FILE")) {
			if (i + 1 >= argc) {
				say("PUT: FILE needs a file name\r\n");
				return (1);
			}
			fileat = i + 1;
			tocons = 0;
			continue;
		}
		if (kwis(argv[i], "ECHO"))
			continue;
		if (kwis(argv[i], "NO") || kwis(argv[i], "NOT")) {
			echo = 0;
			continue;
		}
		if (kwis(argv[i], "SYSTEM"))
			continue;
		if (kwis(argv[i], "PROGRAM")) {
			say("PUT: [PROGRAM] is not implemented.  The CCP is a\r\n");
			say("     transient here and writes through the same RSX\r\n");
			say("     chain as a program, so this PUT is always\r\n");
			say("     [SYSTEM] and cannot capture one and not the other\r\n");
			return (1);
		}
		if (kwis(argv[i], "FILTERED") || kwis(argv[i], "RAW")) {
			say("PUT: [FILTERED] and [RAW] are not implemented; the\r\n");
			say("     output is copied byte for byte as it stands\r\n");
			return (1);
		}
		if (kwis(argv[i], "PRINTER") || kwis(argv[i], "LIST")
		    || kwis(argv[i], "LST:")) {
			say("PUT: the printer is not implemented.  Capturing it\r\n");
			say("     means intercepting BDOS function 5, which\r\n");
			say("     PUT.RSX does not claim\r\n");
			return (1);
		}
		if (kwis(argv[i], "AUXILIARY") || kwis(argv[i], "AUXOUT:")
		    || kwis(argv[i], "AUX:")) {
			say("PUT: this system has no AUX: device to write\r\n");
			return (1);
		}
		if (fileat == 0 && !tocons) {
			fileat = i;		/* PUT name, the short form */
			continue;
		}
		say("PUT: unknown keyword or option: ");
		say(argv[i]);
		say("\r\n");
		return (1);
	}

	if (fileat == 0 && !tocons) {
		usage();
		return (1);
	}

	if (tocons) {			/* PUT CONSOLE: close and stop */
		if (!active()) {
			say("PUT: nothing is being written\r\n");
			return (1);
		}
		/* the close's answer: 0-3 is success */
		if (subfn(PUT_KILL) == 0xff) {
			say("PUT: the file did not close cleanly\r\n");
			return (1);
		}
		say("PUT completed for console\r\n");
		return (0);
	}

	if (active()) {
		say("PUT: a file is already being written; PUT CONSOLE first\r\n");
		return (1);
	}

	mkfcb(argv[fileat], &filefcb);
	for (i = 0; i < sizeof (struct fcb); i++)
		((char *) &tryfcb)[i] = ((char *) &filefcb)[i];
	if (__bdos(BDOS_OPEN, (long) &tryfcb) != 255) {
		__bdos(BDOS_CLOSE, (long) &tryfcb);
		say("PUT: ");
		say(argv[fileat]);
		say(" already exists; erase it first\r\n");
		return (1);
	}
	/*  The module is read and checked BEFORE the output file is
	    created.  Every way PUT.RSX can be refused used to happen after
	    the file existed, which left an empty file of the user's own
	    name behind -- and the test just above then refused the
	    corrected retry because of it.  */
	if (readmod(echo) != 0)
		return (1);

	for (i = 0; i < sizeof (struct fcb); i++)
		((char *) &tryfcb)[i] = ((char *) &filefcb)[i];
	if ((__bdos(BDOS_MAKE, (long) &tryfcb) & 0xff) == 255) {
		say("PUT: no directory space for ");
		say(argv[fileat]);
		say("\r\n");
		return (1);
	}
	__bdos(BDOS_CLOSE, (long) &tryfcb);

	/*  The attach is the only step that can still fail, and it cannot
	    be done before the file exists, so this one takes the file back
	    out again rather than leaving the stub.  */
	if (attachmod() != 0) {
		for (i = 0; i < sizeof (struct fcb); i++)
			((char *) &tryfcb)[i] = ((char *) &filefcb)[i];
		__bdos(BDOS_DELETE, (long) &tryfcb);
		return (1);
	}

	say("Putting console output to file: ");
	sayfcb(&filefcb);
	say(echo ? " [ECHO]\r\n" : " [NO ECHO]\r\n");
	return (0);
}
