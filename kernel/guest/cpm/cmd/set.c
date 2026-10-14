/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * set.c - SET, the CP/M 3 attribute/label/stamping-mode utility, for the
 * C900.
 *
 *	SET [options]			the current drive
 *	SET d: [options]		one drive
 *	SET afn [afn ...] [options]	files
 *	SET [options] afn [afn ...]	the same, options first
 *
 * Drive protection and passwords use the directory label (function 100);
 * file protection and passwords use XFCBs (functions 102 and 103). A default
 * password is installed with function 106. The label's protection bit enables
 * BDOS password checks for the drive.
 *
 * Access and create share one timestamp field, so enabling either disables
 * the other. Wildcards are collected before updates so directory changes do
 * not disturb the active search. The table therefore spans the full directory.
 */

#include "cpm.h"

#define	BDOS_SELDSK	14
#define	BDOS_CURDSK	25
#define	BDOS_SETATTR	30
#define	BDOS_GETUSER	32
#define	BDOS_WRPROT	28
#define	BDOS_RESETDRV	37

#define	LABELTYPE	0x20		/* directory label entry		*/

/*
 * Both tables are sized so they CANNOT overflow on this system, because
 * v3 has no such limit to be faithful to: it interleaves search-next
 * with the writes and processes one name at a time (set.plm:1682-1694,
 * 1770-1779), so any cap here is the port's own and a silent one would
 * leave files unset while reporting success.
 *
 *   MAXFILES  = DRM + 1, the whole directory.  Both drives declare
 *		 DRM 511 (src/bios900.c dpba/dpbb), and one directory
 *		 entry cannot hold two distinct names, so 512 is an
 *		 upper bound on the matches expand() can ever collect.
 *   MAXSPEC   = the most file specs a command tail can carry.  The tail
 *		 is at most 128 bytes (SECLEN buffer, set.plm reads the
 *		 same 80h buffer); a spec plus its separator is at least
 *		 two of them, so 64 cannot be exceeded.
 *
 * The guards below therefore report a can't-happen, but they report it:
 * a drive with a larger DRM would be caught rather than mis-set.
 */
#define	MAXFILES	512		/* wildcard expansion capacity	*/
#define	MAXSPEC		64		/* file specs on one command	*/

/* option numbers -- set.plm:38-55, in the table's own order */
#define	O_ACCESS	0
#define	O_ARCHIVE	1
#define	O_CREATE	2
#define	O_DEFAULT	3
#define	O_DIR		4
#define	O_F1		5
#define	O_F2		6
#define	O_F3		7
#define	O_F4		8
#define	O_NAME		9
#define	O_PASS		10
#define	O_PROT		11
#define	O_RO		12
#define	O_RW		13
#define	O_SYS		14
#define	O_UPDATE	15
#define	O_PAGE		16
#define	O_NOPAGE	17
#define	NOPT		18

/* modifier numbers -- sopt.dcl:29-31, as mods$map holds them (mindex-1) */
#define	M_OFF		0
#define	M_ON		1
#define	M_READ		2
#define	M_WRITE		3
#define	M_DELETE	4
#define	M_NONE		5
#define	NMOD		6

#define	MOD_NONE	0		/* opt$mod columns, sopt.dcl:4-21 */
#define	MOD_ONOFF	1		/*   OFF and ON			*/
#define	MOD_PROT	2		/*   OFF ON READ WRITE DELETE	*/
#define	MOD_STRING	3		/*   an arbitrary string	*/

static char *optname[NOPT] = {
	"ACCESS", "ARCHIVE", "CREATE", "DEFAULT", "DIR", "F1", "F2", "F3",
	"F4", "NAME", "PASSWORD", "PROTECT", "RO", "RW", "SYS", "UPDATE",
	"PAGE", "NOPAGE"
};

static char optkind[NOPT] = {
	MOD_ONOFF, MOD_ONOFF, MOD_ONOFF, MOD_STRING, MOD_NONE,
	MOD_ONOFF, MOD_ONOFF, MOD_ONOFF, MOD_ONOFF,
	MOD_STRING, MOD_STRING, MOD_PROT,
	MOD_NONE, MOD_NONE, MOD_NONE, MOD_ONOFF, MOD_NONE, MOD_NONE
};

static char *modname[NMOD] = {
	"OFF", "ON", "READ", "WRITE", "DELETE", "NONE"
};

static struct fcb	wfcb;		/* the FCB handed to the BDOS	*/
static struct fcb	qfcb;		/* the '?' FCB used for searches */
static char		dirbuf[SECLEN];

static char	optmap[NOPT];		/* option seen			*/
static char	modmap[NOPT];		/* its modifier			*/
static char	labname[16];		/* [NAME=] argument		*/
static int	labnlen;
static char	passname[16];		/* [PASSWORD=] argument		*/
static int	lenpass;
static char	defpass[16];		/* [DEFAULT=] argument		*/
static int	lendef;
static int	passmsg;		/* "assign a password" said once */
/*
 * The DMA buffer function 100 reads a password out of: the FIRST eight
 * bytes are the label's CURRENT password, which the BDOS checks before
 * it changes anything, and the second eight are the new one
 * (bdos30.asm:4917-4922, and set.plm:332-333 which points the DMA at
 * exactly this buffer before every wrlbl).
 */
