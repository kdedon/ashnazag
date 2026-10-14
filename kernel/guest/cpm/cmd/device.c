/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * device.c - DEVICE, the CP/M 3 device assignment utility.
 *
 *	DEVICE			names and assignments, then a prompt
 *	DEVICE NAMES		the physical devices
 *	DEVICE VALUES		the current assignments
 *	DEVICE pd		one physical device's characteristics
 *	DEVICE ld		one logical device's assignment
 *	DEVICE CONSOLE[COLUMNS=nnn,LINES=nnn]
 *	DEVICE CONSOLE[PAGESIZE]
 *
 * The BIOS has two character devices and fixed routing: console input,
 * console output and the list device go to the console (CRT), the
 * auxiliary pair to the serial line (SIO).  The table and assignments
 * are those the CP/M-80 BIOS device table reports.  Reassignment, a
 * protocol change or a NULL assignment is parsed and checked, then
 * refused: nothing can carry it out.  Console width and page length are
 * the SCB's and are set.
 */

#include "cpm.h"

#define	CR		13
#define	LF		10
#define	END		0xff
#define	UC(c)		((c) & 0xff)

#define	CONIN_OFF	0x22
#define	CONOUT_OFF	0x24
#define	AUXIN_OFF	0x26
#define	AUXOUT_OFF	0x28
#define	LSTOUT_OFF	0x2a
#define	PAGE_OFF	0x1c
#define	WIDTH_OFF	0x1a

#define	MB_INPUT	0x01
#define	MB_OUTPUT	0x02
#define	MB_INOUT	0x03
#define	MB_SOFTBAUD	0x04

#define	SPACE		5		/* delimiter number of ' '	*/

static char options[] =
	"NAMES~VALUES~HELP~CON:~CONIN:~CONOUT:~LST:~\
AUX:~AUXIN:~AUXOUT:~CONSOLE~KEYBOARD~\
PRINTER~AUXILIARY~AXI:~AXO:\377";
static char optoff[] =
	{ 0, 6, 13, 18, 23, 30, 38, 43, 48, 55, 63, 71, 80, 88, 98, 103, 107 };

static char mods[] =
	"XON~NOXON~NULL~50   ~75   ~110~134~150~300~\
600~1200~1800~2400~3600~4800~7200~\
9600~19200\377";
static char modoff[] =
	{ 0, 4, 10, 15, 21, 27, 31, 35, 39, 43, 47, 52, 57, 62,
	  67, 72, 77, 82, 87 };

static char pageopt[] = "COLUMNS~LINES~PAGESIZE\377";
static char pageoff[] = { 0, 8, 14, 22 };

static char delims[] = { 0, '[', ']', '=', ',', ' ', 0, '\377' };

static char baudrates[] =
	"NONE 50   75   110  134  150  300  600  \
1200 1800 2400 3600 4800 7200 9600 19200";

static char *chartab[] =
	{ "INPUT    ", "OUTPUT   ", "SOFT-BAUD", "SERIAL   ", "XON-XOFF " };

/* the logical device's SCB offset, by option index - 4; below 4 is
 * a pair: 0 and 3 the console, 1 and 2 the auxiliary line */
static char logoff[] = { 0, CONIN_OFF, CONOUT_OFF, LSTOUT_OFF, 1,
	AUXIN_OFF, AUXOUT_OFF, 3, CONIN_OFF, LSTOUT_OFF, 2, AUXIN_OFF,
	AUXOUT_OFF };

struct phys {
	char	name[6];
	char	ch;
	char	baud;
};

static struct phys ptab[16] = {
	{ { 'C', 'R', 'T', ' ', ' ', ' ' }, MB_INOUT, 0 },
	{ { 'S', 'I', 'O', ' ', ' ', ' ' }, MB_INOUT | 0x08, 0 },
	{ { 0 } }
};

static char	tail[SECLEN + 2];
static char	memory[258];		/* interactive input buffer	*/
static char	*begbuf;
static char	*bufp;
static int	buflen;
static int	conwidth;
static int	conpage;
static char	devbits[16];

