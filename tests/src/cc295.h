/*
 * cc295.h -- calls between code from the kernel's compiler (t_cc295.c)
 * and from gcc 2.95.3 (cc295abi.c), which must agree on layout and calls.
 */
#ifndef CC295_H
#define CC295_H

struct mix {			/* padding after char, short; double alignment */
	char c;
	double d;
	short s;
	int i;
	char tail;
};
struct s4 { short a, b; };	/* small struct returns */
struct s8 { int a, b; };
struct s3 { char a, b, c; };
struct big { int v[9]; double d; };	/* returned through a hidden pointer */
struct bits { unsigned a:3, b:7; int c:12; unsigned d:10; };

struct layout {
	int size_mix, off_d, off_s, off_i, off_tail;
	int size_s3, size_bits, size_ld, size_ll;
};

/* in cc295abi.c (gcc 2.95.3) */
extern char cc295_ver[];
void abi_layout(struct layout *);
struct mix abi_mix(int, double, struct mix, float);
struct s4 abi_s4(short, short);
struct s8 abi_s8(int, int);
struct s3 abi_s3(int);
struct big abi_big(struct big, int);
double abi_dbl(float, double, char, short, long long, double);
float abi_flt(float, float);
long long abi_ll(long long, int, long long);
double abi_vsum(int, ...);
void abi_vfmt(char *, char *, ...);
unsigned abi_bits(struct bits);
double abi_cb(double (*)(double, int, struct s8), double);

/* in t_cc295.c (the kernel's compiler), called back from gcc 2.95.3 code */
struct s8 old_s8(int);
struct big old_big(int);
double old_vsum(int, ...);
double old_cb(double, int, struct s8);

#endif
