/*
 * cc295abi.c -- the gcc 2.95.3 half of t_cc295 (built -O2).
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include "cc295.h"

#define STR2(x)	#x
#define STR(x)	STR2(x)
char cc295_ver[] = STR(__GNUC__) "." STR(__GNUC_MINOR__);

void
abi_layout(struct layout *l)
{
	l->size_mix = sizeof(struct mix);
	l->off_d = offsetof(struct mix, d);
	l->off_s = offsetof(struct mix, s);
	l->off_i = offsetof(struct mix, i);
	l->off_tail = offsetof(struct mix, tail);
	l->size_s3 = sizeof(struct s3);
	l->size_bits = sizeof(struct bits);
	l->size_ld = sizeof(long double);
	l->size_ll = sizeof(long long);
}

struct mix
abi_mix(int k, double x, struct mix m, float f)
{
	m.c += k;
	m.d = m.d * x + f;
	m.s -= k;
	m.i *= k;
	m.tail ^= 0x5a;
	return m;
}

struct s4
abi_s4(short a, short b)
{
	struct s4 r;

	r.a = b;
	r.b = a;
	return r;
}

struct s8
abi_s8(int a, int b)
{
	struct s8 r;

	r.a = a + b;
	r.b = a - b;
	return r;
}

struct s3
abi_s3(int k)
{
	struct s3 r;

	r.a = k;
	r.b = k + 1;
	r.c = k + 2;
	return r;
}

struct big
abi_big(struct big b, int k)
{
	struct big r;
	struct s8 o;
	int i;

	for (i = 0; i < 9; i++)
		r.v[i] = b.v[8 - i] + k;
	r.d = b.d / 4;
	o = old_s8(k);		/* nested struct returns, both directions */
	r.v[0] += old_big(o.a).v[8] - o.b;
	return r;
}

double
abi_dbl(float f, double d, char c, short s, long long ll, double e)
{
	return f + d * c + s + (double)ll + e;
}

float
abi_flt(float a, float b)
{
	return a / b + a * b;
}

long long
abi_ll(long long a, int sh, long long b)
{
	return (a << sh) / b + a % b;
}

double
abi_vsum(int n, ...)
{
	va_list ap;
	double s = 0;

	va_start(ap, n);
	while (n-- > 0) {
		s += va_arg(ap, int);
		s += va_arg(ap, double);
		s += (double)va_arg(ap, long long);
		s += va_arg(ap, struct s8).b;
	}
	va_end(ap);
	return s + old_vsum(2, 10, 0.5, 20, 0.25);
}

void
abi_vfmt(char *buf, char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);
	va_end(ap);
}

unsigned
abi_bits(struct bits b)
{
	return b.a + b.b * 8 + (b.c < 0 ? 1000 : 0) + b.d * 2000;
}

double
abi_cb(double (*f)(double, int, struct s8), double x)
{
	struct s8 p;

	p.a = 3;
	p.b = 4;
	return f(x, 2, p) + f(x * 2, -1, p);
}