static char	passwd[16];

static char	names[MAXFILES][11];	/* expanded file names		*/
static int	nnames;
static char	specs[MAXSPEC][15];	/* file specs from the tail	*/
static int	nspecs;
static int	specdrv[MAXSPEC];

static int	cdisk;			/* the drive being worked on	*/
static int	fileref;		/* the options apply to files	*/
static int	pageon, linepage, lineout;
static int	sfamsg, drvmsg, fullmsg;	/* "said that once" flags	*/

static char	tail[SECLEN + 2];
static int	tp;
static char	tok[24];
static int	toklen;
static int	pushed;

static int	scbpb[2];

/* ---------------- BDOS shims ---------------- */

static int scbgetb(off)
int off;
{
	char	*p;

	p = (char *) scbpb;
	p[0] = (char) off;
	p[1] = 0;
	p[2] = 0;
	p[3] = 0;
	return (__bdos(49, (long) scbpb) & 0xff);
}

/* ---------------- output ---------------- */

static VOID crlf2()
{
	cputs("\r\n");
}

/* set.plm:206-228 -- the paging newline */
static VOID nl()
{
	register int	c;

	if (pageon) {
		lineout++;
		if (lineout + 2 > linepage) {
			crlf2();
			crlf2();
			cputs("Press RETURN to continue.");
			while (__bdos(BDOS_CONST, 0L) == 0)
				;
			c = __bdos(BDOS_CONIN, 0L) & 0xff;
			if (c == 3) {
				crlf2();
				__bdos(BDOS_WBOOT, 0L);
			}
			lineout = 1;
			crlf2();
		}
	}
	crlf2();
}

/* set.plm:230-237 -- a new line, then the text */
static VOID pr(s)
char *s;
{
	nl();
	cputs(s);
}

/* set.plm:641-648 */
static VOID eprint(s)
char *s;
{
	pr("ERROR: ");
	cputs(s);
	nl();
}

static VOID die(s)
char *s;
{
	eprint(s);
	crlf2();
	__bdos(BDOS_WBOOT, 0L);
}

/*
 * set.plm:1716-1726 (getfname).  Options are GLOBAL to the whole
 * command: v3's parser flags a `[' that follows a file name (optdel,
 * set.plm:596-600), and getfname then names the file and stops.  A
 * second option group is a user error, not a set of file names -- which
 * is what it silently became here before, so `SET A.TXT [RO] B.TXT
 * [F1=ON]' set RO on A.TXT and then reported five files not found
 * called `[', `F1', `=', `ON' and `]'.
 */
static VOID errglobal()
{
	eprint("Cannot set local options for file.");
	pr("FILE: ");
	if (nspecs > 0)
		cputs(specs[nspecs - 1]);
	nl();
	crlf2();
	__bdos(BDOS_WBOOT, 0L);
}

static VOID showdrive()
{
	conout('A' + cdisk);
	conout(':');
}

/* set.plm:702-712 -- drive, then 11 name bytes with a dot before the type */
static VOID printfn(n)
register char *n;
{
	register int	k;

	showdrive();
	for (k = 0; k < 11; k++) {
		if (k == 8)
			conout('.');
		conout(n[k] & 0x7f);
	}
}

/* set.plm:955-962 */
static VOID putfile(n)
char *n;
{
	nl();
	printfn(n);
	cputs("  ");
}

/* ---------------- the BDOS error path ---------------- */

/*
 * set.plm:717-740.  Function 45 is left in return mode around every
 * write, so a physical error arrives as a code in the high byte instead
 * of taking the machine to the CCP.
 */
static VOID bdoserror(code)
int code;
{
	pr("ERROR: ");
	if (code < 3) {
		showdrive();
		conout(' ');
		if (code == 1)
			cputs("Disk I/O");
		if (code == 2)
			cputs("Drive Read Only");
		crlf2();
		__bdos(BDOS_WBOOT, 0L);
	}
	if (code == 3)
		cputs("Read Only");
	else if (code == 4)
		cputs("Invalid Drive.");
	else if (code == 7)
		cputs("Wrong Password");	/* set.plm:737	*/
	else if (code == 9)
		cputs("? in filespec.");
	else {
		cputs("BDOS code ");
		putdec((unsigned) code);
	}
	nl();
}

/* ---------------- name matching ---------------- */

/*
 * "NAME.TYP" -> 11 blank-padded bytes, '*' expanded to '?' as the CCP
 * does.  The drive letter has already been taken off by the caller.
 */
static VOID mkname(s, out)
register char *s;
register char *out;
{
	register int	i;

	for (i = 0; i < 11; i++)
		out[i] = ' ';
	i = 0;
	while (*s != 0 && *s != '.') {
		if (*s == '*') {
			while (i < 8)
				out[i++] = '?';
			s++;
			continue;
		}
		if (i < 8)
			out[i] = *s;
		i++;
		s++;
	}
	if (*s == '.')
		s++;
	i = 8;
	while (*s != 0) {
		if (*s == '*') {
			while (i < 11)
				out[i++] = '?';
			s++;
			continue;
		}
		if (i < 11)
			out[i] = *s;
		i++;
		s++;
	}
}

