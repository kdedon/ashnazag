/*
 * patch.c -- the routines of the loaded AMS that touch hardware jump to
 * service stubs instead.  Each is found in the Flash copy through
 * AMS's ROM-call table or by its code; a dump without one keeps it,
 * and its access faults.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include "profile.h"
#define W(a) (*(__volatile__ unsigned short *)(TI_ALIAS + ((a) - TI_FLASH)))
#define LW(a) ((unsigned long)W(a) << 16 | W((a) + 2))
#define END arclo		/* code ends where the archive blocks begin */
#define STUB(n) (TI_HOST + 8 * (n))

extern unsigned long arclo;
unsigned char patched[TI_FLASH_SIZE >> 16];	/* blocks the copy holds patches in */

/* the first a in [lo, hi) where the words of p begin, 0 if none; -1 in p matches any */
static unsigned long
find(lo, hi, p, n)
	unsigned long lo, hi;
	int *p, n;
{
	unsigned long a;
	int i;

	for (a = lo; a + 2 * n <= hi; a += 2) {
		for (i = 0; i < n && (p[i] < 0 || W(a + 2 * i) == p[i]); i++)
			;
		if (i == n)
			return a;
	}
	return 0;
}

static void
put(a, w)
	unsigned long a;
	int w;
{
	W(a) = w;
	patched[(a - TI_FLASH) >> 16] = 1;
}

/* jmp (op 0x4ef9) or jsr (0x4eb9) to service n at a */
static void
call(a, op, n)
	unsigned long a;
	int op, n;
{
	put(a, op);
	put(a + 2, (int)(STUB(n) >> 16));
	put(a + 4, (int)STUB(n) & 0xffff);
}

/* the ROM-call table: a count, then entry points nearly all in Flash; ROM call $29A is off (trap #4; rts) */
static unsigned long
romcalls(n)
	unsigned long *n;
{
	unsigned long a, t, e;
	long i, in;

	for (a = TI_FLASH + 0x10000; a + 8 < END; a += 2) {
		*n = LW(a);
		if (*n < 0x400 || *n > 0x900 || a + 4 + 4 * *n > END)
			continue;
		t = a + 4;
		for (i = in = 0; i < *n; i++) {
			e = LW(t + 4 * i);
			if (e >= TI_FLASH && e < END)
				in++;
			else if (e > TI_IO)
				break;
		}
		if (i == *n && in * 10 > *n * 9 && (e = LW(t + 4 * 0x29a)) >= TI_FLASH && e < END &&
		    W(e) == 0x4e44 && W(e + 2) == 0x4e75)
			return t;
	}
	return 0;
}

static int p_scan[] = { 0x48e7, 0x78c0, 0x40e7 };
static int p_mods[] = { 0x203c, 0x0000, 0xfffe };
static int p_rows[] = { 0x7009, 0x323c, 0xfdff, 0x206f, 0x0004 };
static int p_halt[] = { 0x13c0, 0x0060, 0x0005 };		/* move.b d0,$600005 */
static int p_wlen[] = { 0x5284, 0xe28c, -1 };			/* addq.l #1,d4; lsr.l #1,d4; beq */
static int p_wnext[] = { 0x5384, -1 };				/* subq.l #1,d4; bne */
static int p_write[] = { 0x206f, 0x0004, 0x226f, 0x0008, 0x222f, 0x000c };
static int p_move[] = { 0x227c, -1, -1, 0x46fc, 0x2700, 0x32bc, 0xffff, 0x32bc, 0x5050, 0x32bc, 0x2020 };
static int p_erase[] = { 0x226f, 0x0004, 0xb3fc, 0x0060, 0x0000 };
static int p_size[] = { 0x43f9, 0x0021, 0x0000, 0x32bc, 0x9090 };
static int p_batt[] = { 0x13fc, -1, 0x0060, 0x0000 };		/* move.b #n,$600000 */
static int p_dark[] = { 0x13fc, 0x00ff, 0x0060, 0x001c };	/* move.b #$ff,$60001c */
static int p_lit[] = { 0x13fc, 0x0021, 0x0060, 0x001c };	/* move.b #$21,$60001c */

void
patch(te)
	struct ti_enter *te;
{
	unsigned long t, n, a, b, e, lo = TI_FLASH + 0x10000;
	int i;

