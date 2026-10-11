/*
 * cpmshim80.c -- a CP/M-80 .COM through the 8080/Z80 interpreter.
 */

#include <sys/types.h>
#include <string.h>
#include "bdos3.h"
#include "cpmshim.h"
#include "shim/z80.h"

/* the seams' character control block, a host pointer and a length */
struct hccb {
	char	*a;
	z16	n;
};

/* the native BDOS; addr, when set, is the parameter */
int
z80sys(fn, val, addr)
	int fn;
	z16 val;
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

/* no GSX device: every call is refused */
int
z80gsxd(m, g)
	struct z80 *m;
	char *g;
{
	return 1;
}

void
shim_fini80()
{
	z80bdosfini();
}

void
shim_run80(tail)
	char *tail;
{
	static struct z80 m;
	char *mem, *img;
	long n, k;
	int rc, brc = B_RUN;

	if ((mem = shim_segget()) == 0 || (img = shim_segget()) == 0 || (n = shim_read(img, SEG)) <= 0) {
		shim_say("Cannot load the program\r\n");
		return;
	}
	memset((char *)&m, 0, sizeof m);
	if ((rc = z80load(&m, mem, img, n)) != CL_OK) {
		shim_say(z80lerr(rc));
		shim_say("\r\n");
		return;
	}
	shim_segput(img);
	z80tail(&m, tail);
	z80wait = z80sleep;
	z80bdosinit(&m);
	do {
		k = 0;
		rc = z80run(&m, 0x7fffffffL, &k);
	} while (rc == X_HOOK && (brc = z80bdos(&m)) == B_RUN);
	z80oflush();
	z80bdosfini();
	if (brc != B_EXIT) {
		shim_say("\r\nCP/M-80 program stopped: ");
		shim_say(rc == X_HOOK ? z80berr() : rc == X_HALT ? "HLT" : "an instruction it cannot run");
		shim_say("\r\n");
	}
}
