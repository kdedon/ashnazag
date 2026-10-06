/*
 * bsdtest: bsdsocket.library against the host test's server on
 * 127.0.0.1:7397.  Connects, waits a second in WaitSelect for data that
 * does not come, says hello, waits for the reply, resolves localhost
 * and bsdtest.example, and tries a refused connect; then sends its
 * findings as one line.
 */
typedef long L;
typedef unsigned long U;
typedef unsigned char B;

extern L bcall(void *, L, L *);
extern void *execbase(void);
static void *sb;

void *memset(void *d, int c, unsigned long n) { B *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { B *a = d; const B *b = s; while (n--) *a++ = *b++; return d; }
static L err;

static L call(void *base, L lvo, L d0, L d1, L d2, L a0, L a1, L a2, L a3)
{
	L r[8];
	r[0] = d0; r[1] = d1; r[2] = d2; r[3] = 0;
	r[4] = a0; r[5] = a1; r[6] = a2; r[7] = a3;
	return bcall(base, lvo, r);
}
#define S(lvo, d0, d1, d2, a0, a1, a2, a3) call(sb, lvo, d0, d1, d2, (L)(a0), (L)(a1), (L)(a2), (L)(a3))

static char rep[256];
static int nrep;
static void put(const char *s) { while (*s && nrep < 255) rep[nrep++] = *s++; rep[nrep] = 0; }
static void num(L v)
{
	char t[12];
	int n = 0;
	if (v < 0) put("-"), v = -v;
	do t[n++] = '0' + v % 10; while (v /= 10);
	while (n) { char c[2] = { t[--n], 0 }; put(c); }
}
static void quad(const B *a)
{
	int i;
	for (i = 0; i < 4; i++) { if (i) put("."); num(a[i]); }
}
/* the first address of a hostent, or "none" */
static void host(L *h)
{
	if (!h) put("none");
	else quad(**(B ***)&h[4]);
}

static L conn(L port)
{
	B sin[16] = { 16, 2, 0, 0, 127, 0, 0, 1 };
	L s = S(-30, 2, 1, 0, 0, 0, 0, 0);
	sin[2] = port >> 8;
	sin[3] = port;
	if (s < 0) return -1;
	if (S(-54, s, 16, 0, sin, 0, 0, 0) < 0) {
		S(-120, s, 0, 0, 0, 0, 0, 0);
		return -1;
	}
	return s;
}

static L sel(L s, U secs)
{
	U rd[4] = { 0, 0, 0, 0 }, tv[2];
	tv[0] = secs;
	tv[1] = 0;
	rd[s >> 5] = 1UL << (s & 31);
	return S(-126, s + 1, 0, 0, rd, 0, 0, tv);
}

int bsdmain(void)
{
	U tags[3] = { 0x80000031UL, 0, 0 };	/* SBTM_SETVAL(SBTC_ERRNOLONGPTR) */
	char buf[64];
	L s, s2, n;

	if (!(sb = (void *)call(execbase(), -552, 4, 0, 0, 0, (L)"bsdsocket.library", 0, 0)))
		return 20;
	tags[1] = (U)&err;
	if (S(-294, 0, 0, 0, tags, 0, 0, 0) != 0) return 20;
	if ((s = conn(7397)) < 0) return 10;
	put("R localhost=");
	host((L *)S(-210, 0, 0, 0, "localhost", 0, 0, 0));
	put(" select0=");
	num(sel(s, 1));
	S(-66, s, 6, 0, "hello\n", 0, 0, 0);
	put(" select1=");
	num(sel(s, 10));
	n = S(-78, s, sizeof buf - 1, 0, buf, 0, 0, 0);
	buf[n > 0 ? n : 0] = 0;
	if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = 0;
	put(" recv=");
	put(n > 0 ? buf : "none");
	put(" dns=");
	host((L *)S(-210, 0, 0, 0, "bsdtest.example", 0, 0, 0));
	s2 = conn(7398);
	put(" refused=");
	num(s2 < 0 ? err : 0);
	if (s2 >= 0) S(-120, s2, 0, 0, 0, 0, 0, 0);
	put("\n");
	S(-66, s, nrep, 0, rep, 0, 0, 0);
	S(-120, s, 0, 0, 0, 0, 0, 0);
	call(execbase(), -414, 0, 0, 0, 0, (L)sb, 0, 0);
	return 0;
}
