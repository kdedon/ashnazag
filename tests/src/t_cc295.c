/*
 * t_cc295.c -- gcc 2.95.3 code against the kernel's compiler: struct
 * layout, struct returns, float/double/long long arguments, varargs and
 * callbacks in both directions (cc295abi.c is the 2.95.3 half), then
 * t_arith built entirely with 2.95.3.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "t.h"
#include "cc295.h"

struct s8
old_s8(int k)
{
	struct s8 r;

	r.a = k * 3;
	r.b = k + 1;
	return r;
}

struct big
old_big(int n)
{
	struct big r;
	int i;

	for (i = 0; i < 9; i++)
		r.v[i] = n + i;
	r.d = n;
	return r;
}

double
old_vsum(int n, ...)
{
	va_list ap;
	double s = 0;

	va_start(ap, n);
	while (n-- > 0) {
		s += va_arg(ap, int);
		s += va_arg(ap, double);
	}
	va_end(ap);
	return s;
}

double
old_cb(double x, int k, struct s8 p)
{
	return x * k + p.a - p.b;
}

static void
test_layout()
{
	struct layout l;

	abi_layout(&l);
	t_check("layout_mix", l.size_mix == sizeof(struct mix) && l.off_d == offsetof(struct mix, d) &&
	    l.off_s == offsetof(struct mix, s) && l.off_i == offsetof(struct mix, i) &&
	    l.off_tail == offsetof(struct mix, tail), "size %d d@%d s@%d i@%d tail@%d, expected %d %d %d %d %d",
	    l.size_mix, l.off_d, l.off_s, l.off_i, l.off_tail, (int)sizeof(struct mix),
	    (int)offsetof(struct mix, d), (int)offsetof(struct mix, s), (int)offsetof(struct mix, i),
	    (int)offsetof(struct mix, tail));
	t_check("layout_misc", l.size_s3 == sizeof(struct s3) && l.size_bits == sizeof(struct bits) &&
	    l.size_ld == sizeof(long double) && l.size_ll == sizeof(long long),
	    "s3 %d bits %d long double %d long long %d", l.size_s3, l.size_bits, l.size_ld, l.size_ll);
}

static void
test_calls()
{
	struct mix m;
	struct s4 r4;
	struct s8 r8;
	struct s3 r3;
	struct big b, rb;
	struct bits bf;
	char buf[80];
	int i, ok;

	m.c = 1; m.d = 1.5; m.s = 10; m.i = 7; m.tail = 0x11;
	m = abi_mix(2, 2.0, m, 0.25f);
	t_check("struct_arg_ret", m.c == 3 && m.d == 3.25 && m.s == 8 && m.i == 14 && m.tail == 0x4b,
	    "%d %g %d %d 0x%x", m.c, m.d, m.s, m.i, m.tail);
	r4 = abi_s4(3, -5);
	r8 = abi_s8(10, 4);
	r3 = abi_s3(7);
	t_check("small_struct_ret", r4.a == -5 && r4.b == 3 && r8.a == 14 && r8.b == 6 &&
	    r3.a == 7 && r3.b == 8 && r3.c == 9, "s4 %d,%d s8 %d,%d s3 %d,%d,%d",
	    r4.a, r4.b, r8.a, r8.b, r3.a, r3.b, r3.c);
	for (i = 0; i < 9; i++)
		b.v[i] = i * i;
	b.d = 10;
	rb = abi_big(b, 5);
	ok = rb.v[0] == 86 && rb.d == 2.5;
	for (i = 1; i < 9; i++)
		ok &= rb.v[i] == (8 - i) * (8 - i) + 5;
	t_check("big_struct_ret", ok, "v0 %d v1 %d d %g", rb.v[0], rb.v[1], rb.d);
	t_check("double_args", abi_dbl(1.5f, 2.25, -3, 1000, 1LL << 40, 0.125) == 1099511628770.875,
	    "%f", abi_dbl(1.5f, 2.25, -3, 1000, 1LL << 40, 0.125));
	t_check("float_ret", abi_flt(3.0f, 0.5f) == 7.5f, "%g", (double)abi_flt(3.0f, 0.5f));
	t_check("longlong", abi_ll(0x123456789LL, 4, 1000) == 78187838LL, "wrong");
	r8.a = 0; r8.b = 4;
	b.v[0] = 0;
	{
		struct s8 q;

		q.a = 0; q.b = 8;
		t_check("varargs", abi_vsum(2, 1, 0.5, 3LL, r8, 2, 0.25, 5LL, q) == 54.5, "%g",
		    abi_vsum(2, 1, 0.5, 3LL, r8, 2, 0.25, 5LL, q));
	}
	abi_vfmt(buf, "%d %s %.3f %ld %c", 42, "ok", 2.5, 123456L, 'x');
	t_check("vsprintf", strcmp(buf, "42 ok 2.500 123456 x") == 0, "\"%s\"", buf);
	bf.a = 5; bf.b = 100; bf.c = -7; bf.d = 513;
	t_check("bitfields", abi_bits(bf) == 1027805, "%u", abi_bits(bf));
	t_check("callback", abi_cb(old_cb, 1.5) == -2.0, "%g", abi_cb(old_cb, 1.5));
}

main()
{
	int st;

	t_init("cc295", 55);
	if (strcmp(cc295_ver, "2.95") != 0) {
		t_skip("abi", "no gcc 2.95.3 (half built with %s)", cc295_ver);
		return t_done();
	}
	test_layout();
	test_calls();
	if (access("/tests/cc295_arith", 0) == 0) {
		st = system("/tests/cc295_arith");
		t_check("arith_run", st == 0, "status 0x%x", st);
	}
	return t_done();
}