static int namematch(pat, n)
register char *pat;
register char *n;
{
	register int	i;

	for (i = 0; i < 11; i++)
		if (pat[i] != '?' && pat[i] != (n[i] & 0x7f))
			return (0);
	return (1);
}

static int samename(a, b)
register char *a;
register char *b;
{
	register int	i;

	for (i = 0; i < 11; i++)
		if ((a[i] & 0x7f) != (b[i] & 0x7f))
			return (0);
	return (1);
}

/* ---------------- directory access ---------------- */

static VOID seldisk(d)
int d;
{
	cdisk = d;
	__bdos(BDOS_SELDSK, (long) d);
}

/*
 * Collect every directory entry matching pat, once each, WITH its
 * attribute bits: an option names one bit and must leave the other six
 * alone, which is what v3 gets for free by pointing its FCB straight at
 * the directory entry (set.plm:755-763, set$up$file).  v3 walks the
 * directory with search-next between writes and restores six SCB search
 * words each time (set.plm:765-788); collecting up front is the same set
 * of files and needs no such surgery.
 */
static VOID expand(pat)
char *pat;
{
	register int	rc, i;
	register char	*e;
	int		user;

	user = __bdos(BDOS_GETUSER, 0xffL) & 0xff;
	setdma(dirbuf);
	qfcb.drvcode = '?';
	rc = __bdos(17, (long) &qfcb) & 0xff;
	while (rc != 0xff) {
		e = &dirbuf[(rc & 3) << 5];
		if ((e[0] & 0xff) == user && namematch(pat, &e[1])) {
			for (i = 0; i < nnames; i++)
				if (samename(names[i], &e[1]))
					break;
			if (i == nnames) {
				if (nnames >= MAXFILES) {
					if (!fullmsg) {
						eprint("Too many matching files for one command.");
						fullmsg = 1;
					}
				} else {
					for (i = 0; i < 11; i++)
						names[nnames][i] = e[i + 1];
					nnames++;
				}
			}
		}
		rc = __bdos(18, 0L) & 0xff;
	}
}

/* the type-20h label entry of the current drive, or 0 (set.plm:913-924) */
static char *readlbl()
{
	register int	rc;
	register char	*e;

	setdma(dirbuf);
	qfcb.drvcode = '?';
	rc = __bdos(17, (long) &qfcb) & 0xff;
	while (rc != 0xff) {
		e = &dirbuf[(rc & 3) << 5];
		if ((e[0] & 0xff) == LABELTYPE)
			return (e);
		rc = __bdos(18, 0L) & 0xff;
	}
	return ((char *) 0);
}

/* ---------------- file attributes ---------------- */

/*
 * set.plm:815-844.  The attribute bits are bit 7 of the name and type
 * bytes: F1..F4 in bytes 1..4, read-only in 9, system in 10, archive
 * in 11, all counted from the FCB drive byte.
 */
static VOID printatt(n)
register char *n;
{
	cputs("set to ");
	cputs((n[9] & 0x80) ? "system (SYS)" : "directory (DIR)");
	cputs(", ");
	cputs((n[8] & 0x80) ? "Read Only (RO)" : "Read Write (RW)");
	conout('\t');
	if (n[10] & 0x80)
		conout('A');
	if (n[0] & 0x80)
		conout('1');
	if (n[1] & 0x80)
		conout('2');
	if (n[2] & 0x80)
		conout('3');
	if (n[3] & 0x80)
		conout('4');
}

/* the attribute the options ask for, applied to one collected name */
static char abyte[5] = { O_F1, O_F2, O_F3, O_F4, O_ARCHIVE };
static char aidx[5]  = { 0, 1, 2, 3, 10 };

static VOID applyatt(n)
register char *n;
{
	register int	i, o, x;

	for (i = 0; i < 5; i++) {
		o = abyte[i] & 0xff;
		x = aidx[i] & 0xff;
		if (optmap[o])
			n[x] = (char) (modmap[o] ? (n[x] | 0x80)
						 : (n[x] & 0x7f));
	}

	if (optmap[O_DIR] && !optmap[O_SYS])
		n[9] &= 0x7f;
	else if (optmap[O_SYS] && !optmap[O_DIR])
		n[9] |= 0x80;

	if (optmap[O_RO] && !optmap[O_RW])
		n[8] |= 0x80;
	else if (optmap[O_RW] && !optmap[O_RO])
		n[8] &= 0x7f;
}

