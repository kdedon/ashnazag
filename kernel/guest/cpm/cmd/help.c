/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * help.c - HELP, the CP/M 3 on-line manual reader, for the C900.
 *
 *	HELP			list the top-level topics
 *	HELP topic		print one topic and name its subtopics
 *	HELP topic subtopic ...	descend, up to HELPDEPTH levels
 *	HELP			(then answer the HELP> prompt) the same,
 *				one request per line, empty line to leave
 *
 * HELP.HLP is plain marked-up text. Lines beginning "///n" introduce a
 * topic at nesting level n. Requests match names case-insensitively by prefix.
 * The file is small enough to scan directly, so index-maintenance options are
 * not implemented. BDOS function 111 supplies console paging.
 */

#include "cpm.h"

#define	HELPDEPTH	4	/* '///1' .. '///4'			*/
#define	LINELEN		128	/* longest line we will assemble	*/
#define	NAMELEN		16	/* longest topic name we keep		*/
#define	MAXREQ		HELPDEPTH

#define	CPMEOF		0x1a

/*  The open file, read one 128-byte record at a time.  */

static struct fcb	hfcb;
static char		recbuf[SECLEN];
static int		reclen;		/* bytes of recbuf that are real */
static int		recpos;
static int		athelpeof;

/*  The request: up to MAXREQ names, uppercased.  */

static char	req[MAXREQ][NAMELEN];
static int	nreq;

static char	line[LINELEN + 2];

static int	anytopic;	/* did we print any topic at all?	*/


static int upper(c)
register int c;
{
	if (c >= 'a' && c <= 'z')
		return (c - 'a' + 'A');
	return (c);
}


static VOID crlf()
{
	cputs("\r\n");
}


/*
 * hopen() -- open HELP.HLP.  v3 looks on the default drive and then on
 * A:; so do we.  A second open of the same file when the default drive
 * already is A: is harmless and saves a drive-number test.
 */

static int hopen()
{
	mkfcb("HELP.HLP", &hfcb);
	if ((__bdos(BDOS_OPEN, (long) &hfcb) & 0xff) != 0xff)
		return (1);
	mkfcb("A:HELP.HLP", &hfcb);
	if ((__bdos(BDOS_OPEN, (long) &hfcb) & 0xff) != 0xff)
		return (1);
	return (0);
}


static VOID hrewind()
{
	hfcb.extent = 0;
	hfcb.s1 = 0;
	hfcb.s2 = 0;
	hfcb.cur_rec = 0;
	__bdos(BDOS_OPEN, (long) &hfcb);
	reclen = 0;
	recpos = 0;
	athelpeof = 0;
}


/*
 * hgetc() -- one byte of the file, -1 at end.
 *
 * A 1Ah ends the file: that is the CP/M text-EOF byte the last partial
 * record of a text file is padded with.  So does a NUL: the image
 * builder pads by extension and .HLP is not on its text list, so
 * HELP.HLP is zero-padded on the image.  Adding HLP there would repad
 * every .HLP and move the image; taking both bytes here costs one
 * comparison and moves nothing.
 */

static int hgetc()
{
	register int	c;

	if (athelpeof)
		return (-1);
	if (recpos >= reclen) {
		setdma(recbuf);
		if (__bdos(BDOS_READSEQ, (long) &hfcb) != 0) {
			athelpeof = 1;
			return (-1);
		}
		reclen = SECLEN;
		recpos = 0;
	}
	c = recbuf[recpos++] & 0xff;
	if (c == CPMEOF || c == 0) {
		athelpeof = 1;
		return (-1);
	}
	return (c);
}


/*
 * hgetline() -- the next line into line[], without its CR/LF, NUL
 * terminated.  Returns 0 at end of file.  A line longer than LINELEN is
 * split rather than truncated, which is what a reader wants and costs
 * nothing here.
 */

static int hgetline()
{
	register int	c;
	register int	n;

	n = 0;
	for (;;) {
		c = hgetc();
		if (c < 0) {
			line[n] = 0;
			return (n > 0);
		}
		if (c == '\n') {
			line[n] = 0;
			return (1);
		}
		if (c == '\r')
			continue;
		if (n < LINELEN)
			line[n++] = (char) c;
		else {
			line[n] = 0;
			return (1);
		}
	}
}


/*
 * marker() -- if line[] is a topic marker, return its level (1..9) and
 * copy the subject into name[]; otherwise return 0.  v3 takes the
 * marker from the head of the line; help.dat indents its markers two
 * spaces, so leading blanks are skipped here and a v3 file still reads.
 */

static int marker(name)
register char *name;
{
	register char	*p;
	register int	i;
	int		lev;

	for (p = line; *p == ' ' || *p == '\t'; p++)
		;
	if (p[0] != '/' || p[1] != '/' || p[2] != '/')
		return (0);
	p += 3;
	if (*p < '1' || *p > '9')
		return (0);
	lev = *p++ - '0';
	while (*p == ' ')
		p++;
	for (i = 0; i < NAMELEN - 1 && *p != 0 && *p != ' ' && *p != '\t';
	     i++)
		name[i] = (char) upper(*p++);
	name[i] = 0;
	return (i > 0 ? lev : 0);
}


