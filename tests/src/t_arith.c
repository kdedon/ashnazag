/*
 * t_arith.c -- 32/64-bit integer arithmetic and floating point.
 * Floating point runs in a child so a missing FPU shows as a signal.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

extern int _fp_hw;

typedef long long ll;
typedef unsigned long long ull;

/* volatile operands keep the compiler from folding the arithmetic */
static volatile ull va = 0x0123456789ABCDEFLL, vb = 0xFEDCBA98LL, vmax = 0xFFFFFFFFFFFFFFFFLL;
static volatile ll vc = -0x0123456789ABCDEFLL;
static volatile long d351 = 351, d151 = 151, dneg = -351, d7 = 7;
static volatile unsigned long uall = 0xFFFFFFFF, u3 = 3;

static void
test_int()
{
	int ok;

	t_check("div32_var", d351 / d151 == 2 && d351 % d151 == 49, "351/151 = %ld, 351%%151 = %ld",
	    d351 / d151, d351 % d151);
	t_check("div32_neg", dneg / d151 == -2 && dneg % d151 == -49, "-351/151 = %ld rem %ld",
	    dneg / d151, dneg % d151);
	t_check("div32_unsigned", uall / u3 == 0x55555555 && uall % u3 == 0, "0x%lx", uall / u3);
	t_check("mul32_wrap", (unsigned long)(uall * d7) == 0xFFFFFFF9, "0x%lx", (unsigned long)(uall * d7));

	ok = va * vb == 0xacf13578ad05ebe8LL;
	t_check("mul64", ok, "product wrong");
	t_check("div64", va / vb == 0x1249249LL && va % vb == 0x24efe897LL, "quotient/remainder wrong");
	t_check("mul64_signed", (ull)(vc * 7) == 0xf8091a2b3c4d5e77LL, "wrong");
	t_check("div64_signed", vc / 1000003 == -81985283260LL && vc % 1000003 == -637115LL, "wrong");
	t_check("shift64", (va << 13) == 0x68acf13579bde000LL && (va >> 7) == 0x2468acf13579bLL &&
	    ((ull)(-(ll)va) >> 60) == 0xf && (vc >> 60) == -1, "wrong");
	t_check("div64_max", vmax / 10 == 0x1999999999999999LL && vmax % 10 == 5, "wrong");
	t_check("cmp64", va < vb * vb && vc < 0 && (ull)vc > va && (ll)0x100000000LL > (ll)0xFFFFFFFFLL,
	    "wrong");
	{
		volatile ll x = 1;
		int i;
		for (i = 0; i < 62; i++)
			x *= 2;
		t_check("pow2_62", x == 0x4000000000000000LL && (long)(x >> 32) == 0x40000000, "wrong");
	}
}

static int
fp_body()
{
	volatile double a = 1.5, b = 2.25, three = 3.0, x;
	volatile float f = 0.1f;
	char buf[64];
	int bad = 0;

	if (a * b != 3.375) bad |= 1;
	x = 1.0 / three;
	if (fabs(x * three - 1.0) > 1e-15) bad |= 2;
	if (fabs(sqrt(2.0) * sqrt(2.0) - 2.0) > 1e-12) bad |= 4;
	if (fabs(sin(M_PI / 6) - 0.5) > 1e-12) bad |= 8;
	if (fabs(exp(log(10.0)) - 10.0) > 1e-12) bad |= 16;
	if (pow(2.0, 10.0) != 1024.0) bad |= 32;
	if (strtod("3.25", (char **)0) != 3.25 || atof("-1e3") != -1000.0) bad |= 64;
	sprintf(buf, "%.6f %g", 3.14159, 1e10);
	if (strcmp(buf, "3.141590 1e+10") != 0) bad |= 128;
	if ((int)(a * 10) != 15 || (int)-2.7 != -2 || (double)d351 != 351.0) bad |= 256;
	if (f * 10.0f < 0.99f || f * 10.0f > 1.01f) bad |= 512;
	if (floor(-1.5) != -2.0 || ceil(1.2) != 2.0 || fmod(7.5, 2.0) != 1.5) bad |= 1024;
	if (!(a > 1.0) || a < 1.0 || a == b) bad |= 2048;
	return bad;
}

/* FP state must survive context switches: two processes, different values */
static double
fp_loop(seed, n)
double seed;
long n;
{
	double acc = seed, k = seed / 7.0;
	long i;

	for (i = 0; i < n; i++) {
		acc = acc * 0.999999 + k;
		if ((i & 1023) == 0)
			getpid();
	}
	return acc;
}

static void
test_fp()
{
	pid_t pid, p2;
	int st, st2;
	long present = t_kmem("fpu_present");

	t_info("fpu", "_fp_hw %d, kernel fpu_present %ld", _fp_hw, present);
	pid = fork();
	if (pid == 0)
		_exit(fp_body());
	t_waitchild(pid, &st, 20);
	if (WIFSIGNALED(st)) {
		if (present == 0)
			t_skip("fp_basic", "killed by signal %d; kernel reports no FPU", WTERMSIG(st));
		else
			t_fail("fp_basic", "killed by signal %d", WTERMSIG(st));
		return;
	}
	t_check("fp_basic", st == 0, "failed checks mask 0x%x", WEXITSTATUS(st));

	{
		volatile double r1;
		int pp[2];
		pipe(pp);
		pid = fork();
		if (pid == 0) {
			double v = fp_loop(1.0, 200000L);
			write(pp[1], (char *)&v, sizeof v);
			_exit(0);
		}
		p2 = fork();
		if (p2 == 0) {
			double v = fp_loop(3.0, 200000L);
			write(pp[1], (char *)&v, sizeof v);
			_exit(0);
		}
		r1 = fp_loop(5.0, 200000L);
		t_waitchild(pid, &st, 60);
		t_waitchild(p2, &st2, 60);
		{
			double v[2], e1, e3;
			read(pp[0], (char *)&v[0], sizeof v[0]);
			read(pp[0], (char *)&v[1], sizeof v[1]);
			e1 = fp_loop(1.0, 200000L);
			e3 = fp_loop(3.0, 200000L);
			if ((v[0] == e1 && v[1] == e3) || (v[0] == e3 && v[1] == e1)) {
				if (r1 == fp_loop(5.0, 200000L))
					t_pass("fp_context_switch");
				else
					t_fail("fp_context_switch", "parent result changed");
			} else if (present == 0)
				t_skip("fp_context_switch", "results differ; kernel reports no FPU, so FP state is not switched");
			else
				t_fail("fp_context_switch", "concurrent FP results differ from serial ones");
		}
		close(pp[0]);
		close(pp[1]);
	}
}

int
main()
{
	t_init("arith", 55);
	test_int();
	test_fp();
	return t_done();
}