/* set.plm:1309-1333 -- function 30, then the resulting attributes */
static VOID putattributes()
{
	register int	i, k, rc;

	for (k = 0; k < nnames; k++) {
		for (i = 0; i < (int) sizeof (struct fcb); i++)
			((char *) &wfcb)[i] = 0;
		wfcb.drvcode = (char) (cdisk + 1);
		for (i = 0; i < 8; i++)
			wfcb.fname[i] = names[k][i];
		for (i = 0; i < 3; i++)
			wfcb.ftype[i] = names[k][i + 8];
		applyatt(&wfcb.fname[0]);

		rc = __bdos(BDOS_SETATTR, (long) &wfcb);
		if ((rc >> 8) & 0xff) {
			bdoserror((rc >> 8) & 0xff);
			putfile(names[k]);
			continue;
		}
		if ((rc & 0xff) == 0xff) {
			eprint(" File not found");
			putfile(names[k]);
			continue;
		}
		putfile(&wfcb.fname[0]);
		printatt(&wfcb.fname[0]);
	}
}

/* ---------------- drive read-only / read-write ---------------- */

/* set.plm:870-895 */
static VOID setdrvstatus(ro)
int ro;
{
	if (ro)
		__bdos(BDOS_WRPROT, 0L);
	else
		__bdos(BDOS_RESETDRV, (long) (1 << cdisk));
	pr("Drive ");
	showdrive();
	conout(' ');
	cputs("set to ");
	cputs(ro ? "Read Only (RO)" : "Read Write (RW)");
	nl();
}

/* ---------------- the directory label ---------------- */

/*
 * set.plm:654-658, getpasswd.  Read the label's CURRENT password into
 * the first eight bytes of the DMA buffer, so that function 100 can be
 * tried again.  Console echo is on: v3's own prompt echoes too, and a
 * password nobody can see themselves typing is a support call.
 */
static VOID getpasswd()
{
	register int	c;
	register int	i;

	pr("Password ? ");
	for (i = 0; i < 8; i++)
		passwd[i] = ' ';
	i = 0;
	while ((c = conin() & 0x7f) != '\r' && c != '\n' && c != 0) {
		if (c == 8 || c == 0x7f) {	/* backspace	*/
			if (i > 0)
				passwd[--i] = ' ';
			continue;
		}
		if (i < 8)
			passwd[i++] = (char) c;
	}
	nl();
}

/*
 * set.plm:1200-1241.  The Passwds Reqd column reports the label's own
 * password-enable bit, which function 101 now answers with: it is the
 * switch the BDOS arms every password check off (sys/fileio.c ckpass()),
 * so "on" here means the drive really does enforce them.
 */
static VOID showlbl(name, mode)
register char *name;
int mode;
{
	pr("Label for drive ");
	showdrive();
	nl();
	pr("Directory       Passwds  Stamp    Stamp    Stamp");
	pr("Label           Reqd     Create   Access   Update");
	pr("--------------  -------  -------  -------  -------");
	nl();
	printfn(name);
	cputs((mode & DL_PASSWD) ? "    on   " : "    off  ");
	cputs((mode & DL_CREATE) ? "    on   " : "    off  ");
	cputs((mode & DL_ACCESS) ? "    on   " : "    off  ");
	cputs((mode & DL_UPDATE) ? "    on   " : "    off  ");
	nl();
}

/*
 * set.plm:909-949 + 1094-1114 + 1060-1089, done in one place: read the
 * label so its name and stamps survive, fold the mode bits the options
 * ask for into its extent byte, take [NAME=] if given, and write it back
 * with function 100.
 */
