/* CP/M-86 .CMD loader (DRI System Guide 3.4). The 128-byte header has
 * eight nine-byte descriptors followed by group images. Fields are a form
 * byte, then little-endian paragraph words: length, base, minimum, maximum.
 * One group owns one 64 KB segment; absolute bases and larger requests
 * are refused. Destination memory must be cleared before copying images. */

#include "i86.h"

static i16 gw(h, n)
char *h;
int n;
{
	return ((i16)((h[n] & 0xff) | ((h[n + 1] & 0xff) << 8)));
}

/* Allocate at least supplied length and minimum, growing toward maximum.
 * Maximum zero means 64 KB; minimum wins if above a nonzero maximum. */
static i16 galloc(g)
struct i86grp *g;
{
	register i16 n, want;

	n = g->len;
	if (g->min > n)
		n = g->min;
	if (n == 0)
		n = 1;			/* a placed group is never empty */
	want = g->max == 0 ? (i16)CMD_MAXPAR : g->max;
	if (want > (i16)CMD_MAXPAR)
		want = CMD_MAXPAR;
	if (want > n)			/* grow toward the ask, never shrink */
		n = want;
	if (n > CMD_MAXPAR)
		n = CMD_MAXPAR;
	return (n);
}

/* Validate the header.  A file may end before its groups do, as DRI's
 * loader allows; i86have() says how much is there.
 * Returns CE_OK or a CE_* refusal. */
int i86hdr(hdr, flen, c)
char *hdr;
i32 flen;
struct i86cmd *c;
{
	register struct i86grp *g;
	register int i;
	int seen[10];
	i32 off;

	/* A file shorter than its own header is the first thing a real
	 * directory can hand us -- a truncated copy, or something that is
	 * not a .CMD at all -- and it is the one refusal the caller cannot
	 * make on our behalf, because by the time it has a 128-byte buffer
	 * to pass in, the missing bytes are whatever the buffer held. */
	if (flen < (i32)CMD_HDR)
		return (CE_TRUNC);
	for (i = 0; i < 10; i++)
		seen[i] = 0;
	c->ng = 0;
	c->model = M_8080;
	c->entry = 0;
	off = CMD_HDR;
	for (i = 0; i < CMD_NGRP; i++) {
		g = &c->g[i];
		g->form = (i8)(hdr[i * 9] & 0xff);
		g->len = gw(hdr, i * 9 + 1);
		g->base = gw(hdr, i * 9 + 3);
		g->min = gw(hdr, i * 9 + 5);
		g->max = gw(hdr, i * 9 + 7);
		g->foff = off;
		g->npar = 0;
		/* S_NONE, not S_ES: an unplaced group must not compare
		 * equal to a real slot, and the aux groups of the large
		 * model KEEP this value after placement -- they own a
		 * segment and no segment register.  The caller uses
		 * g->sidx to reach one, never g->seg. */
		g->seg = S_NONE;
		g->sidx = 0;
		g->par = 0;
		if (g->form == G_NONE)
			continue;
		if (g->form > G_AUX4)
			return (CE_FORM);	/* 9 = shared code	*/
		if (seen[g->form])
			return (CE_DUP);
		seen[g->form] = 1;
		c->ng++;
		/* A nonzero A-Base says the group is not
		 * relocatable and must load where it says.  We do not
		 * honour absolute bases -- we cannot, without giving up
		 * the identity between guest and host offsets. */
		if (g->base != 0)
			return (CE_BASE);
		/* More than one 64 KB host segment. */
		if (g->len > CMD_MAXPAR || g->min > CMD_MAXPAR)
			return (CE_BIG);
		if (g->max != 0 && g->max > CMD_MAXPAR)
			return (CE_BIG);
		g->npar = galloc(g);
		if (g->npar == 0)
			return (CE_EMPTY);
		off += (i32)g->len * CMD_PARA;
	}
	if (!seen[G_CODE])
		return (CE_NOCODE);
	c->need = off;

