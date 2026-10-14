/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * wildt.c - Exercise wildcard and existing-name errors for open, make,
 * attribute, and rename operations.
 */

#include "cpm.h"

static struct fcb	f;
static struct fcb	g;
static char		rn[64];		/* rename's two-name FCB	*/
static char		buf[SECLEN];

static int		bad;

static VOID	mkren();
static VOID	expect();
static VOID	expectok();

int main(argc, argv)
int argc;
char *argv[];
{
	int	r;

	setdma(buf);

	/* ---- a clean slate, then two real files ---- */
	mkfcb("WILDT.TXT", &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb("WILDT2.TXT", &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb("WILDT3.TXT", &f);
	__bdos(BDOS_DELETE, (long) &f);
	mkfcb("WILDT4.TXT", &f);
	__bdos(BDOS_DELETE, (long) &f);

	mkfcb("WILDT.TXT", &f);
	if ((__bdos(BDOS_MAKE, (long) &f) & 0xff) == 0xff) {
		cputs("WILDT: BAD -- cannot create WILDT.TXT\r\n");
		return (1);
	}
	__bdos(BDOS_CLOSE, (long) &f);
	mkfcb("WILDT2.TXT", &f);
	if ((__bdos(BDOS_MAKE, (long) &f) & 0xff) == 0xff) {
		cputs("WILDT: BAD -- cannot create WILDT2.TXT\r\n");
		return (1);
	}
	__bdos(BDOS_CLOSE, (long) &f);

	__bdos(BDOS_ERRMODE, (long) ERRMODE_DISPRET);

	/* ---- check$wild: four functions, five names ---- */

	mkfcb("WILDT?.TXT", &f);
	r = __bdos(BDOS_OPEN, (long) &f);
	expect("fn 15 on a wildcard", r, 9);

	mkfcb("WILDT?.TXT", &f);
	r = __bdos(BDOS_MAKE, (long) &f);
	expect("fn 22 on a wildcard", r, 9);

	mkfcb("WILDT?.TXT", &f);
	r = __bdos(BDOS_SETATTR, (long) &f);
	expect("fn 30 on a wildcard", r, 9);

	/* rename checks the FIRST name (bdos30.asm:1803) ... */
	mkren("WILDT?.TXT", "WILDT3.TXT");
	r = __bdos(BDOS_RENAME, (long) rn);
	expect("fn 23 wildcard 1st name", r, 9);

	/* ... and the SECOND one too (:1817), which is the half a
	   check written from the FCB's own name alone would miss */
	mkren("WILDT.TXT", "WILDT?.TXT");
	r = __bdos(BDOS_RENAME, (long) rn);
	expect("fn 23 wildcard 2nd name", r, 9);

	/* ---- file$exists: a name that is already taken ---- */

	mkfcb("WILDT.TXT", &f);
	r = __bdos(BDOS_MAKE, (long) &f);
	expect("fn 22 on an existing name", r, 8);

	mkren("WILDT.TXT", "WILDT2.TXT");
	r = __bdos(BDOS_RENAME, (long) rn);
	expect("fn 23 onto an existing name", r, 8);

	__bdos(BDOS_ERRMODE, (long) ERRMODE_DEFAULT);

	/* ---- and the ordinary cases still work ---- */

	mkfcb("WILDT.TXT", &f);
	expectok("fn 15 unambiguous", __bdos(BDOS_OPEN, (long) &f));
	__bdos(BDOS_CLOSE, (long) &f);

	mkfcb("WILDT3.TXT", &f);
	expectok("fn 22 new name", __bdos(BDOS_MAKE, (long) &f));
	__bdos(BDOS_CLOSE, (long) &f);

	mkfcb("WILDT3.TXT", &f);
	f.ftype[0] |= 0x80;		/* t1' -- mark it read-only	*/
	expectok("fn 30 set R/O", __bdos(BDOS_SETATTR, (long) &f));
	mkfcb("WILDT3.TXT", &f);
	expectok("fn 30 back to R/W", __bdos(BDOS_SETATTR, (long) &f));

	mkren("WILDT3.TXT", "WILDT4.TXT");
	expectok("fn 23 free name", __bdos(BDOS_RENAME, (long) rn));

	/* WILDT3.TXT must be gone and WILDT4.TXT must be there: a
	   rename that was refused halfway would leave both */
	mkfcb("WILDT3.TXT", &g);
	if ((__bdos(BDOS_OPEN, (long) &g) & 0xff) != 0xff) {
		cputs("WILDT: BAD -- WILDT3.TXT survived the rename\r\n");
		bad++;
	}
	mkfcb("WILDT4.TXT", &g);
	if ((__bdos(BDOS_OPEN, (long) &g) & 0xff) == 0xff) {
		cputs("WILDT: BAD -- WILDT4.TXT is not there\r\n");
		bad++;
	}

	cputs(bad ? "WILDT: FAIL\r\n" : "WILDT: PASS\r\n");
	return (bad != 0);
}


/* Build rename's FCB: the old name in bytes 0-15, the new one at 16
   (src/cmd/gencom.c:520-523 does the same). */
static VOID mkren(from, to)
char *from;
char *to;
{
	register int	i;

	mkfcb(from, &f);
	mkfcb(to, &g);
	for (i = 0; i < 16; i++) {
		rn[i] = ((char *) &f)[i];
		rn[16 + i] = ((char *) &g)[i];
	}
	for (i = 32; i < 36; i++)
		rn[i] = 0;
}


/* Under error mode 0FEh a refusal is <code>/255 (set$aret puts the code
   in the high byte and 0FFh in the low one, bdos30.asm:4373-4380). */
static VOID expect(what, r, code)
char *what;
int r;
int code;
{
	cputs("WILDT: ");
	cputs(what);
	cputs(" -> ");
	putdec((unsigned) ((r >> 8) & 0xff));
	cputs("/");
	putdec((unsigned) (r & 0xff));
	if (((r >> 8) & 0xff) != code || (r & 0xff) != 0xff) {
		cputs("  BAD -- want ");
		putdec((unsigned) code);
		cputs("/255");
		bad++;
	}
	cputs("\r\n");
}


static VOID expectok(what, r)
char *what;
int r;
{
	cputs("WILDT: ");
	cputs(what);
	cputs(" -> ");
	putdec((unsigned) (r & 0xff));
	if ((r & 0xff) == 0xff) {
		cputs("  BAD -- must succeed");
		bad++;
	}
	cputs("\r\n");
}