	if ((t = romcalls(&n)) == 0) {
		fprintf(stderr, "voyage200: no ROM-call table\n");
		return;
	}
	/* key scan: its result word and the modifier table (movea.l #t,a1; asl.w #1,d3) come from its code */
	a = LW(t + 4 * 0x298);
	if (a >= TI_FLASH && a < END && find(a, a + 6, p_scan, 3) == a) {
		for (b = a; b < a + 0x140; b += 2)
			if (W(b) == 0x31c2) {
				te->scanout = (unsigned long)(long)(short)W(b + 2);
				break;
			} else if (W(b) == 0x33c2) {
				te->scanout = LW(b + 2);
				break;
			}
		for (b = a; b < a + 0x140 && (W(b) != 0x227c || W(b + 6) != 0xe343); b += 2)
			;
		if (te->scanout && b < a + 0x140) {
			te->modtab = LW(b + 2);
			call(a, 0x4ef9, TS_SCAN);
		}
	}
	a = LW(t + 4 * 0x299);
	if (te->modtab && a >= TI_FLASH && a < END && find(a, a + 6, p_mods, 3) == a)
		call(a, 0x4ef9, TS_MODS);
	if ((a = find(lo, END, p_rows, 5)) != 0)
		call(a, 0x4ef9, TS_ROWS);
	/* idle halt: the routine copies a stub with the store to RAM; its lea is the entry */
	if ((a = find(lo, END, p_halt, 3)) != 0)
		for (b = a - 2; b > a - 0x40; b -= 2)
			if ((W(b) == 0x41fa && (e = b + 2 + (short)W(b + 2)) <= a && e >= a - 8) ||
			    (W(b) == 0x41f9 && (e = LW(b + 2)) <= a && e >= a - 8)) {
				call(b, 0x4ef9, TS_HALT);
				break;
			}
	/* program: the loop after AMS's range checks becomes jsr service; bra to its exit */
	if ((a = find(lo, END, p_wlen, 3)) != 0 && (W(a + 4) & 0xff00) == 0x6700) {
		a += 6;
		if ((b = find(a, a + 0x60, p_wnext, 2)) != 0 && (W(b + 2) & 0xff00) == 0x6600 &&
		    W(b + 4) == 0x4ef9) {
			call(a, 0x4eb9, TS_PROG);
			put(a + 6, 0x6000 | (int)((b + 4 - (a + 8)) & 0xff));
		}
	}
	/* erase: moveq #n,d0; lea body(pc),a0; bra.w copier, then the body */
	if ((a = find(lo, END, p_erase, 5)) != 0 && W(a - 10) >> 8 == 0x70 && W(a - 8) == 0x41fa &&
	    W(a - 4) == 0x6000)
		call(a - 10, 0x4ef9, TS_ERASE);
	/* program from stack arguments: moveq #n,d0; lea body,a0; lea RAM,a1, then the copy loop and the body */
	if ((a = find(lo, END, p_write, 6)) != 0)
		for (b = a - 2; b > a - 0x30; b -= 2)
			if (W(b) >> 8 == 0x70 && W(b + 2) == 0x41fa && b + 4 + (short)W(b + 4) == a) {
				call(b, 0x4ef9, TS_WRITE);
				break;
			}
	/* a block to a fixed one (movea.l #block,a1), same entry as erase; the service takes a1 from it */
	if ((a = find(lo, END, p_move, 11)) != 0 && W(a - 10) >> 8 == 0x70 && W(a - 8) == 0x41fa &&
	    W(a - 4) == 0x6000) {
		put(a - 10, 0x227c);
		put(a - 8, W(a + 2));
		put(a - 6, W(a + 4));
		call(a - 4, 0x4ef9, TS_MOVE);
	}
	if ((a = find(lo, END, p_size, 5)) != 0)
		call(a, 0x4ef9, TS_SIZE);
	/* the battery comparator, bit 2, stays high through AMS's own stores */
	for (a = lo; (a = find(a, END, p_batt, 4)) != 0; a += 8)
		put(a + 2, W(a + 2) | 4);
	/* LCD off and on: jsr service; nop */
	for (a = lo; (a = find(a, END, p_dark, 4)) != 0; a += 8) {
		call(a, 0x4eb9, TS_DARK);
		put(a + 6, 0x4e71);
	}
	for (a = lo; (a = find(a, END, p_lit, 4)) != 0; a += 8) {
		call(a, 0x4eb9, TS_LIT);
		put(a + 6, 0x4e71);
	}
	for (i = 0, n = 0; i < sizeof patched; i++)
		n += patched[i];
	if (getenv("VOYAGE200_CENSUS"))
		fprintf(stderr, "voyage200: ROM calls at %lx, key word %lx, %lu blocks patched\n",
		    t, te->scanout, n);
}