static int	idx;
static int	endbuf;
static int	delim;

static int	scbpb[2];

static VOID crlf()
{
	cputs("\r\n");
}

static VOID quit()
{
	__bdos(BDOS_WBOOT, 0L);
}

static int getscbbyte(off)
int off;
{
	char	*p;

	p = (char *)scbpb;
	p[0] = (char)off;
	p[1] = 0;
	return (__bdos(49, (long)scbpb) & 0xff);
}

static VOID setscbbyte(off, v)
int off, v;
{
	char	*p;

	p = (char *)scbpb;
	p[0] = (char)off;
	p[1] = (char)0xff;
	p[2] = (char)v;
	p[3] = 0;
	__bdos(49, (long)scbpb);
}

/* The assignment vector the BIOS's routing amounts to. */
static unsigned vecof(off)
int off;
{
	if (off == AUXIN_OFF || off == AUXOUT_OFF)
		return (0x4000);
	return (0x8000);
}

static VOID refuse()
{
	crlf();
	cputs("Device Reassignment Not Supported");
	crlf();
	quit();
}

/* ---- option scanner ---- */

static int separator(c)
int c;
{
	register int	k;

	for (k = 1; UC(delims[k]) != END; k++)
		if (UC(delims[k]) == UC(c))
			return (k);
	return (0);
}

static VOID eatblanks()
{
	while ((delim = separator(*bufp)) == SPACE)
		bufp++;
}

/*
 * Match the word at bufp against a '~'-separated list.  idx is one more
 * than the matched entry, or 0 for no match or an ambiguous one; bufp is
 * left past the word and its delimiter, and endbuf is the word's length.
 */
static char	*slist, *soff;
static int	si, schar, swpos, sliw, sstart, svalid;

static VOID inlist()
{
	register int	i;

	i = sstart;
	swpos = UC(soff[i]);
	while (UC(slist[swpos]) != END) {
		i++;
		idx = i;
		if (UC(slist[swpos]) == schar)
			return;
		swpos = UC(soff[i]);
	}
	idx = 0;
}

static VOID ssetup()
{
	schar = UC(bufp[0]);
	inlist();
	sliw = swpos;
	si = 1;
	schar = UC(bufp[1]);
}

static VOID testletter()
{
	sliw++;
	if (slist[sliw] == '~' || UC(slist[sliw]) != schar)
		svalid = 0;
	si++;
	schar = UC(bufp[si]);
}

static VOID sskip()
{
	schar = UC(bufp[si]);
	delim = separator(schar);
	while (delim < 1 || delim > 6) {
		si++;
		schar = UC(bufp[si]);
		delim = separator(schar);
	}
	endbuf = si;
	bufp += endbuf + 1;
}

static VOID scan(list, off)
char *list, *off;
{
	int	j, lenfound, save;

	slist = list;
	soff = off;
	eatblanks();
	sstart = 0;
	ssetup();
	while (idx != 0) {
		sstart = idx;
		delim = separator(schar);
		svalid = 1;
		while (delim == 0) {
			testletter();
			if (!svalid)
				break;
			delim = separator(schar);
		}
		if (svalid)
			goto good;
		ssetup();
	}
	endbuf = si;
	sskip();
	return;

good:
	endbuf = si;
	lenfound = endbuf;
	save = idx;
	for (;;) {
		sstart = idx;
		ssetup();
		if (idx == 0)
			break;
		if (UC(soff[idx + 1]) - UC(soff[idx]) - 1 != lenfound)
			continue;
		svalid = 1;
		for (j = 1; j <= lenfound; j++) {
			testletter();
			if (!svalid)
				break;
		}
		if (!svalid)
			continue;
		idx = 0;		/* ambiguous */
		sskip();
		return;
	}
	idx = save;
	bufp += endbuf;
	eatblanks();
	if (delim != 0)
		bufp++;
	else
		delim = 5;
}

/* ---- errors ---- */

static char	*sp, *tp;
static int	caret;

