/*
 * x87.c -- the floating-point unit, for programs that find one (WIN87EM
 * fixups turned into x87 instructions).  Not yet: every x87 instruction
 * raises #NM, which the host reports.
 */

#include "x86.h"

void
x87_init(c)
	struct x86 *c;
{
	c->x87 = 0;
}

void
x87_exec(c, op, mod, reg, rm, seg, off)
	struct x86 *c;
	int op, mod, reg, rm, seg;
	u32 off;
{
	x86_exception(c, X_NM, 0);
}
