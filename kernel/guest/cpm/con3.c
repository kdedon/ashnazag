/*
 * con3.c -- BDOS console: input, line editing, cooked output, paging
 * and flow control (functions 1, 2, 6, 9, 10, 11, 111, 112).
 *
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */

#include "bdos3.h"

int b3_column, b3_conwidth = 80, b3_conpage, b3_conline, b3_pagemode;
int b3_conmode, b3_delim = '$', b3_lstecho;

static int kbchar;		/* one character of type-ahead */
static int brkctr;		/* characters since the last ^S poll */
static int paging;		/* inside the page prompt */
static int stcol, oldend;	/* function 10's line geometry */
static unsigned char chainbuf[RECLEN + 2];
static int chainlen = -1;	/* a function 47 command, or -1 */

#define	CONBRK_POLL	8
#define	RECALL		128

static unsigned char lastlin[RECALL];
static int lastlen;

static void
ctlcexit()
{
	b3_retcode = RC_CTLC;
	b3_wboot();
}

int
b3_constat()
{
	int ch;

	if (b3_conmode & CM_CTLC) {
		if (kbchar)
			return kbchar == CTRLC;
		if (!b3_const())
			return 0;
		kbchar = ch = b3_conin();
		return ch == CTRLC;
	}
	return kbchar ? 1 : b3_const() ? 1 : 0;
}

/* ^S stops output until ^Q; ^C ends the program; ^P toggles the printer */
static void
conbrk()
{
	int ch, stop;

	if (b3_conmode & CM_NOSTOP) {
		brkctr = 0;
		return;
	}
	if (++brkctr < CONBRK_POLL)
		return;
	brkctr = 0;
	stop = 0;
	if (b3_const())
		do {
			if ((ch = b3_conin()) == CTRLC && !(b3_conmode & CM_NOTERM))
				ctlcexit();
			if (ch == CTRLS)
				stop = 1;
			else if (ch == CTRLQ)
				stop = 0;
			else if (ch == CTRLP)
				b3_lstecho = !b3_lstecho;
			else
				kbchar = ch;
		} while (stop);
}

int
b3_getch()
{
	int c;

	if ((c = kbchar) != 0) {
		kbchar = 0;
		return c;
	}
	return b3_conin();
}

static void
pagelf()
{
	char *s;
	int ch;

	if (paging || b3_conpage == 0)
		return;
	if (++b3_conline < b3_conpage)
		return;
	b3_conline = 0;
	if (b3_pagemode)
		return;
	paging = 1;
	for (s = "\r\n\r\nPress RETURN to Continue "; *s; s++)
		b3_conout3(*s);
	ch = b3_getch();
	b3_conout3(CR);
	paging = 0;
	if (ch == CTRLC && !(b3_conmode & CM_NOTERM))
		ctlcexit();
}

void
b3_conout3(ch)
	int ch;
{
	ch &= 0xff;
	conbrk();
	if (ch == LF)
		pagelf();
	b3_conout(ch);
	if (b3_lstecho && !(b3_conmode & CM_RAW))
		b3_list(ch);
	if (ch >= ' ')
		b3_column++;
	else if (ch == CR)
		b3_column = 0;
	else if (ch == BS && b3_column)
		b3_column--;
}

/* tabs expand, and with ctlout control characters echo as ^X */
void
b3_cookdout(ch, ctlout)
	int ch, ctlout;
{
	ch &= 0xff;
	if (ch == TAB && !(!ctlout && (b3_conmode & CM_RAW))) {
		do
			b3_conout3(' ');
		while (b3_column & 7);
		return;
	}
	if (ctlout && ch < ' ') {
		b3_conout3('^');
		ch |= 0x40;
	}
	b3_conout3(ch);
}

int
b3_conin3()
{
	int ch = b3_getch();

	b3_conout3(ch);
	if (ch == CTRLP)
		b3_lstecho = !b3_lstecho;
	return ch;
}

/* function 6: FF input, FE status, FD input, else output */
int
b3_rawio(parm)
	int parm;
{
	parm &= 0xff;
	if (parm == 0xff || parm == 0xfd)
		return b3_getch();
	if (parm == 0xfe)
		return b3_constat() ? 0xff : 0;
	b3_conout(parm);
	return 0;
}

void
b3_prtline(a)
	unsigned long a;
{
	int n;

	for (n = 0; n < 0x10000 && *B3MEM(a) != (char)b3_delim; n++, a++)
		b3_cookdout(*B3MEM(a), 0);
}

/* functions 111/112: block at {long address, word length} */
void
b3_prtblk(ccb, toconsole)
	unsigned long ccb;
	int toconsole;
{
	unsigned long a = b3_get32(ccb), n = b3_get16(ccb + 4);

	for (; n; n--, a++)
		if (toconsole)
			b3_cookdout(*B3MEM(a), 0);
		else
			b3_list(*B3MEM(a) & 0xff);
}