static VOID prcmd(size)
int size;
{
	register int	j;

	for (j = 0; j < size; j++)
		conout(*sp++);
	crlf();
	for (j = 0; j < size; j++) {
		if (tp == bufp) {
			caret = 1;
			conout('^');
		} else
			conout(' ');
		tp++;
	}
	crlf();
}

static char *errmsg[] = {
	"Invalid number",
	"End of line expected",
	"Invalid delimiter",
	"Invalid option",
	"Baud rate can not be set for this device",
	"Invalid physical device",
	"Physical device does not have input capability",
	"Physical device does not have output capability",
	"Physical device does not have input/output capability",
	"A NULL device can not be assigned to CONIN",
	"Ambiguous assignments to a NULL device are not allowed"
};

/* Echo the command with a caret under the error, then quit. */
static VOID errors(code)
int code;
{
	int	i, nlines, rem;

	caret = 0;
	sp = tp = begbuf;
	if (conwidth < 40)
		conwidth = 40;
	nlines = buflen / conwidth;
	rem = buflen % conwidth;
	if (code == 1 || code == 2)
		bufp--;
	else
		bufp -= endbuf + 1;
	crlf();
	for (i = 0; i < nlines; i++) {
		tp = sp;
		prcmd(conwidth);
	}
	prcmd(rem);
	cputs(caret ? "Error at the '^'; " : "Error at end of line; ");
	if (conwidth < 63)
		crlf();
	cputs(errmsg[code]);
	crlf();
	quit();
}

/* ---- displays ---- */

static VOID prname(i)
int i;
{
	register int	k;

	for (k = 0; k < 6; k++)
		conout(ptab[i].name[k]);
}

static VOID prbaud(i)
int i;
{
	register int	k, b;

	b = UC(ptab[i].baud);
	if (b > 15)
		b = 0;
	for (k = 0; k < 5; k++)
		conout(baudrates[b * 5 + k]);
}

static VOID prchars(i)
int i;
{
	int	c, n;

	c = UC(ptab[i].ch);
	n = 0;
	if (c & 0x01) { conout('I'); n++; }
	if (c & 0x02) { conout('O'); n++; }
	if (c & 0x08) { conout('S'); n++; }
	if (c & 0x10) { conout('X'); n++; }
	while (n++ < 4)
		conout(' ');
}

static VOID names()
{
	int	i, j, cols;

	crlf();
	cputs("Physical Devices: \r\n");
	cputs("I=Input,O=Output,S=Serial,X=Xon-Xoff\r\n");
	i = conwidth < 40 ? 40 : conwidth;
	cols = i / 20;
	j = 0;
	for (;;) {
		for (i = 1; ; i++) {
			if (ptab[j].name[0] == 0) {
				crlf();
				return;
			}
			prname(j);
			conout(' ');
			prbaud(j);
			conout(' ');
			prchars(j);
			cputs("   ");
			j++;
			if (i >= cols)
				break;
		}
		crlf();
	}
}

static VOID showphys(vec)
unsigned vec;
{
	int	i, k, cols, max, present;

	i = conwidth < 40 ? 40 : conwidth;
	cols = (i - 10) / 7;
	for (max = 0; ptab[max].name[0] != 0 && max < 15; max++)
		;
	present = 0;
	k = 1;
	for (i = 0; i < 15; i++) {
		if (!(vec & (0x8000 >> i)))
			continue;
		if (i > max) {
			cputs("\r\nBad Logical Device Assignment; ");
			cputs("Physical Device Does Not Exist");
			crlf();
			return;
		}
		present = 1;
		prname(i);
		conout(' ');
		if (++k > cols) {
			k = 1;
			crlf();
			cputs("          ");
		}
	}
	if (vec & 1) {
		present = 1;
		cputs("File");
	}
	if (!present)
		cputs("Null Device");
	crlf();
}

