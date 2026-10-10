/*
 * shimrun -- a CP/M-80 or CP/M-86 program through the shim and our BDOS
 * on the build host, A: being a directory.
 *
 *	shimrun dir PROG.COM|PROG.CMD [tail]
 *
 * The seams hand our BDOS host pointers as 32-bit CP/M addresses, as on
 * the target; so CP/M's memory is 32-bit addressable here too and the
 * program is linked without PIE.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include "bdos3.h"
#include "cpmshim.h"

#define	MEMSZ	0x400000L
#define	BP	0x20000L

char *b3_mem;
static char *in = "";
static jmp_buf wb;

int b3_conin() { return *in ? *in++ : 0x1a; }
int b3_const() { return *in != 0; }
void b3_conout(c) int c; { putchar(c); }
void b3_list(c) int c; { }
long b3_bios(fn, d1, d2) long fn, d1, d2; { return 0; }
void b3_wboot() { cpm_shimabort(); longjmp(wb, 1); }

int
main(argc, argv)
	int argc;
	char **argv;
{
	char n[11], *p, *q, *mem;
	int i, n0;

	if (argc < 3)
		return 2;
	if ((mem = mmap(0, MEMSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
	    -1, 0)) == MAP_FAILED)
		return 1;
	b3_mem = 0;
	hf_setdrive(0, argv[1], HF_AREAS);
	b3_init((unsigned long)mem + 0x2000, (unsigned long)mem + 0x30000, (unsigned long)mem + MEMSZ,
	    (unsigned long)mem + 0x8000);
	memset(n, ' ', 11);
	p = argv[2];
	q = strchr(p, '.');
	for (i = 0; p[i] && p + i != q && i < 8; i++)
		n[i] = p[i];
	for (i = 0; q && q[i + 1] && i < 3; i++)
		n[8 + i] = q[i + 1];
	if (hf_find(0, 0, n, &b3_shimh) < 0) {
		fprintf(stderr, "shimrun: no %s in %s\n", argv[2], argv[1]);
		return 1;
	}
	b3_shim = shimtype(n);
	b3_shimdrv = 0;
	p = argc > 3 ? argv[3] : "";
	n0 = strlen(p);
	mem[BP + 0x80] = n0 + 1;
	mem[BP + 0x81] = ' ';
	memcpy(mem + BP + 0x82, p, n0);
	if (setjmp(wb) == 0)
		cpm_shim((unsigned long)mem + BP);
	return 0;
}