static VOID writelabel()
{
	register char	*e;
	register int	i, rc;
	int		mode, dot;
	char		name[11];

	e = readlbl();
	if (e != (char *) 0) {
		for (i = 0; i < 11; i++)
			name[i] = e[i + 1] & 0x7f;
		mode = e[12] & 0xff;
	} else {
		for (i = 0; i < 11; i++)
			name[i] = ' ';
		name[0] = 'L';		/* set.plm:943, label$name	*/
		name[1] = 'A';
		name[2] = 'B';
		name[3] = 'E';
		name[4] = 'L';
		mode = 0;
	}
	mode &= 0xf0;		/* set.plm:935, `turn off set passwd':
				   in a STORED mode byte bit 0 means "the
				   label exists", in the byte handed to
				   function 100 it means "assign a new
				   password".  Reading one and writing it
				   back must not turn the second into the
				   first, so the low nibble goes.	*/

	/* [ACCESS] and [CREATE] share one field: set.plm:1610-1632 */
	if (optmap[O_ACCESS]) {
		if (modmap[O_ACCESS]) {
			mode &= ~DL_CREATE;
			mode |= DL_ACCESS;
		} else
			mode &= ~DL_ACCESS;
	}
	if (optmap[O_CREATE]) {
		if (modmap[O_CREATE]) {
			mode &= ~DL_ACCESS;
			mode |= DL_CREATE;
		} else
			mode &= ~DL_CREATE;
	}
	if (optmap[O_UPDATE]) {
		if (modmap[O_UPDATE])
			mode |= DL_UPDATE;
		else
			mode &= ~DL_UPDATE;
	}
	if (optmap[O_PROT]) {		/* set.plm:1149-1151, protect	*/
		if (modmap[O_PROT])
			mode |= DL_PASSWD;
		else
			mode &= ~DL_PASSWD;
	}
	/*
	 * [PASSWORD=] on a drive, set.plm:1035-1048.  Bit 0 of the mode
	 * byte handed to function 100 asks it to take a new password out
	 * of the second eight bytes of the DMA; the first eight are the
	 * password the label has now, which the BDOS checks first.  An
	 * empty [PASSWORD=] clears the label's password, which is v3's
	 * `if lenpass = 0 then do; passmode = 1; return; end' -- the
	 * assign bit with eight blanks behind it.
	 */
	for (i = 0; i < 16; i++)
		passwd[i] = ' ';
	if (optmap[O_PASS]) {
		mode |= DL_EXISTS;	/* the ASSIGN bit, on the way in */
		for (i = 0; i < lenpass; i++)
			passwd[i + 8] = passname[i];
	}

	if (optmap[O_NAME]) {
		for (i = 0; i < 11; i++)
			name[i] = ' ';
		dot = -1;
		for (i = 0; i < labnlen; i++)
			if (labname[i] == '.') {
				dot = i;
				break;
			}
		if (dot < 0) {
			for (i = 0; i < labnlen && i < 8; i++)
				name[i] = labname[i];
		} else {
			for (i = 0; i < dot && i < 8; i++)
				name[i] = labname[i];
			for (i = 0; i < labnlen - dot - 1 && i < 3; i++)
				name[i + 8] = labname[dot + 1 + i];
		}
	}

	for (i = 0; i < (int) sizeof (struct fcb); i++)
		((char *) &wfcb)[i] = 0;
	wfcb.drvcode = (char) (cdisk + 1);
	for (i = 0; i < 8; i++)
		wfcb.fname[i] = name[i];
	for (i = 0; i < 3; i++)
		wfcb.ftype[i] = name[i + 8];
	wfcb.extent = (char) mode;

	setdma(passwd);
	rc = __bdos(100, (long) &wfcb);
	if (((rc >> 8) & 0xff) == 7) {
		/*
		 * The label already has a password of its own and the
		 * eight blanks above were not it.  set.plm:654-658 asks
		 * for one at the console and tries again; this asks once.
		 * Without this there would be no way to change or remove
		 * a label password once set, which is a trap rather than
		 * a protection.
		 */
		getpasswd();
		rc = __bdos(100, (long) &wfcb);
	}
	setdma(dirbuf);
	if ((rc >> 8) & 0xff) {
		bdoserror((rc >> 8) & 0xff);
		pr("Directory Label ");
		nl();
		return;
	}
	if ((rc & 0xff) == 0xff) {
		eprint("Directory needs to be re-formatted for time/date stamps.\r\n       Please see INITDIR.");
		return;
	}
	mode = __bdos(101, (long) cdisk) & 0xff;
	showlbl(name, mode);
}

/* ---------------- the default password, function 106 ---------------- */

/*
 * set.plm:1005-1020, defaultpass.  Eight blank-padded bytes at the
 * PARAMETER address -- this is the one password in this program that
 * does not travel through the DMA (bdos30.asm:5066-5076).
 *
 * The BDOS keeps it until something replaces it, so it outlives this
 * program: it is what the NEXT program opens a password-protected file
 * with, and with no CCP prompt for a file password it is the only thing
 * that can be.
 */
static char	defbuf[8];

static VOID defaultpass()
{
	register int	i;

	for (i = 0; i < 8; i++)
		defbuf[i] = ' ';
	for (i = 0; i < lendef; i++)
		defbuf[i] = defpass[i];
	__bdos(106, (long) defbuf);
	pr("Default password = ");
	for (i = 0; i < 8; i++)
		conout(defbuf[i]);
	nl();
}

/* ---------------- a file's password, functions 102 and 103 ---------------- */

/* set.plm:1244-1263, show$xfcb */
static VOID showxfcb(pmode)
int pmode;
{
	register int	i;

	cputs("Protection = ");
	if (pmode & 0x80)
		cputs("READ");
	else if (pmode & 0x40)
		cputs("WRITE");
	else if (pmode & 0x20)
		cputs("DELETE");
	else
		cputs("NONE");
	if (pmode & 1) {
		cputs(", Password = ");
		for (i = 8; i < 16; i++)
			conout(passwd[i]);
	}
}

/*
 * One file's XFCB: set$up$xfcb (set.plm:973-993), set$password
 * (:1022-1049), protect's file arm (:1113-1136) and write$xfcb
 * (:1362-1410), in that order because that is do$options' order.
 *
 * Function 102 first, for the mode the file has NOW.  v3 needs it for
 * one decision and so do we: a protection mode may only be set on a file
 * that has a password or is being given one, because a mode with no
 * password is a lock with no key -- the BDOS refuses to write one
 * (bdos30.asm:5005-5010) and this says why.
 *
 * The DMA buffer is the same 16 bytes function 100 uses, and it means
 * the same thing: the first eight are the password the file has now,
 * which the BDOS checks before it changes anything, and the second eight
 * are the new one.  Function 102 leaves the file's create stamp in the
 * first eight, so they are blanked -- eight blanks are "no password
 * offered", which is what a file with no password needs and what makes
 * a file WITH one ask, through error 7 and pass$check (:1276-1300).
 */