static VOID values()
{
	crlf();
	cputs("Current Assignments: \r\n");
	cputs("CONIN:  = ");
	showphys(vecof(CONIN_OFF));
	cputs("CONOUT: = ");
	showphys(vecof(CONOUT_OFF));
	cputs("AUXIN:  = ");
	showphys(vecof(AUXIN_OFF));
	cputs("AUXOUT: = ");
	showphys(vecof(AUXOUT_OFF));
	cputs("LST:    = ");
	showphys(vecof(LSTOUT_OFF));
	crlf();
}

static int search(s)
char *s;
{
	char	str[6];
	int	i, j;

	if (endbuf > 6)
		return (END);
	for (i = 0; i < 6; i++)
		str[i] = i < endbuf ? s[i] : ' ';
	for (i = 0; i < 16 && ptab[i].name[0] != 0; i++) {
		for (j = 0; j < 6; j++)
			if (str[j] != ptab[i].name[j])
				break;
		if (j == 6)
			return (i);
	}
	return (END);
}

/* XON, NOXON or a baud rate for physical device i. */
static VOID option(i)
int i;
{
	idx = 0;
	delim = 1;
	while (delim != 2 && delim != 6) {
		scan(mods, modoff);
		if (idx == 0 || idx == 3)
			errors(3);
		if (idx > 3 && !(ptab[i].ch & MB_SOFTBAUD))
			errors(4);
		refuse();
	}
}

static int number(s, len)
char *s;
int len;
{
	int	i, v;

	if (len > 3)
		errors(0);
	v = 0;
	for (i = 0; i < len; i++, s++) {
		if (*s < '0' || *s > '9')
			errors(0);
		v = v * 10 + *s - '0';
	}
	if (v > 255)
		errors(0);
	return (v);
}

static VOID prbyte(n)
int n;
{
	n &= 0xff;
	if (n >= 100)
		conout('0' + n / 100);
	if (n >= 10)
		conout('0' + n / 10 % 10);
	conout('0' + n % 10);
}

static VOID pageopts()
{
	int	off;

	delim = 1;
	idx = 0;
	while (delim != 2 && delim != 6) {
		scan(pageopt, pageoff);
		if (idx == 0)
			errors(3);
		if (idx == 1 || idx == 2) {
			off = idx == 1 ? WIDTH_OFF : PAGE_OFF;
			if (delim != 3)
				errors(2);
			scan(pageopt, pageoff);
			setscbbyte(off, number(bufp - endbuf - 1, endbuf) - 1);
		}
	}
	conwidth = getscbbyte(WIDTH_OFF);
	conpage = getscbbyte(PAGE_OFF);
	crlf();
	cputs("Console width set to ");
	prbyte(conwidth + 1);
	cputs(" columns\r\nConsole page set to ");
	prbyte(conpage + 1);
	cputs(" lines\r\n");
	crlf();
	quit();
}

static VOID showassign(i)
int i;
{
	int	off;

	off = logoff[i - 4];
	if (off == 0 || off == 3) {
		cputs("CONIN:  = ");
		showphys(vecof(CONIN_OFF));
		cputs("CONOUT: = ");
		showphys(vecof(CONOUT_OFF));
	}
	if (off == 1 || off == 2) {
		cputs("AUXIN:  = ");
		showphys(vecof(AUXIN_OFF));
		cputs("AUXOUT: = ");
		showphys(vecof(AUXOUT_OFF));
	}
	if (off > 3) {
		switch (off) {
		case CONIN_OFF:		cputs("CONIN:  = "); break;
		case CONOUT_OFF:	cputs("CONOUT: = "); break;
		case AUXIN_OFF:		cputs("AUXIN:  = "); break;
		case AUXOUT_OFF:	cputs("AUXOUT: = "); break;
		case LSTOUT_OFF:	cputs("LST:    = "); break;
		}
		showphys(vecof(off));
	}
	crlf();
	quit();
}

