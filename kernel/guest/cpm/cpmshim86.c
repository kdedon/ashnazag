/*
 * cpmshim86.c -- a CP/M-86 .CMD through the 8086 interpreter.  Each
 * group gets a 64 KB host segment at paragraph 0x1000 * (n + 1); the
 * staging segment becomes paragraph 0, the vector table.
 */

#include <sys/types.h>
#include <string.h>
#include "bdos3.h"
#include "cpmshim.h"
#include "shim/i86.h"

#ifndef I86RUN
#define	I86RUN	i86runa		/* the assembly run loop */
#endif

/* the seams' character control block, a host pointer and a length */
struct hccb {
	char	*a;
	i16	n;
};

/* the native BDOS; addr, when set, is the parameter */
int
i86sys(fn, val, addr)
	int fn;
	i16 val;
	char *addr;
{
	struct hccb *h = (struct hccb *)addr;
	static unsigned char ccb[6];

	if (addr && (fn == 111 || fn == 112)) {
		/* ours is {long address, word length} in CP/M memory */
		b3_put32((unsigned long)(ccb - (unsigned char *)b3_mem), (unsigned long)(h->a - b3_mem));
		b3_put16((unsigned long)(ccb - (unsigned char *)b3_mem) + 4, (unsigned long)(h->n & 0xffff));
		addr = (char *)ccb;
	}
	return (int)cpm_bdos3((long)fn, addr ? (unsigned long)(addr - b3_mem) : (unsigned long)(val & 0xffff), 0L);
}

void
shim_fini86()
{
	i86bdosfini();
}

static int
refuse(s)
	char *s;
{
	shim_say(s);
	shim_say("\r\n");
	return 0;
}

void
shim_run86(tail)
	char *tail;
{
	static struct i86 m;
	static struct i86cmd c;
	struct i86in in;
	char hdr[CMD_HDR], *img;
	long n, np;
	int i, rc, brc = B_RUN, slot;

	if ((img = shim_segget()) == 0 || (n = shim_read(img, SEG)) <= 0) {
		refuse("Cannot load the program");
		return;
	}
	memset(hdr, 0, sizeof hdr);
	memcpy(hdr, img, n < CMD_HDR ? (int)n : CMD_HDR);
	memset((char *)&c, 0, sizeof c);
	memset((char *)&m, 0, sizeof m);
	if ((rc = i86hdr(hdr, (i32)n, &c)) == CE_OK && n >= SEG && c.need > n)
		rc = CE_BIG;
	if (rc != CE_OK) {
		refuse(i86cerr(rc));
		return;
	}
	if (c.ng > NSEG - 2) {
		refuse("Too many groups");
		return;
	}
	for (i = 0; i < c.ng; i++) {
		if ((i86sbase[i] = shim_segget()) == 0) {
			refuse("No memory for the program");
			return;
		}
		i86spar[i] = (i16)(0x1000 * (i + 1));
	}
	i86nseg = c.ng;
	m.fl = F_ONES;
	m.lz = LZ_NONE;
	if ((rc = i86place(&c, &m, c.ng)) != CE_OK) {
		refuse(i86cerr(rc));
		return;
	}
	for (i = 0; i < CMD_NGRP; i++)
		if (c.g[i].form != G_NONE && c.g[i].form <= G_AUX4)
			memcpy(i86sbase[c.g[i].sidx], img + c.g[i].foff, (int)i86have(&c.g[i], (i32)n));
	memset(img, 0, (int)SEG);
	i86spar[c.ng] = 0;
	i86sbase[c.ng] = img;
	i86nseg = c.ng + 1;
	slot = c.model == M_8080 ? S_CS : S_DS;
	i86bpage(&c, &m, slot, tail);
	np = 0;
	for (i = 0; i < CMD_NGRP; i++)
		if (c.g[i].form && c.g[i].seg == slot)
			np = c.g[i].npar;
	if (np == 0)
		np = c.g[0].npar;
	m.r[R_SP] = (i16)(np >= CMD_MAXPAR ? 0xfffe : np * CMD_PARA);
	i86segget = shim_segget;
	i86segput = shim_segput;
	i86wait = i86sleep;
	i86gsxdev = 0;
	i86bdosinit(&m);
	for (;;) {
		i86nrun = 16384;
		if ((rc = I86RUN(&m, &in)) == X_OK)
			continue;
		if (rc != X_INT || (brc = i86bdos(&m)) != B_RUN)
			break;
	}
	i86oflush();
	i86bdosfini();
	if (rc != X_WBOOT && brc != B_EXIT) {
		shim_say("\r\nCP/M-86 program stopped: ");
		shim_say(rc == X_INT ? i86berr() : rc == X_HALT ? "HLT" : "an instruction it cannot run");
		shim_say("\r\n");
	}
}