static VOID putxfcb(n)
char *n;
{
	register int	i;
	int		rc, pmode, haspw, newpw, m;

	for (i = 0; i < (int) sizeof (struct fcb); i++)
		((char *) &wfcb)[i] = 0;
	wfcb.drvcode = (char) (cdisk + 1);
	for (i = 0; i < 8; i++)
		wfcb.fname[i] = (char) (n[i] & 0x7f);
	for (i = 0; i < 3; i++)
		wfcb.ftype[i] = (char) (n[i + 8] & 0x7f);

	for (i = 0; i < 16; i++)
		passwd[i] = ' ';
	setdma(passwd);
	rc = __bdos(102, (long) &wfcb);
	setdma(dirbuf);
	if ((rc >> 8) & 0xff) {
		bdoserror((rc >> 8) & 0xff);
		putfile(n);
		return;
	}
	if ((rc & 0xff) == 0xff) {
		eprint(" File not found");
		putfile(n);
		return;
	}
	pmode = wfcb.extent & 0xff;
	haspw = (pmode != 0);
	for (i = 0; i < 16; i++)
		passwd[i] = ' ';

	newpw = 0;
	if (optmap[O_PASS]) {			/* set$password	*/
		pmode |= 1;
		if (lenpass == 0)
			pmode = 1;		/* an empty [PASSWORD=]
						   removes the password */
		else {
			newpw = 1;
			for (i = 0; i < lenpass; i++)
				passwd[i + 8] = passname[i];
		}
	}
	if (optmap[O_PROT]) {			/* protect, file arm	*/
		m = modmap[O_PROT] & 0xff;
		if (m == M_READ)
			pmode = 0x80;
		else if (m == M_WRITE)
			pmode = 0x40;
		else if (m == M_DELETE)
			pmode = 0x20;
		else {				/* ON and OFF both mean
						   "no protection" here	*/
			pmode = 1;
			for (i = 8; i < 16; i++)
				passwd[i] = ' ';
		}
		if (newpw)
			pmode |= 1;
	}

	if (pmode > 1 && !haspw && !newpw) {	/* write$xfcb :1366-1381 */
		if (passmsg)
			return;
		passmsg = 1;
		eprint("Assign a password to this file.");
		putfile(n);
		return;
	}
	if (pmode == 1 && newpw)
		pmode |= 0x80;			/* read is v3's default	*/

	wfcb.extent = (char) pmode;
	setdma(passwd);
	rc = __bdos(103, (long) &wfcb);
	if (((rc >> 8) & 0xff) == 7) {		/* pass$check(3)	*/
		putfile(n);
		getpasswd();
		rc = __bdos(103, (long) &wfcb);
	}
	setdma(dirbuf);
	if ((rc >> 8) & 0xff) {
		bdoserror((rc >> 8) & 0xff);
		putfile(n);
		return;
	}
	if ((rc & 0xff) == 0xff) {
		eprint(" File not found");
		pr("       or protection not enabled for disk.");
		nl();
		return;
	}
	if (pmode == 1) {
		/*
		 * The XFCB carries no password and no mode now, so it is
		 * erased -- set.plm:1404-1407.  Bit 7 of FCB byte 5 is
		 * f5', v3's XFCB-ONLY delete (bdos30.asm:1602-1607): the
		 * file itself is not touched, and a delete without it
		 * would erase the file this command was protecting.
		 */
		wfcb.fname[4] |= 0x80;
		__bdos(BDOS_DELETE, (long) &wfcb);
		wfcb.fname[4] &= 0x7f;
	}
	putfile(n);
	showxfcb(pmode);
}

static VOID putxfcbs()
{
	register int	k;

	for (k = 0; k < nnames; k++)
		putxfcb(names[k]);
}

/* ---------------- the command tail ---------------- */

static VOID skipbl()
{
	while (tail[tp] == ' ' || tail[tp] == '\t' || tail[tp] == ',')
		tp++;
}

static int next()
{
	register int	c;

	if (pushed) {
		pushed = 0;
		return (tok[0] & 0xff);
	}
	skipbl();
	toklen = 0;
	tok[0] = 0;
	c = tail[tp] & 0xff;
	if (c == 0)
		return (0);
	if (c == '[' || c == ']' || c == '=' || c == ':') {
		tp++;
		tok[0] = (char) c;
		tok[1] = 0;
		toklen = 1;
		return (c);
	}
	while ((c = tail[tp] & 0xff) != 0 && c != ' ' && c != '\t' && c != ','
	       && c != '[' && c != ']' && c != '=' && c != ':') {
		if (toklen < 23)
			tok[toklen++] = (char) (c >= 'a' && c <= 'z'
						? c - 32 : c);
		tp++;
	}
	tok[toklen] = 0;
	return (tok[0] & 0xff);
}

static VOID pushback()
{
	pushed = 1;
}

