/*
 * t_fpimm.c -- FPU instructions with immediate operands, as game timers
 * use them, and ones a 68060 leaves to the kernel (extended immediates,
 * fsin, fetox, fintrz), which must keep fp2-fp7.  Hand-assembled so the
 * compiler cannot fold them.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "t.h"

/* ((1.5 + 2.25) * 4) / 0.5 - 1, every operand a double immediate */
static void
imm_double(out)
double *out;
{
	__asm__ __volatile__("movel %0,%%a0\n"
	    "\t.word 0xf23c,0x5400,0x3ff8,0,0,0\n"	/* fmoved #1.5,%fp0 */
	    "\t.word 0xf23c,0x5422,0x4002,0,0,0\n"	/* faddd #2.25,%fp0 */
	    "\t.word 0xf23c,0x5423,0x4010,0,0,0\n"	/* fmuld #4,%fp0 */
	    "\t.word 0xf23c,0x5420,0x3fe0,0,0,0\n"	/* fdivd #0.5,%fp0 */
	    "\t.word 0xf23c,0x5428,0x3ff0,0,0,0\n"	/* fsubd #1,%fp0 */
	    "\t.word 0xf210,0x7400\n"			/* fmoved %fp0,%a0@ */
	    : : "g" (out) : "a0", "fp0", "memory");
}

/* an unsigned microsecond count to seconds, as games' timers do it */
static void
imm_timer(us, out)
long us;
double *out;
{
	__asm__ __volatile__("movel %0,%%d0\n\tmovel %1,%%a0\n"
	    "\t.word 0xf200,0x4000\n"			/* fmovel %d0,%fp0 */
	    "\t.word 0xf23c,0x5466,0x41f0,0,0,0\n"	/* fdaddd #4294967296,%fp0 */
	    "\t.word 0xf23c,0x5467,0x3eb0,0xc6f7,0xa0b5,0xed8d\n" /* fdmuld #1e-6,%fp0 */
	    "\t.word 0xf23c,0x5464,0x4000,0,0,0\n"	/* fddivd #2,%fp0 */
	    "\t.word 0xf210,0x7400\n"
	    : : "g" (us), "g" (out) : "d0", "a0", "fp0", "memory");
}

/* single, long and extended immediates: 0.75 + 5 + 3 */
static void
imm_other(out)
double *out;
{
	__asm__ __volatile__("movel %0,%%a0\n"
	    "\t.word 0xf23c,0x4400,0x3f40,0\n"		/* fmoves #0.75,%fp0 */
	    "\t.word 0xf23c,0x4022,0,5\n"		/* faddl #5,%fp0 */
	    "\t.word 0xf23c,0x4822,0x4000,0,0xc000,0,0,0\n" /* faddx #3,%fp0 */
	    "\t.word 0xf210,0x7400\n"
	    : : "g" (out) : "a0", "fp0", "memory");
}

/* fp2-fp7 across instructions the 68060 leaves to the kernel */
static void
imm_keep(before, after)
char *before, *after;
{
	__asm__ __volatile__("movel %0,%%a0\n\tmovel %1,%%a1\n"
	    "\t.word 0xf23c,0x4100,0,2,0xf23c,0x4180,0,3\n"	/* fmovel #2,%fp2; #3,%fp3 */
	    "\t.word 0xf23c,0x4200,0,4,0xf23c,0x4280,0,5\n"
	    "\t.word 0xf23c,0x4300,0,6,0xf23c,0x4380,0,7\n"
	    "\t.word 0xf210,0xf03f\n"			/* fmovemx %fp2-%fp7,%a0@ */
	    "\t.word 0xf23c,0x4000,0,1\n"		/* fmovel #1,%fp0 */
	    "\t.word 0xf200,0x000e,0xf200,0x0010\n"	/* fsinx %fp0; fetoxx %fp0 */
	    "\t.word 0xf200,0x0003\n"			/* fintrzx %fp0 */
	    "\t.word 0xf23c,0x4880,0x4000,0,0xc000,0,0,0\n" /* fmovex #3,%fp1 */
	    "\t.word 0xf211,0xf03f\n"			/* fmovemx %fp2-%fp7,%a1@ */
	    : : "g" (before), "g" (after)
	    : "a0", "a1", "fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7", "memory");
}

static int
body()
{
	volatile double d = 0, t = 0, o = 0, e;
	char kb[72], ka[72];
	int bad = 0;

	imm_double((double *)&d);
	if (d != 29.0)
		bad |= 1;
	imm_timer(0xf0000000, (double *)&t);
	e = 4026531840.0 * 1e-6 / 2;
	if (t - e > 1e-9 || e - t > 1e-9)
		bad |= 2;
	imm_other((double *)&o);
	if (o != 8.75)
		bad |= 4;
	imm_keep(kb, ka);
	if (memcmp(kb, ka, sizeof kb) != 0)
		bad |= 8;
	return bad;
}

int
main()
{
	pid_t pid;
	int st;
	long n0 = t_kmem("f60_effadd_n");

	t_init("fpimm", 30);
	pid = fork();
	if (pid == 0)
		_exit(body());
	t_waitchild(pid, &st, 20);
	if (WIFSIGNALED(st))
		t_fail("fp_immediate", "killed by signal %d", WTERMSIG(st));
	else
		t_check("fp_immediate", WEXITSTATUS(st) == 0, "failed checks mask 0x%x",
		    WEXITSTATUS(st));
	if (n0 != -1)
		t_info("fp_immediate", "%ld unimplemented-EA traps", t_kmem("f60_effadd_n") - n0);
	return t_done();
}
