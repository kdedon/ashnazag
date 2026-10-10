/*
 * cpmshim.c -- CP/M-80 .COM and CP/M-86 .CMD programs inside CP/M-68K.
 *
 * The BDOS loads a small 68K program in their place (pgmld); it calls
 * BIOS function B3_SHIM, which lands here with the base page.  The
 * guest runs in an interpreter, and its BDOS calls go to our BDOS.  A
 * warm boot that leaves the BDOS early (^C, a fatal error) ends the run
 * through cpm_shimabort().
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bdos3.h"
#include "cpmshim.h"

static int active;		/* 0, SHIM_80 or SHIM_86 */
static char *seg[NSEG];
static int nseg;

/* 64 KB the guest owns, zeroed */
char *
shim_segget()
{
	char *p;

	if (nseg == NSEG || (p = malloc(SEG)) == 0)
		return 0;
	memset(p, 0, SEG);
	return seg[nseg++] = p;
}

int
shim_segput(p)
	char *p;
{
	int i;

	for (i = 0; i < nseg; i++)
		if (seg[i] == p) {
			free(p);
			seg[i] = seg[--nseg];
			return 1;
		}
	return 0;
}

static void
segfree()
{
	while (nseg > 0)
		free(seg[--nseg]);
}

void
shim_say(s)
	char *s;
{
	while (*s)
		b3_conout(*s++);
}

/* the program's file, read whole: its length, or -1 */
long
shim_read(buf, max)
	char *buf;
	long max;
{
	long n;
	int fd;

	if ((fd = hf_open(b3_shimdrv, &b3_shimh, 0)) < 0 || lseek(fd, 0L, 0) != 0)
		return -1;
	n = b3_shimh.size < max ? b3_shimh.size : max;
	return read(fd, buf, (unsigned)n) == n ? n : -1;
}

/* BIOS B3_SHIM: run the program pgmld put aside, then return to its 68K stub */
long
cpm_shim(bp)
	unsigned long bp;
{
	char tail[128];
	int n, kind = b3_shim;

	b3_shim = 0;
	if (!kind)
		return 0;
	n = *(unsigned char *)B3MEM(bp + 0x80);
	if (n > 126)
		n = 126;
	memcpy(tail, B3MEM(bp + 0x81), n);
	tail[n] = 0;
	active = kind;
	if (kind == SHIM_80)
		shim_run80(tail);
	else
		shim_run86(tail);
	active = 0;
	segfree();
	return 0;
}

/* a warm boot from inside the BDOS: the run is over */
void
cpm_shimabort()
{
	if (active == SHIM_80)
		shim_fini80();
	else if (active == SHIM_86)
		shim_fini86();
	active = 0;
	segfree();
}