/*
 * sopt.inc:37-272.  The input must be a prefix of exactly one entry;
 * anything else -- no match, or an ambiguous one -- is a miss.  Returns
 * the index, or -1.
 */
static int lookup(list, n)
char *list[];
int n;
{
	register int	i, j;
	int		hit;

	hit = -1;
	for (i = 0; i < n; i++) {
		for (j = 0; j < toklen; j++)
			if (list[i][j] == 0 || list[i][j] != tok[j])
				break;
		if (j == toklen && toklen > 0) {
			if (hit >= 0)
				return (-1);
			hit = i;
		}
	}
	return (hit);
}

static VOID optprt(msg, text)
char *msg;
char *text;
{
	eprint(msg);
	pr("Option: ");
	cputs(text);
	nl();
}

static VOID modprt(msg, text)
char *msg;
char *text;
{
	eprint(msg);
	pr("Modifier: ");
	cputs(text);
	nl();
}

/* set.plm:1476-1592 */
static VOID parseopts()
{
	register int	c, k, m;
	register int	i;
	char		optext[24];

	c = next();
	while (c != 0 && c != ']') {
		k = lookup(optname, NOPT);
		for (i = 0; i < 24; i++)
			optext[i] = tok[i];
		if (k < 0) {
			optprt("Unrecognized option.", optext);
			c = next();
			continue;
		}
		m = -1;
		c = next();
		if (c == '=') {
			c = next();
			if (optkind[k] == MOD_NONE) {
				optprt("There are no modifiers for this option.",
				       optext);
				c = next();
				continue;
			}
			if (optkind[k] == MOD_STRING) {
				if (k == O_NAME) {
					labnlen = toklen;
					if (labnlen > 11) {
						labnlen = 11;
						eprint("Only first 11 characters of label name used.");
					}
					for (i = 0; i < labnlen; i++)
						labname[i] = tok[i];
				}
				if (k == O_PASS) {
					lenpass = toklen;
					if (lenpass > 8) {
						lenpass = 8;
						eprint("Only first 8 characters of password used.");
					}
					for (i = 0; i < lenpass; i++)
						passname[i] = tok[i];
				}
				if (k == O_DEFAULT) {	/* set.plm:485	*/
					lendef = toklen;
					if (lendef > 8) {
						lendef = 8;
						eprint("Only first 8 characters of default password used.");
					}
					for (i = 0; i < lendef; i++)
						defpass[i] = tok[i];
				}
				m = 8;
			} else {
				m = lookup(modname, NMOD);
				if (m < 0) {
					modprt("Modifier missing or unrecognizable.",
					       tok);
					c = next();
					continue;
				}
				if (optkind[k] == MOD_ONOFF && m > M_ON) {
					modprt("Not a valid modifier for this option.",
					       tok);
					c = next();
					continue;
				}
			}
			c = next();
		}
		if (m < 0 && optkind[k] != MOD_NONE) {
			optprt("This option needs a modifier.", optext);
			continue;
		}
		optmap[k] = 1;
		if (m >= 0)
			modmap[k] = (char) m;
	}
}

/* ---------------- option dispatch ---------------- */

/*
 * set.plm:1594-1680.  The checks v3 makes before it writes anything, in
 * v3's order, plus the one this port adds for passwords.
 */
static int checkopts()
{
	int	ok;

	ok = 1;
	if (!fileref && optmap[O_PROT] && modmap[O_PROT] > M_ON) {
		/* set.plm:1143-1147, errDrvProt: a drive is protected or
		   not; READ/WRITE/DELETE are modes of a FILE's password */
		eprint("Drive protection must be ON or OFF.");
		optmap[O_PROT] = 0;
		ok = 0;
	}

	if (optmap[O_ACCESS] && optmap[O_CREATE]
	    && modmap[O_ACCESS] && modmap[O_CREATE]) {
		if (fileref)
			eprint("Option only for drives.");
		eprint("Cannot have both create and access time stamps.");
		optmap[O_ACCESS] = optmap[O_CREATE] = 0;
		ok = 0;
	}
	if (optmap[O_DIR] && optmap[O_SYS]) {
		if (!fileref)
			eprint("Option requires a file reference");
		eprint("Cannot set both sys and dir.");
		optmap[O_DIR] = optmap[O_SYS] = 0;
		ok = 0;
	}
	if (optmap[O_RO] && optmap[O_RW]) {
		eprint("Cannot set RO and RW.");
		optmap[O_RO] = optmap[O_RW] = 0;
		ok = 0;
	}

	/* label options against a file reference, and back (set.plm:1064-
	   1070, 1100-1106, 1170-1174) -- said once, as v3 says it */
	if (fileref && (optmap[O_NAME] || optmap[O_ACCESS] || optmap[O_CREATE]
			|| optmap[O_UPDATE])) {
		if (!drvmsg) {
			eprint("Option only for drives.");
			drvmsg = 1;
		}
		optmap[O_NAME] = optmap[O_ACCESS] = 0;
		optmap[O_CREATE] = optmap[O_UPDATE] = 0;
		ok = 0;
	}
	if (!fileref && (optmap[O_ARCHIVE] || optmap[O_DIR] || optmap[O_SYS]
			 || optmap[O_F1] || optmap[O_F2] || optmap[O_F3]
			 || optmap[O_F4])) {
		if (!sfamsg) {
			eprint("Option requires a file reference");
			sfamsg = 1;
		}
		optmap[O_ARCHIVE] = optmap[O_DIR] = optmap[O_SYS] = 0;
		optmap[O_F1] = optmap[O_F2] = optmap[O_F3] = optmap[O_F4] = 0;
		ok = 0;
	}
	return (ok);
}