/*
 * pfxmatch() -- does the request match this topic name?  v3 matches a
 * request against the leading characters of the name, so `HELP SUB'
 * finds SUBMIT.  Both sides are already upper case.
 */

static int pfxmatch(r, name)
register char *r;
register char *name;
{
	while (*r != 0) {
		if (*r != *name)
			return (0);
		r++;
		name++;
	}
	return (1);
}


/*
 * The scan.  One pass over the file answers everything: we walk the
 * markers keeping the current path (path[1..HELPDEPTH]), and a marker
 * is INSIDE the request when the first nreq levels of the path match
 * the request.  Text lines are printed while we are inside the topic
 * itself; markers exactly one level below it are collected as its
 * subtopic list.
 *
 * Doing it in one pass rather than with a seek per level is the whole
 * of the no-index deviation above.
 */

static char	path[HELPDEPTH + 2][NAMELEN];

static VOID scan()
{
	char	name[NAMELEN];
	int	depth;			/* level of the last marker	*/
	int	inside;			/* printing this topic's text	*/
	int	within;			/* the path matches the request	*/
	int	subhdr;			/* have we printed `Subtopics:'	*/
	register int	lev;
	register int	i;

	for (i = 0; i <= HELPDEPTH + 1; i++)
		path[i][0] = 0;
	depth = 0;
	inside = 0;
	subhdr = 0;

	hrewind();
	while (hgetline()) {
		lev = marker(name);
		if (lev == 0) {
			if (inside) {
				cputs(line);
				crlf();
			}
			continue;
		}
		if (lev > HELPDEPTH)
			continue;
		for (i = 0; i < NAMELEN; i++)
			path[lev][i] = name[i];
		for (i = lev + 1; i <= HELPDEPTH; i++)
			path[i][0] = 0;
		depth = lev;

		/*  is this marker at or below the requested topic?  */
		within = 1;
		for (i = 0; i < nreq; i++)
			if (!pfxmatch(req[i], path[i + 1])) {
				within = 0;
				break;
			}

		/*  Text belongs to the requested topic only while the
		    last marker seen IS that topic: a marker for anything
		    else -- a deeper subtopic, a sibling, another
		    top-level topic -- ends its text.  */
		inside = (within && depth == nreq);

		if (!within)
			continue;
		if (depth == nreq) {		/* the topic itself	*/
			anytopic = 1;
			subhdr = 0;
			crlf();
			continue;
		}
		if (depth == nreq + 1) {	/* one of its subtopics	*/
			anytopic = 1;
			if (!subhdr) {
				subhdr = 1;
				crlf();
				cputs(nreq == 0 ? "Topics:" : "Subtopics:");
				crlf();
			}
			cputs("    ");
			cputs(path[depth]);
			crlf();
			continue;
		}
		/*  deeper than one below the request: not ours	*/
	}
	if (anytopic)
		crlf();
}


/*
 * split() -- break a request into up to MAXREQ names, upper cased.  The
 * CCP has already upper-cased the command tail, but HELP is also driven
 * from its own prompt, where nothing has.
 */

static VOID split(s)
register char *s;
{
	register int	i;

	nreq = 0;
	while (*s != 0 && nreq < MAXREQ) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == 0)
			break;
		for (i = 0; i < NAMELEN - 1 && *s != 0 && *s != ' '
		     && *s != '\t'; i++)
			req[nreq][i] = (char) upper(*s++);
		req[nreq][i] = 0;
		nreq++;
		while (*s != 0 && *s != ' ' && *s != '\t')
			s++;
	}
}


/*
 * ask() -- the HELP> prompt.  Function 10 reads the line into the 8080
 * buffer form: max, len, then the text.
 */

static char	conbuf[NAMELEN * MAXREQ + 8];

static int ask()
{
	register int	n;

	crlf();
	cputs("HELP> ");
	conbuf[0] = (char) (sizeof conbuf - 3);
	conbuf[1] = 0;
	__bdos(BDOS_RDCONBUF, (long) conbuf);
	crlf();
	n = conbuf[1] & 0xff;
	if (n > (int) sizeof conbuf - 3)
		n = sizeof conbuf - 3;
	conbuf[2 + n] = 0;
	return (n);
}


main()
{
	register char	*tail;
	register int	n;
	int		first;

	if (!hopen()) {
		cputs("HELP: No HELP.HLP file on this drive or on A:.\r\n");
		__bdos(BDOS_RETCODE, 0x0001L);
		return (1);
	}

	tail = &_base->buff[1];
	n = _base->buff[0] & 0xff;
	if (n > 126)
		n = 126;
	tail[n] = 0;

	/*  v3 answers the command tail first and then prompts.  With no
	    tail the request is empty, which is the request that lists
	    the top-level topics -- so a bare HELP prints the topic list
	    and then prompts, and nothing needs a special case.  An empty
	    request AT THE PROMPT ends the session instead; that is the
	    one asymmetry, and it is v3's.  */

	first = 1;
	for (;;) {
		if (first) {
			split(tail);
			first = 0;
		} else {
			if (ask() == 0)
				break;
			split(&conbuf[2]);
			if (nreq == 0)
				break;
		}
		anytopic = 0;
		scan();
		if (!anytopic)
			cputs("No information on that topic.\r\n");
	}
	return (0);
}