/* function 47: the command line at the DMA, read by the next function 10 */
void
b3_chain()
{
	int n;

	for (n = 0; n < RECLEN && B3MEM(b3_dma)[n]; n++)
		chainbuf[n] = B3MEM(b3_dma)[n];
	chainlen = n;
}

/* ---- function 10 ---- */

static void
newline()
{
	int i;

	b3_conout3(CR);
	b3_conout3(LF);
	for (i = stcol; i; i--)
		b3_conout3(' ');
}

/* column after echoing n characters of the line */
static int
colof(p, n)
	unsigned char *p;
	int n;
{
	int col = stcol, i;

	for (i = 0; i < n; i++)
		if (p[i] == TAB)
			col += 8 - (col & 7);
		else if (p[i] < ' ')
			col += 2;
		else
			col++;
	return col;
}

static void
backto(col)
	int col;
{
	while (b3_column > col)
		b3_conout3(BS);
}

static int
toend(p, cur, len)
	unsigned char *p;
	int cur, len;
{
	while (cur < len)
		b3_cookdout(p[cur++], 1);
	return cur;
}

/* reprint from `from', blank what the old line left, cursor to cur */
static void
repaint(p, from, cur, len)
	unsigned char *p;
	int from, cur, len;
{
	int i;

	for (i = from; i < len; i++)
		b3_cookdout(p[i], 1);
	for (i = 0; b3_column < oldend; i++)
		b3_conout3(' ');
	for (; i; i--)
		b3_conout3(BS);
	backto(colof(p, cur));
}

static void
savelin(p, len)
	unsigned char *p;
	int len;
{
	int i;

	if (len == 0)
		return;
	for (i = 0; i < len && i < RECALL; i++)
		lastlin[i] = p[i];
	lastlen = i;
}

void
b3_readline(a)
	unsigned long a;
{
	unsigned char *b = (unsigned char *)B3MEM(a), *p = b + 2;
	int max = b[0], len = 0, cur = 0, ch, i;

	stcol = b3_column;
	if (chainlen >= 0) {
		len = chainlen < max ? chainlen : max;
		for (i = 0; i < len; i++)
			b3_cookdout(p[i] = chainbuf[i], 1);
		b[1] = len;
		chainlen = -1;
		return;
	}
	b[1] = 0;
	for (;;) {
		ch = b3_getch();
		oldend = colof(p, len);
		if (ch == CTRLC && len == 0 && !(b3_conmode & CM_NOTERM)) {
			b3_cookdout(CTRLC, 1);
			ctlcexit();
		} else if (ch == CR || ch == LF) {
			b3_conout3(CR);
			break;
		} else if (ch == BS || ch == RUB) {
			if (cur) {
				cur--;
				for (i = cur; i + 1 < len; i++)
					p[i] = p[i + 1];
				len--;
				backto(colof(p, cur));
				repaint(p, cur, cur, len);
			}
		} else if (ch == CTRLG) {
			if (cur < len) {
				for (i = cur; i + 1 < len; i++)
					p[i] = p[i + 1];
				len--;
				repaint(p, cur, cur, len);
			}
		} else if (ch == CTRLA) {
			if (cur)
				backto(colof(p, --cur));
		} else if (ch == CTRLF) {
			if (cur < len)
				b3_cookdout(p[cur++], 1);
		} else if (ch == CTRLB) {
			if (cur) {
				cur = 0;
				backto(stcol);
			} else
				cur = toend(p, cur, len);
		} else if (ch == CTRLK) {
			len = cur;
			repaint(p, cur, cur, len);
		} else if (ch == CTRLW) {
			if (cur < len)
				cur = toend(p, cur, len);
			else if (len == 0) {
				for (i = 0; i < lastlen && i < max; i++)
					b3_cookdout(p[i] = lastlin[i], 1);
				len = cur = i;
			}
		} else if (ch == CTRLP)
			b3_lstecho = !b3_lstecho;
		else if (ch == CTRLX) {
			if (cur) {
				for (i = 0; i + cur < len; i++)
					p[i] = p[i + cur];
				len -= cur;
				cur = 0;
				backto(stcol);
				repaint(p, 0, 0, len);
			}
		} else if (ch == CTRLE)
			newline();
		else if (ch == CTRLU) {
			savelin(p, len);
			b3_conout3('#');
			newline();
			len = cur = 0;
		} else if (ch == CTRLR) {
			b3_conout3('#');
			newline();
			cur = toend(p, 0, len);
		} else if (len >= max)
			b3_conout3(0x07);
		else {
			for (i = len; i > cur; i--)
				p[i] = p[i - 1];
			p[cur] = ch;
			len++;
			cur++;
			repaint(p, cur - 1, cur, len);
		}
		b[1] = len;
	}
	b[1] = len;
	savelin(p, len);
}