static int anyattr()
{
	return (optmap[O_ARCHIVE] || optmap[O_DIR] || optmap[O_SYS]
		|| optmap[O_F1] || optmap[O_F2] || optmap[O_F3]
		|| optmap[O_F4] || optmap[O_RO] || optmap[O_RW]);
}

static int anylabel()
{
	return (optmap[O_NAME] || optmap[O_ACCESS] || optmap[O_CREATE]
		|| optmap[O_UPDATE] || optmap[O_PASS] || optmap[O_PROT]);
		/* the drive forms of the last two are label writes like
		   the rest of them: set.plm:1606 and :1149-1151	*/
}

/* ---------------- main ---------------- */

/* a file spec token: "FOO.TXT" or "B:FOO.TXT"; returns 0 for a bare "d:" */
static int takespec()
{
	register int	c;
	int		drv;
	char		word[24];
	register int	i;

	drv = -1;
	for (i = 0; i < 24; i++)
		word[i] = tok[i];
	c = next();
	if (c == ':') {
		if (word[0] < 'A' || word[0] > 'P' || word[1] != 0)
			die("Invalid file name.");
		drv = word[0] - 'A';
		c = next();
		if (c == 0 || c == '[') {
			if (c == '[')
				pushback();
			cdisk = drv;
			return (0);
		}
		for (i = 0; i < 24; i++)
			word[i] = tok[i];
	} else if (c != 0)
		pushback();

	if (nspecs >= MAXSPEC)
		die("Too many file specs on one command.");
	for (i = 0; i < 14 && word[i] != 0; i++)
		specs[nspecs][i] = word[i];
	specs[nspecs][i] = 0;
	specdrv[nspecs] = drv;
	nspecs++;
	return (1);
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, c;
	int		n, k;
	int		xfcbcmd;
	char		pat[11];

	n = _base->buff[0] & 0x7f;
	for (i = 0; i < n; i++)
		tail[i] = _base->buff[i + 1];
	tail[n] = 0;

	cdisk = __bdos(BDOS_CURDSK, 0L) & 0xff;
	pageon = (scbgetb(0x2c) == 0);
	linepage = scbgetb(0x1c);
	if (linepage < 5)		/* GENCPM owns this byte and has	*/
		linepage = 24;		/*  not been run: use v3's default */

	tp = 0;
	c = next();
	if (c == 0)
		die("No options specified.");
	if (c == '[') {
		parseopts();
		while ((c = next()) != 0) {	/* set.plm:1810, getfname */
			if (c == '[')
				errglobal();
			takespec();
		}
	} else {
		while (c != 0 && c != '[') {
			takespec();
			c = next();
		}
		if (c != '[')
			die("No options specified.");
		parseopts();
		while ((c = next()) != 0) {
			if (c == '[')
				errglobal();
			takespec();
		}
	}

	if (optmap[O_PAGE] && optmap[O_NOPAGE]) {
		eprint("Page and nopage option selected.   Nopage in effect.");
		pageon = 0;
	} else if (optmap[O_NOPAGE])
		pageon = 0;
	else if (optmap[O_PAGE])
		pageon = 1;

	fileref = (nspecs > 0);
	if (nspecs > 0 && specdrv[0] >= 0)
		cdisk = specdrv[0];
	seldisk(cdisk);

	checkopts();

	if (optmap[O_DEFAULT])		/* set.plm:1609, do$options	*/
		defaultpass();

	xfcbcmd = fileref && (optmap[O_PASS] || optmap[O_PROT]);

	if (fileref) {
		if (!anyattr() && !xfcbcmd) {
			crlf2();
			return (0);
		}
		for (k = 0; k < nspecs; k++) {
			if (specdrv[k] >= 0 && specdrv[k] != cdisk)
				seldisk(specdrv[k]);
			nnames = 0;
			mkname(specs[k], pat);
			expand(pat);
			if (nnames == 0) {
				eprint(" File not found");
				putfile(pat);
				continue;
			}
			__bdos(BDOS_ERRMODE, (long) ERRMODE_RETURN);
			if (anyattr())
				putattributes();
			if (xfcbcmd)
				putxfcbs();
			__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
		}
	} else {
		if (anylabel()) {
			__bdos(BDOS_ERRMODE, (long) ERRMODE_RETURN);
			writelabel();
			__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);
		}
		if (optmap[O_RO] && !optmap[O_RW])
			setdrvstatus(1);
		else if (optmap[O_RW] && !optmap[O_RO])
			setdrvstatus(0);
	}

	crlf2();
	return (0);
}