	/* Code-only enters at 0x100 with its base page in the same group.
	 * DATA, EXTRA/STACK, and AUX select small, compact, and large models;
	 * these enter code at zero. */
	c->entry = 0;
	if (seen[G_AUX1] || seen[G_AUX2] || seen[G_AUX3] || seen[G_AUX4])
		c->model = M_LARGE;
	else if (seen[G_EXTRA] || seen[G_STACK])
		c->model = M_COMPACT;
	else if (seen[G_DATA])
		c->model = M_SMALL;
	else {
		c->model = M_8080;
		c->entry = 0x100;
	}
	return (CE_OK);
}

/* Bytes of a group's image a file of flen bytes holds; the rest of the
 * group stays zero. */
i32 i86have(g, flen)
struct i86grp *g;
i32 flen;
{
	register i32 n;

	if (flen <= g->foff)
		return (0);
	n = flen - g->foff;
	if (n > (i32)g->len * CMD_PARA)
		n = (i32)g->len * CMD_PARA;
	return (n);
}

char *i86cerr(e)
int e;
{
	switch (e) {
	case CE_OK:	return ("ok");
	case CE_NOCODE:	return ("no code group");
	case CE_BASE:	return ("nonzero A-Base: not relocatable");
	case CE_BIG:	return ("group wants more than 64K");
	case CE_FORM:	return ("unsupported group form");
	case CE_TRUNC:	return ("file shorter than its header");
	case CE_EMPTY:	return ("group needs no memory");
	case CE_DUP:	return ("two descriptors with the same form");
	case CE_NSEG:	return ("more groups than this machine has segments");
	}
	return ("unknown");
}


i16	i86dgpar;		/* the group the base page and stack are in */
i32	i86dgtop;		/* bytes of it the guest was given	*/

/* Place groups densely in CODE, DATA, EXTRA, STACK, AUX order using
 * caller-supplied segments. CS uses code, DS uses data or code, ES/SS
 * default to DS unless overridden. Aux paragraphs appear in the base
 * page. Code-only aliases all registers. Refuse insufficient segments. */