/* DEVICE ld, ld[page options], ld=NULL or ld=pd[opt],pd... */
static VOID logical()
{
	int	save, off, eoln, val, c, i;

	save = idx;
	if (delim == 0 || delim == 6)
		showassign(idx);
	if (delim == 1) {
		if (idx == 4 || idx == 5 || idx == 6 || idx == 11)
			pageopts();
		errors(2);
	}
	if (delim != 3)
		errors(2);
	delim = 1;
	idx = 0;
	scan(mods, modoff);
	off = UC(logoff[save - 4]);
	if (idx == 3) {				/* NULL */
		if (off < 4)
			errors(10);
		if (off == CONIN_OFF)
			errors(9);
		refuse();
	}
	for (i = 0; i < 16; i++)
		devbits[i] = 0;
	val = 0;
	for (eoln = 0; !eoln; ) {
		val = search(bufp - endbuf - 1);
		if (val == END)
			errors(5);
		devbits[val] = 1;
		if (delim == 1)
			option(val);
		if (delim == 0 || delim == 6 || (delim == 2 && *bufp == 0))
			eoln = 1;
		if (delim == 2 && *bufp == ',')
			bufp++;
		if (!eoln)
			scan(mods, modoff);
	}
	c = UC(ptab[val].ch);
	if (off < 4) {
		if ((c & MB_INOUT) != MB_INOUT)
			errors(8);
	} else if (off == CONIN_OFF || off == AUXIN_OFF) {
		if (!(c & MB_INPUT))
			errors(6);
	} else if (!(c & MB_OUTPUT))
		errors(7);
	refuse();
}

static VOID showchars(i)
int i;
{
	int	c, k;

	c = UC(ptab[i].ch);
	crlf();
	cputs("Physical Device:  ");
	prname(i);
	crlf();
	cputs("Baud Rate:        ");
	prbaud(i);
	crlf();
	cputs("Characteristics:  ");
	for (k = 0; k < 5; k++, c >>= 1) {
		if (c & 1)
			cputs(chartab[k]);
		else if (k == 3)
			cputs("PARALLEL");
		else
			continue;
		cputs("\r\n                  ");
	}
	quit();
}

static VOID physical(s)
char *s;
{
	int	i;

	i = search(s);
	if (i == END)
		errors(5);
	if (delim == 0 || delim == 6)
		showchars(i);
	if (delim == 1)
		option(i);
	else
		errors(2);
}

static VOID parser()
{
	int	eoln;

	delim = 1;
	idx = 0;
	scan(options, optoff);
	eoln = delim == 0 || delim == 2 || delim == 6;
	if (idx == 0 || idx == 3)
		physical(bufp - endbuf - 1);
	else if (idx == 1 || idx == 2) {
		if (!eoln)
			errors(1);
		if (idx == 1)
			names();
		else
			values();
		return;
	} else
		logical();
	names();
	values();
}

static int blank(s)
char *s;
{
	while (*s == ' ' || *s == '\t')
		s++;
	return (*s == 0);
}

static int upper(c)
int c;
{
	return (c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
}

int main()
{
	int	i, n, v;

	v = __bdos(12, 0L);
	if ((v & 0xff) < 0x30 || ((v >> 8) & 0xff) == 1) {
		cputs("Requires CP/M 3.0");
		quit();
	}
	n = UC(_base->buff[0]);
	if (n > SECLEN - 1)
		n = SECLEN - 1;
	for (i = 0; i < n; i++)
		tail[i] = upper(_base->buff[i + 1]);
	tail[n] = 0;
	conwidth = getscbbyte(WIDTH_OFF);
	conpage = getscbbyte(PAGE_OFF);
	begbuf = bufp = tail;
	buflen = n;
	if (blank(tail)) {
		names();
		values();
		cputs("Enter new assignment or hit RETURN ");
		crlf();
		memory[0] = (char)255;
		memory[1] = 0;
		__bdos(BDOS_RDCONBUF, (long)memory);
		crlf();
		n = UC(memory[1]);
		memory[1] = ' ';
		memory[n + 2] = 0;
		if (blank(memory + 1))
			quit();
		for (i = 0; i < n; i++)
			memory[i + 2] = upper(memory[i + 2]);
		begbuf = bufp = memory + 2;
		buflen = n;
	}
	parser();
	quit();
	return (0);
}