int i86place(c, m, nseg)
struct i86cmd *c;
struct i86 *m;
int nseg;
{
	/* The canonical order, and the only place it is written down. */
	static i8 gorder[CMD_NGRP] = {
		G_CODE, G_DATA, G_EXTRA, G_STACK,
		G_AUX1, G_AUX2, G_AUX3, G_AUX4
	};
	register struct i86grp *g;
	register int i, j;
	int idx[9];			/* form -> descriptor, or -1	*/
	int next, ds;

	/* Nothing spare until a group has been placed, so that a refused
	 * placement cannot leave the last guest's numbers standing. */
	i86dgpar = 0;
	i86dgtop = 0x10000L;
	for (i = 0; i <= 8; i++)
		idx[i] = -1;
	for (i = 0; i < CMD_NGRP; i++) {
		g = &c->g[i];
		if (g->form > G_NONE && g->form <= G_AUX4)
			idx[g->form] = i;
	}
	if (idx[G_CODE] < 0)
		return (CE_NOCODE);
	if (nseg > I86NSEG)
		nseg = I86NSEG;

	if (c->model == M_8080) {
		if (nseg < 1)
			return (CE_NSEG);
		g = &c->g[idx[G_CODE]];
		g->seg = S_CS;
		g->sidx = 0;
		g->par = i86spar[0];
		for (i = 0; i < 4; i++) {
			m->sr[i] = i86spar[0];
			m->sb[i] = i86sbase[0];
		}
		/* 0x100, not 0, and it has to be set on THIS path too: the
		 * 8080 model is the one that does not start at the bottom
		 * of its group, so leaving IP alone here starts the guest
		 * in the base page.  Found by running a fixture rather
		 * than by reading the code -- the in-memory placement test
		 * checked m->ip only on the small-model path. */
		m->ip = c->entry;
		m->wseg = m->sr[S_SS];
		m->wset = 1;
		i86dgpar = g->par;
		i86dgtop = (i32)(g->npar & 0xffff) * (i32)CMD_PARA;
		return (CE_OK);
	}

	/* One segment per declared group, in gorder, densely. */
	if (nseg < c->ng)
		return (CE_NSEG);
	next = 0;
	for (j = 0; j < CMD_NGRP; j++) {
		i = idx[gorder[j]];
		if (i < 0)
			continue;
		g = &c->g[i];
		g->sidx = (i8) next;
		g->par = i86spar[next];
		g->seg = S_NONE;
		next++;
	}

	/* The registers.  DS falls back to the code group when the file
	 * declares none -- a header with a stack group and no data group
	 * is legal and the base page still has to live somewhere -- and
	 * SS and ES fall back to DS, which is the small model's rule and
	 * the one every DRI file relies on. */
	g = &c->g[idx[G_CODE]];
	g->seg = S_CS;
	m->sr[S_CS] = g->par;
	m->sb[S_CS] = i86sbase[g->sidx];

	ds = idx[G_DATA] >= 0 ? idx[G_DATA] : idx[G_CODE];
	g = &c->g[ds];
	if (idx[G_DATA] >= 0)
		g->seg = S_DS;
	m->sr[S_DS] = g->par;
	m->sb[S_DS] = i86sbase[g->sidx];
	m->sr[S_ES] = m->sr[S_SS] = m->sr[S_DS];
	m->sb[S_ES] = m->sb[S_SS] = m->sb[S_DS];
	i86dgpar = g->par;
	i86dgtop = (i32)(g->npar & 0xffff) * (i32)CMD_PARA;

	if (idx[G_EXTRA] >= 0) {
		g = &c->g[idx[G_EXTRA]];
		g->seg = S_ES;
		m->sr[S_ES] = g->par;
		m->sb[S_ES] = i86sbase[g->sidx];
	}
	if (idx[G_STACK] >= 0) {
		g = &c->g[idx[G_STACK]];
		g->seg = S_SS;
		m->sr[S_SS] = g->par;
		m->sb[S_SS] = i86sbase[g->sidx];
	}

	m->ip = c->entry;
	/* The paragraph the guest is ENTERED with in SS, recorded because a
	 * far transfer to its offset 0 is this environment's warm boot --
	 * i86exec.c wboot().  It is recorded HERE, at the one place that
	 * decides it, and never recomputed from m->sr[S_SS]: the guest
	 * reloads SS in its first ten instructions, so by the time the
	 * epilogue runs the register no longer answers the question. */
	m->wseg = m->sr[S_SS];
	m->wset = 1;
	return (CE_OK);
}

/* Parse FCBs at 0x5c/0x6c using CCP delimiters and wildcards. Tokens split
 * at whitespace; an unqualified name uses default drive zero. */
static int cdelim(c)
int c;
{
	if (c <= ' ')
		return (1);
	switch (c) {
	case '>': case '<': case '.': case ',': case '=': case ':':
	case '+': case '-': case '&': case '/': case '\\': case '|':
	case '(': case ')': case '[': case ']': case ';':
		return (1);
	}
	return (0);
}

/* One field character, the CCP's true_char(): `*' becomes `?' and is
 * NOT consumed, so it fills the rest of its field; a delimiter pads
 * with a blank and is not consumed either. */
static char i86tchar(pp)
char **pp;
{
	register int c;

	c = **pp & 0xff;
	if (c == '*')
		return ('?');
	if (cdelim(c))
		return (' ');
	(*pp)++;
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	return ((char) c);
}

/* One token of the tail into one FCB's drive byte and 11 name bytes. */
static int i86mkfcb(s, f)
char *s, *f;
{
	char *p;
	register int i, c;

	f[0] = 0;
	for (i = 1; i <= 11; i++)
		f[i] = ' ';
	p = s;
	if (*p == '\0')
		return (0);
	c = *p & 0xff;
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	if (c >= 'A' && c <= 'P' && p[1] == ':') {
		f[0] = (char) (c - 'A' + 1);
		p += 2;
	}
	for (i = 1; i <= 8; i++)
		f[i] = i86tchar(&p);
	while (*p && !cdelim(*p & 0xff))
		p++;
	if (*p == '.') {
		p++;
		for (i = 9; i <= 11; i++)
			f[i] = i86tchar(&p);
	}
	return (1);
}

/* Build the data group's (or code-only group's) 256-byte base page:
 * eight SIX-byte group descriptors, FCBs at 0x5c/0x6c, tail at 0x80.
 *
 * Six, not four, and the length is a last byte offset and not a paragraph
 * count.  DRI's DDT86 is the witness: it copies 0x30 bytes out of a
 * loaded program's base page and indexes them by six (DDT86.CMD 06F0h),
 * taking the word at +3 as the group's base PARAGRAPH -- which is also
 * where its own startup reads the code group's base, at 0003h, and the
 * extra group's at 000Fh -- and forming the group's last address from
 * the word at +0 with the byte at +2 as its paragraph carry.  Nothing
 * else in the corpus indexes the table, which is how a four-byte
 * spelling survived this long. */
int i86bpage(c, m, slot, tail)
struct i86cmd *c;
struct i86 *m;
int slot;
char *tail;
{
	register struct i86grp *g;
	register int i, n;
	register char *t;
	int e;
	i32 last;

	for (i = 0; i < 256; i++)
		m->sb[slot][i] = 0;
	for (i = 0; i < CMD_NGRP; i++) {
		g = &c->g[i];
		if (g->form == G_NONE || g->form > G_AUX4)
			continue;
		e = (g->form - 1) * 6;		/* code is first	*/
		if (e > 0x2a)
			continue;
		/* The group's last byte offset, as DRI's loader stores it:
		 * a whole 64 KB group is 00FFFFh. */
		last = (i32)(g->npar & 0xffff) * CMD_PARA - 1;
		m->sb[slot][e] = (char)(last & 0xff);
		m->sb[slot][e + 1] = (char)((last >> 8) & 0xff);
		m->sb[slot][e + 2] = (char)((last >> 16) & 0xff);
		/* g->par, not m->sr[g->seg]: an auxiliary group has a
		 * paragraph and no segment register, and the base page
		 * is the ONLY way its program can learn that paragraph.
		 * For every group that does have a register the two are
		 * the same value, so nothing below the large model moves. */
		m->sb[slot][e + 3] = (char)(g->par & 0xff);
		m->sb[slot][e + 4] = (char)((g->par >> 8) & 0xff);
		/* The 8080 model, in the byte DDT86 tests before it will
		 * start a program at 0100h rather than at zero. */
		if (g->form == G_CODE && c->model == M_8080) {
			m->sb[slot][e + 5] = 1;
			/* DRI repeats the length as the data group's and
			 * leaves its base zero. */
			m->sb[slot][6] = m->sb[slot][e];
			m->sb[slot][7] = m->sb[slot][e + 1];
			m->sb[slot][8] = m->sb[slot][e + 2];
		}
	}
	n = 0;
	if (tail) {
		while (n < 127 && tail[n]) {
			m->sb[slot][0x81 + n] = tail[n];
			n++;
		}
	}
	m->sb[slot][0x80] = (char)n;
	m->sb[slot][0x81 + n] = 0;
	/* The two default FCBs, parsed out of the tail we have just
	 * written -- read back from the base page rather than from
	 * `tail', so the FCB the guest finds is parsed from the tail
	 * the guest finds even when the caller handed us more than the
	 * 127 characters a base page can hold. */
	t = &m->sb[slot][0x81];
	while (*t == ' ' || *t == '\t')
		t++;
	i86mkfcb(t, &m->sb[slot][0x5c]);
	while (*t && *t != ' ' && *t != '\t')
		t++;
	while (*t == ' ' || *t == '\t')
		t++;
	i86mkfcb(t, &m->sb[slot][0x6c]);
	return (n);
}
