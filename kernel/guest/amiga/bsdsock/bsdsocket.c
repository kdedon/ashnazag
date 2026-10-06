/*
 * bsdsocket.library over the host's sockets.  Each call is a socket call
 * of the environment's own Unix process (AMIGAIOC_SOCK), so the host's
 * network identity and the user's rights apply.  Every opener gets its
 * own base with its own socket table, errno and signals.  Host sockets
 * never block: a blocking call polls once, then sleeps in Exec's Wait
 * until host readiness arrives as a PORTS interrupt, a break signal or
 * a timeout.
 */
#include "amigasock.h"

typedef unsigned long U;
typedef long L;
typedef unsigned char B;
typedef unsigned short W;

extern L excall(L, L, L, L, L), hsys(L, L, L, L, L);
extern void bs_isr(void), linit(void);
extern L vectors[];

#define EX(lvo, d0, d1, a0, a1) excall(lvo, (L)(d0), (L)(d1), (L)(a0), (L)(a1))
#define Disable() EX(-120, 0, 0, 0, 0)
#define Enable() EX(-126, 0, 0, 0, 0)
#define Forbid() EX(-132, 0, 0, 0, 0)
#define Permit() EX(-138, 0, 0, 0, 0)
#define AllocMem(n) ((void *)EX(-198, n, 0x10001, 0, 0))
#define FreeMem(p, n) EX(-210, n, 0, 0, p)
#define FindTask() ((void *)EX(-294, 0, 0, 0, 0))
#define SetSignal(v, m) ((U)EX(-306, v, m, 0, 0))
#define Wait(m) ((U)EX(-318, m, 0, 0, 0))
#define Signal(t, m) EX(-324, m, 0, 0, t)
#define AllocSignal() EX(-330, -1, 0, 0, 0)
#define FreeSignal(n) EX(-336, n, 0, 0, 0)
#define CheckIO(r) EX(-468, 0, 0, 0, r)
#define WaitIO(r) EX(-474, 0, 0, 0, r)
#define AbortIO(r) EX(-480, 0, 0, 0, r)
#define SendIO(r) EX(-462, 0, 0, 0, r)

/* SVR4 host */
#define H_OPEN 5
#define H_CLOSE 6
#define H_READ 3
#define H_IOCTL 54
#define H_FCNTL 62
#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 0x10
#define POLLNVAL 0x20
#define POLLRDNORM 0x40
#define POLLRDBAND 0x80

/* BSD errnos, as Amiga programs know them */
#define EBADF 9
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define EMFILE 24
#define EINTR 4
#define EIO 5
#define EWOULDBLOCK 35
#define EINPROGRESS 36
#define ENOTSOCK 38
#define EOPNOTSUPP 45
#define EAFNOSUPPORT 47
#define ESOCKTNOSUPPORT 44
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3

#define NFD 128
#define NREL 32
#define NADDR 8
#define SIGBREAKF_CTRL_C (1UL << 12)
#define MSG_HOST 7		/* OOB, PEEK, DONTROUTE */
#define MSG_WAITALL 0x40
#define MSG_DONTWAIT 0x80

struct pfd { L fd; W ev, rev; };
struct sk { short h; B nb, used; U rto[2], sto[2]; };

struct base {
	B lib[36];
	struct base *master, *next;
	void *task, *seg;
	U netmask, breakmask, sigio, sigurg, eventmask;
	L netbit, waiting, dtsize, err, herr, errsz;
	void *errp, *herrp;
	void *tport, *treq;
	struct sk sk[NFD];
	struct pfd pfd[NFD];
	short map[NFD];
	char ntoa[16];
	/* hostent, servent, protoent */
	L he[5], se[4], pe[3];
	char *alias[1], *addrp[NADDR + 1];
	U addr[NADDR];
	char name[256], sname[64], sproto[16];
	B pkt[512];
	char line[256], fbuf[512];
	L fpos, flen, ffd;
};

static B gap[BS_GAP];		/* used only inside one host call */
static L devfd = -1;
static B isr[22];
static struct { L id, h; struct base *own; } rel[NREL];
static L relid = 0x10000;
const char bs_name[] = "bsdsocket.library";
const char bs_idstring[] = "bsdsocket.library 4.1 (06.10.2026) host sockets";
const L bs_autoinit[4] = { sizeof(struct base), (L)vectors, 0, (L)linit };

void *memset(void *d, int c, unsigned long n) { B *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { B *a = d; const B *b = s; while (n--) *a++ = *b++; return d; }
static L slen(const char *s) { L n = 0; while (s[n]) n++; return n; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, const char *b)
{
	while (*a && lower(*a) == lower(*b)) a++, b++;
	return !*a && !*b;
}
static void scopy(char *d, const char *s, L n) { while (--n > 0 && *s) *d++ = *s++; *d = 0; }

/* ---- errors ---- */

static const B errmap[][2] = {
	{ 11, 35 }, { 45, 11 }, { 46, 77 }, { 78, 63 }, { 89, 78 }, { 90, 62 }, { 93, 66 },
	{ 95, 38 }, { 96, 39 }, { 97, 40 }, { 98, 41 }, { 99, 42 }, { 120, 43 }, { 121, 44 },
	{ 122, 45 }, { 123, 46 }, { 124, 47 }, { 125, 48 }, { 126, 49 }, { 127, 50 },
	{ 128, 51 }, { 129, 52 }, { 130, 53 }, { 131, 54 }, { 132, 55 }, { 133, 56 },
	{ 134, 57 }, { 143, 58 }, { 144, 59 }, { 145, 60 }, { 146, 61 }, { 147, 64 },
	{ 148, 65 }, { 149, 37 }, { 150, 36 },
};

/* a host errno as BSD numbers it */
static L bsderr(L e)
{
	unsigned i;
	for (i = 0; i < sizeof errmap / sizeof errmap[0]; i++)
		if (errmap[i][0] == e) return errmap[i][1];
	return e > 0 && e <= 34 ? e : EIO;
}

static L fail(struct base *b, L e)
{
	b->err = e;
	if (b->errp) {
		if (b->errsz == 1) *(B *)b->errp = e;
		else if (b->errsz == 2) *(W *)b->errp = e;
		else *(L *)b->errp = e;
	}
	return -1;
}

static void *herr(struct base *b, L e)
{
	b->herr = e;
	if (b->herrp) *(L *)b->herrp = e;
	return 0;
}

static const char *const errstr[] = {
	"Undefined error: 0", "Operation not permitted", "No such file or directory",
	"No such process", "Interrupted system call", "Input/output error",
	"Device not configured", "Argument list too long", "Exec format error",
	"Bad file descriptor", "No child processes", "Resource deadlock avoided",
	"Cannot allocate memory", "Permission denied", "Bad address", "Block device required",
	"Device busy", "File exists", "Cross-device link", "Operation not supported by device",
	"Not a directory", "Is a directory", "Invalid argument", "Too many open files in system",
	"Too many open files", "Inappropriate ioctl for device", "Text file busy",
	"File too large", "No space left on device", "Illegal seek", "Read-only file system",
	"Too many links", "Broken pipe", "Numerical argument out of domain", "Result too large",
	"Operation would block", "Operation now in progress", "Operation already in progress",
	"Socket operation on non-socket", "Destination address required", "Message too long",
	"Protocol wrong type for socket", "Protocol not available", "Protocol not supported",
	"Socket type not supported", "Operation not supported", "Protocol family not supported",
	"Address family not supported by protocol family", "Address already in use",
	"Can't assign requested address", "Network is down", "Network is unreachable",
	"Network dropped connection on reset", "Software caused connection abort",
	"Connection reset by peer", "No buffer space available", "Socket is already connected",
	"Socket is not connected", "Can't send after socket shutdown",
	"Too many references: can't splice", "Connection timed out", "Connection refused",
	"Too many levels of symbolic links", "File name too long", "Host is down",
	"No route to host",
};
static const char *const herrstr[] = {
	"Resolver Error 0 (no error)", "Unknown host", "Host name lookup failure",
	"Unknown server error", "No address associated with name",
};

/* ---- host calls ---- */

static L sock(struct amigasock *s, L op, L fd)
{
	L r;
	s->bs_op = op;
	s->bs_fd = fd;
	s->bs_gap = (char *)gap;
	r = hsys(H_IOCTL, devfd, AMIGAIOC_SOCK, (L)s, 0);
	return r < 0 ? bsderr(-r) : 0;
}

static void zero(void *p, L n) { memset(p, 0, n); }
static int neq(const void *a, const void *b, unsigned long n)
{
	const B *x = a, *y = b;
	while (n--) if (*x++ != *y++) return 1;
	return 0;
}

/* a guest sockaddr_in (length byte, family byte) as the host's (family short) */
static L addrin(struct amigasock *s, const B *a, L n)
{
	if (n < 8 || n > 16) return EINVAL;
	memcpy(s->bs_addr, a, n);
	if (n < 16) zero(s->bs_addr + n, 16 - n);
	s->bs_addr[0] = 0;
	if (s->bs_addr[1] != 2) return EAFNOSUPPORT;
	s->bs_alen = 16;
	return 0;
}

static void addrout(struct amigasock *s, B *a, L *n)
{
	B t[16];
	L k;
	if (!a || !n) return;
	memcpy(t, s->bs_addr, 16);
	t[0] = 16;
	k = *n < 16 ? *n : 16;
	if (k > 0) memcpy(a, t, k);
	*n = 16;
}

/* poll the first n of b->pfd once */
static L hpoll(struct base *b, L n)
{
	struct amigasock s;
	L e;
	zero(&s, sizeof s);
	s.bs_buf = (char *)b->pfd;
	s.bs_len = n;
	if ((e = sock(&s, BSO_POLL, 0)) != 0) return -e;
	return s.bs_rv;
}

static int timer(struct base *b)
{
	if (b->treq) return 1;
	if (!(b->tport = (void *)EX(-666, 0, 0, 0, 0))) return 0;
	if ((b->treq = (void *)EX(-654, 40, 0, b->tport, 0)) &&
	    EX(-444, 0, 0, "timer.device", b->treq) == 0)
		return 1;
	if (b->treq) EX(-660, 0, 0, b->treq, 0);
	EX(-672, 0, 0, b->tport, 0);
	b->treq = b->tport = 0;
	return 0;
}

/*
 * Wait until one of the first n pollfds is ready (their count), a signal
 * in mask or a break arrives (*got), or the timeout tv ends (0).  No tv:
 * no timeout; tv of zero: poll once.
 */
static L await(struct base *b, L n, U *tv, U mask, U *got)
{
	U sigs, tsig = 0;
	L r;
	B *t = b->treq;

	*got = 0;
	if (b->task != FindTask()) return -EBADF;
	if (tv && (tv[0] || tv[1])) {
		if (!timer(b)) return -ENOMEM;
		t = b->treq;
		*(W *)(t + 28) = 9;	/* TR_ADDREQUEST */
		*(U *)(t + 32) = tv[0] + tv[1] / 1000000;
		*(U *)(t + 36) = tv[1] % 1000000;
		tsig = 1UL << ((B *)b->tport)[15];
		SetSignal(0, tsig);
		SendIO(t);
	}
	for (;;) {
		b->waiting = 1;
		SetSignal(0, b->netmask);
		if ((r = n ? hpoll(b, n) : 0) != 0) break;
		if (tv && !tsig) break;
		sigs = Wait(b->netmask | mask | b->breakmask | tsig);
		if (sigs & (mask | b->breakmask)) {
			*got = sigs & (mask | b->breakmask);
			break;
		}
		if (tsig && CheckIO(t)) break;
	}
	b->waiting = 0;
	if (tsig) {
		if (!CheckIO(t)) AbortIO(t);
		WaitIO(t);
		SetSignal(0, tsig);
	}
	return r;
}

/* wait for ev on socket s; 0, or a BSD errno (EINTR on a break, EWOULDBLOCK on timeout) */
static L waitfor(struct base *b, L s, L ev, U *tv)
{
	U got;
	L r;
	b->pfd[0].fd = b->sk[s].h;
	b->pfd[0].ev = ev;
	b->pfd[0].rev = 0;
	r = await(b, 1, tv && (tv[0] || tv[1]) ? tv : 0, 0, &got);
	if (r < 0) return -r;
	if (got) return EINTR;
	return r ? 0 : EWOULDBLOCK;
}

/* ---- descriptors ---- */

static L slot(struct base *b, L h)
{
	L i;
	for (i = 0; i < b->dtsize; i++)
		if (!b->sk[i].used) {
			zero(&b->sk[i], sizeof b->sk[i]);
			b->sk[i].used = 1;
			b->sk[i].h = h;
			return i;
		}
	return -1;
}

static int bad(struct base *b, L s)
{
	return s < 0 || s >= b->dtsize || !b->sk[s].used;
}

#define CHECK(s) if (bad(b, s)) return fail(b, EBADF)

/* ---- library ---- */

/* b->task is on an Exec task list and owns the net signal; the interrupt runs with the lists stable */
static int alive(struct base *b)
{
	B *volatile *a = (B *volatile *)4, *x, *n;
	int k;

	__asm__("" : "+r"(a));
	x = *a;
	if (!(*(U *)((B *)b->task + 18) & b->netmask)) return 0;
	if (b->task == *(void **)(x + 276)) return 1;
	for (k = 0; k < 2; k++)
		for (n = *(B **)(x + (k ? 420 : 406)); *(B **)n; n = *(B **)n)
			if ((void *)n == b->task) return 1;
	return 0;
}

/* a task that died inside a wait never clears waiting */
void bs_server(struct base *m)
{
	struct base *b;
	for (b = m->next; b; b = b->next)
		if (b->waiting) {
			if (alive(b)) Signal(b->task, b->netmask);
			else b->waiting = 0;
		}
}

void *bs_init(B *lib, void *seg)
{
	struct base *m = (struct base *)lib;
	struct amigasock s;
	lib[8] = 9;
	*(const char **)(lib + 10) = bs_name;
	lib[14] = 6;
	*(W *)(lib + 20) = 4;
	*(W *)(lib + 22) = 1;
	*(const char **)(lib + 24) = bs_idstring;
	m->seg = seg;
	m->master = m;
	if ((devfd = hsys(H_OPEN, (L)"/dev/amiga", 2, 0, 0)) < 0) return 0;
	zero(&s, sizeof s);
	if (sock(&s, BSO_POLL, 0)) {
		hsys(H_CLOSE, devfd, 0, 0, 0);
		return 0;
	}
	isr[8] = 2;
	*(const char **)(isr + 10) = bs_name;
	*(void **)(isr + 14) = m;
	*(void **)(isr + 18) = (void *)bs_isr;
	EX(-168, 3, 0, 0, isr);		/* PORTS */
	return lib;
}

void *bs_expunge(B *lib)
{
	struct base *m = (struct base *)lib;
	void *seg = m->seg;
	L i;
	if (*(W *)(lib + 32)) {
		lib[14] |= 8;		/* LIBF_DELEXP */
		return 0;
	}
	EX(-174, 3, 0, 0, isr);
	for (i = 0; i < NREL; i++)
		if (rel[i].h) hsys(H_CLOSE, rel[i].h - 1, 0, 0, 0);
	hsys(H_CLOSE, devfd, 0, 0, 0);
	devfd = -1;
	EX(-252, 0, 0, 0, lib);
	FreeMem(lib - *(W *)(lib + 16), *(W *)(lib + 16) + *(W *)(lib + 18));
	return seg;
}

void *bs_open(B *lib)
{
	struct base *m = (struct base *)lib, *b;
	W neg = *(W *)(lib + 16), pos = *(W *)(lib + 18);
	B *p;
	L i;

	if (!(p = AllocMem(neg + pos))) return 0;
	b = (struct base *)(p + neg);
	memcpy(p, lib - neg, neg + 34);
	if ((b->netbit = AllocSignal()) < 0) {
		FreeMem(p, neg + pos);
		return 0;
	}
	*(W *)(b->lib + 32) = 1;
	b->master = m;
	b->task = FindTask();
	b->netmask = 1UL << b->netbit;
	b->breakmask = SIGBREAKF_CTRL_C;
	b->dtsize = 64;
	b->ffd = -1;
	for (i = 0; i < NFD; i++) b->sk[i].h = -1;
	EX(-636, 0, 0, 0, 0);		/* CacheClearU: the copied jump table */
	(*(W *)(lib + 32))++;
	lib[14] &= ~8;
	Disable();
	b->next = m->next;
	m->next = b;
	Enable();
	return b;
}

void *bs_close(struct base *b)
{
	struct base *m = b->master, **pp;
	W neg = *(W *)(b->lib + 16), pos = *(W *)(b->lib + 18);
	L i;

	for (i = 0; i < NFD; i++)
		if (b->sk[i].used) hsys(H_CLOSE, b->sk[i].h, 0, 0, 0);
	/* sockets this opener released and nobody obtained */
	Forbid();
	for (i = 0; i < NREL; i++)
		if (rel[i].h && rel[i].own == b) {
			hsys(H_CLOSE, rel[i].h - 1, 0, 0, 0);
			rel[i].h = 0;
		}
	Permit();
	if (b->treq) {
		EX(-450, 0, 0, 0, b->treq);
		EX(-660, 0, 0, b->treq, 0);
		EX(-672, 0, 0, b->tport, 0);
	}
	FreeSignal(b->netbit);
	Disable();
	for (pp = &m->next; *pp; pp = &(*pp)->next)
		if (*pp == b) {
			*pp = b->next;
			break;
		}
	Enable();
	FreeMem((B *)b - neg, neg + pos);
	if (--*(W *)(m->lib + 32) == 0 && (m->lib[14] & 8))
		return bs_expunge(m->lib);
	return 0;
}

/* ---- sockets ---- */

L bs_socket(L domain, L type, L proto, struct base *b)
{
	struct amigasock s;
	L e, n;
	if (domain != 2) return fail(b, EAFNOSUPPORT);
	if (type < 1 || type > 3) return fail(b, ESOCKTNOSUPPORT);
	zero(&s, sizeof s);
	s.bs_arg = type == 1 ? 2 : type == 2 ? 1 : 4;
	s.bs_len = proto;
	if ((e = sock(&s, BSO_SOCKET, 0)) != 0) return fail(b, e);
	if ((n = slot(b, s.bs_rv)) < 0) {
		hsys(H_CLOSE, s.bs_rv, 0, 0, 0);
		return fail(b, EMFILE);
	}
	return n;
}

static L addrcall(struct base *b, L fd, L op, const B *a, L n)
{
	struct amigasock s;
	L e;
	zero(&s, sizeof s);
	if ((e = addrin(&s, a, n)) != 0 || (e = sock(&s, op, b->sk[fd].h)) != 0)
		return e;
	return 0;
}

L bs_bind(L fd, const B *a, L n, struct base *b)
{
	L e;
	CHECK(fd);
	return (e = addrcall(b, fd, BSO_BIND, a, n)) != 0 ? fail(b, e) : 0;
}

L bs_connect(L fd, const B *a, L n, struct base *b)
{
	struct amigasock s;
	L e;
	CHECK(fd);
	if ((e = addrcall(b, fd, BSO_CONNECT, a, n)) != EINPROGRESS)
		return e ? fail(b, e) : 0;
	if (b->sk[fd].nb) return fail(b, e);
	if ((e = waitfor(b, fd, POLLOUT, 0)) != 0) return fail(b, e);
	zero(&s, sizeof s);
	if ((e = sock(&s, BSO_CONNWAIT, b->sk[fd].h)) != 0) return fail(b, e);
	return s.bs_rv > 0 ? fail(b, bsderr(s.bs_rv)) : 0;
}

L bs_listen(L fd, L n, struct base *b)
{
	struct amigasock s;
	L e;
	CHECK(fd);
	zero(&s, sizeof s);
	s.bs_arg = n;
	return (e = sock(&s, BSO_LISTEN, b->sk[fd].h)) != 0 ? fail(b, e) : 0;
}

L bs_accept(L fd, B *a, L *n, struct base *b)
{
	struct amigasock s;
	L e, k;
	CHECK(fd);
	for (;;) {
		zero(&s, sizeof s);
		if ((e = sock(&s, BSO_ACCEPT, b->sk[fd].h)) != EWOULDBLOCK || b->sk[fd].nb)
			break;
		if ((e = waitfor(b, fd, POLLIN, 0)) != 0) break;
	}
	if (e) return fail(b, e);
	if ((k = slot(b, s.bs_rv)) < 0) {
		hsys(H_CLOSE, s.bs_rv, 0, 0, 0);
		return fail(b, EMFILE);
	}
	addrout(&s, a, n);
	return k;
}

/* send, all of it unless nonblocking */
L bs_sendto(L fd, const B *buf, L len, L flags, const B *to, L tolen, struct base *b)
{
	struct amigasock s;
	L e = 0, done = 0, nb;
	CHECK(fd);
	nb = b->sk[fd].nb || (flags & MSG_DONTWAIT);
	do {
		zero(&s, sizeof s);
		if (to && (e = addrin(&s, to, tolen)) != 0) break;
		s.bs_buf = (char *)buf + done;
		s.bs_len = len - done;
		s.bs_arg = flags & MSG_HOST;
		if ((e = sock(&s, BSO_SEND, b->sk[fd].h)) == 0)
			done += s.bs_rv;
		else if (e != EWOULDBLOCK || nb)
			break;
		else if ((e = waitfor(b, fd, POLLOUT, b->sk[fd].sto)) != 0)
			break;
	} while (done < len && !nb && !to);
	if (done) return done;
	return e ? fail(b, e) : 0;
}

L bs_send(L fd, const B *buf, L len, L flags, struct base *b)
{
	return bs_sendto(fd, buf, len, flags, 0, 0, b);
}

L bs_recvfrom(L fd, B *buf, L len, L flags, B *from, L *fromlen, struct base *b)
{
	struct amigasock s;
	L e = 0, done = 0, nb;
	CHECK(fd);
	nb = b->sk[fd].nb || (flags & MSG_DONTWAIT);
	for (;;) {
		zero(&s, sizeof s);
		s.bs_buf = (char *)buf + done;
		s.bs_len = len - done;
		s.bs_arg = flags & MSG_HOST;
		if ((e = sock(&s, BSO_RECV, b->sk[fd].h)) == 0) {
			if (!done) addrout(&s, from, fromlen);
			done += s.bs_rv;
			if (!s.bs_rv || !(flags & MSG_WAITALL) || done >= len || nb) break;
		} else if (e != EWOULDBLOCK || nb || done)
			break;
		else if ((e = waitfor(b, fd, POLLIN, b->sk[fd].rto)) != 0)
			break;
	}
	if (done) return done;
	return e ? fail(b, e) : 0;
}

L bs_recv(L fd, B *buf, L len, L flags, struct base *b)
{
	return bs_recvfrom(fd, buf, len, flags, 0, 0, b);
}

L bs_shutdown(L fd, L how, struct base *b)
{
	struct amigasock s;
	L e;
	CHECK(fd);
	zero(&s, sizeof s);
	s.bs_arg = how;
	return (e = sock(&s, BSO_SHUTDOWN, b->sk[fd].h)) != 0 ? fail(b, e) : 0;
}

#define SOL_SOCKET 0xffff
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define SO_ERROR 0x1007
#define SO_TYPE 0x1008

L bs_setsockopt(L fd, L level, L name, const B *val, L len, struct base *b)
{
	struct amigasock s;
	L e;
	CHECK(fd);
	if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
		if (len < 8 || !val) return fail(b, EINVAL);
		memcpy(name == SO_RCVTIMEO ? b->sk[fd].rto : b->sk[fd].sto, val, 8);
		return 0;
	}
	zero(&s, sizeof s);
	s.bs_arg = level << 16 | (name & 0xffff);
	s.bs_buf = (char *)val;
	s.bs_len = len;
	return (e = sock(&s, BSO_SETOPT, b->sk[fd].h)) != 0 ? fail(b, e) : 0;
}

L bs_getsockopt(L fd, L level, L name, B *val, L *len, struct base *b)
{
	struct amigasock s;
	L e, v;
	CHECK(fd);
	if (!val || !len) return fail(b, EFAULT);
	if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
		if (*len < 8) return fail(b, EINVAL);
		memcpy(val, name == SO_RCVTIMEO ? b->sk[fd].rto : b->sk[fd].sto, 8);
		*len = 8;
		return 0;
	}
	zero(&s, sizeof s);
	s.bs_arg = level << 16 | (name & 0xffff);
	s.bs_buf = (char *)val;
	s.bs_len = *len;
	if ((e = sock(&s, BSO_GETOPT, b->sk[fd].h)) != 0) return fail(b, e);
	*len = s.bs_rv;
	if (level == SOL_SOCKET && (name == SO_TYPE || name == SO_ERROR) && *len >= 4) {
		v = *(L *)val;
		*(L *)val = name == SO_ERROR ? (v ? bsderr(v) : 0) : v == 2 ? 1 : v == 1 ? 2 : 3;
	}
	return 0;
}

static L name(L fd, B *a, L *n, L peer, struct base *b)
{
	struct amigasock s;
	L e;
	CHECK(fd);
	zero(&s, sizeof s);
	s.bs_arg = peer;
	if ((e = sock(&s, BSO_NAME, b->sk[fd].h)) != 0) return fail(b, e);
	addrout(&s, a, n);
	return 0;
}

L bs_getsockname(L fd, B *a, L *n, struct base *b) { return name(fd, a, n, 0, b); }
L bs_getpeername(L fd, B *a, L *n, struct base *b) { return name(fd, a, n, 1, b); }

#define FIONBIO 0x8004667eUL
#define FIOASYNC 0x8004667dUL
#define FIONREAD 0x4004667fUL

L bs_ioctl(L fd, U req, L *arg, struct base *b)
{
	struct amigasock s;
	L e;
	CHECK(fd);
	if (!arg) return fail(b, EFAULT);
	switch (req) {
	case FIONBIO:
		b->sk[fd].nb = *arg != 0;
		return 0;
	case FIOASYNC:
		return 0;
	case FIONREAD:
		zero(&s, sizeof s);
		if ((e = sock(&s, BSO_NREAD, b->sk[fd].h)) != 0) return fail(b, e);
		*arg = s.bs_rv;
		return 0;
	}
	return fail(b, EINVAL);
}

L bs_closesocket(L fd, struct base *b)
{
	CHECK(fd);
	hsys(H_CLOSE, b->sk[fd].h, 0, 0, 0);
	b->sk[fd].used = 0;
	b->sk[fd].h = -1;
	return 0;
}

#define ISSET(set, i) ((set)[(i) >> 5] & (1UL << ((i) & 31)))

L bs_waitselect(L nfds, U *rd, U *wr, U *ex, U *tv, U *maskp, struct base *b)
{
	U mask = maskp ? *maskp : 0, got;
	L i, n = 0, r, k;
	U w;

	if (nfds < 0) return fail(b, EINVAL);
	if (nfds > b->dtsize) nfds = b->dtsize;
	for (i = 0; i < nfds; i++) {
		w = (rd && ISSET(rd, i) ? POLLIN : 0) | (wr && ISSET(wr, i) ? POLLOUT : 0) |
		    (ex && ISSET(ex, i) ? POLLPRI | POLLRDBAND : 0);
		if (!w) continue;
		if (bad(b, i)) return fail(b, EBADF);
		b->pfd[n].fd = b->sk[i].h;
		b->pfd[n].ev = w;
		b->pfd[n].rev = 0;
		b->map[n++] = i;
	}
	r = await(b, n, tv, mask, &got);
	if (r < 0) return fail(b, -r);
	if (!(got & mask) && (got & b->breakmask)) {
		if (maskp) *maskp = 0;
		return fail(b, EINTR);
	}
	if (maskp) *maskp = (got | (r ? SetSignal(0, mask) : 0)) & mask;
	for (i = 0; i < (nfds + 31) >> 5; i++) {
		if (rd) rd[i] = 0;
		if (wr) wr[i] = 0;
		if (ex) ex[i] = 0;
	}
	for (i = 0, k = 0; i < n; i++) {
		w = b->pfd[i].rev;
		r = b->map[i];
		if (rd && (b->pfd[i].ev & POLLIN) && (w & (POLLIN | POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)))
			rd[r >> 5] |= 1UL << (r & 31), k++;
		if (wr && (b->pfd[i].ev & POLLOUT) && (w & (POLLOUT | POLLHUP | POLLERR)))
			wr[r >> 5] |= 1UL << (r & 31), k++;
		if (ex && (b->pfd[i].ev & POLLPRI) && (w & (POLLPRI | POLLRDBAND)))
			ex[r >> 5] |= 1UL << (r & 31), k++;
	}
	return k;
}

L bs_setsignals(U intr, U io, U urg, struct base *b)
{
	b->breakmask = intr;
	b->sigio = io;
	b->sigurg = urg;
	return 0;
}

L bs_dtablesize(struct base *b) { return b->dtsize; }

/* ---- passing sockets between tasks: host descriptors are the process's ---- */

L bs_release(L fd, L id, struct base *b)
{
	L i, f = -1;
	CHECK(fd);
	Forbid();
	if (id == -1) id = relid++;
	for (i = 0; i < NREL; i++) {
		if (rel[i].h && rel[i].id == id) f = -2;
		if (!rel[i].h && f == -1) f = i;
	}
	if (f >= 0) {
		rel[f].id = id;
		rel[f].h = b->sk[fd].h + 1;
		rel[f].own = b;
		b->sk[fd].used = 0;
		b->sk[fd].h = -1;
	}
	Permit();
	return f >= 0 ? id : fail(b, f == -2 ? EINVAL : EMFILE);
}

L bs_releasecopy(L fd, L id, struct base *b)
{
	L h, k;
	CHECK(fd);
	if ((h = hsys(H_FCNTL, b->sk[fd].h, 0, 0, 0)) < 0) return fail(b, bsderr(-h));
	if ((k = slot(b, h)) < 0) {
		hsys(H_CLOSE, h, 0, 0, 0);
		return fail(b, EMFILE);
	}
	if ((id = bs_release(k, id, b)) == -1) bs_closesocket(k, b);
	return id;
}

L bs_obtain(L id, L domain, L type, L proto, struct base *b)
{
	L i, h = -1, k;
	Forbid();
	for (i = 0; i < NREL; i++)
		if (rel[i].h && rel[i].id == id) {
			h = rel[i].h - 1;
			rel[i].h = 0;
			break;
		}
	Permit();
	if (h < 0) return fail(b, EWOULDBLOCK);
	if ((k = slot(b, h)) < 0) {
		hsys(H_CLOSE, h, 0, 0, 0);
		return fail(b, EMFILE);
	}
	return k;
}

L bs_dup2(L from, L to, struct base *b)
{
	L h;
	if (from == -1) {
		if (to < 0 || to >= b->dtsize) return fail(b, EBADF);
		if (b->sk[to].used) bs_closesocket(to, b);
		zero(&b->sk[to], sizeof b->sk[to]);
		b->sk[to].used = 1;
		b->sk[to].h = -1;
		return to;
	}
	CHECK(from);
	if (to == from) return to;
	if ((h = hsys(H_FCNTL, b->sk[from].h, 0, 0, 0)) < 0) return fail(b, bsderr(-h));
	if (to < 0) {
		if ((to = slot(b, h)) < 0) {
			hsys(H_CLOSE, h, 0, 0, 0);
			return fail(b, EMFILE);
		}
	} else {
		if (to >= b->dtsize) {
			hsys(H_CLOSE, h, 0, 0, 0);
			return fail(b, EBADF);
		}
		if (b->sk[to].used) bs_closesocket(to, b);
		b->sk[to] = b->sk[from];
		b->sk[to].h = h;
	}
	return to;
}

/* one iovec at a time */
static L msg(L fd, L *m, L flags, int out, struct base *b)
{
	L *iov = (L *)m[2], n = m[3], i, r, t = 0;
	for (i = 0; i < n; i++) {
		r = out ? bs_sendto(fd, (B *)iov[2 * i], iov[2 * i + 1], flags, (B *)m[0], m[1], b) :
		    bs_recvfrom(fd, (B *)iov[2 * i], iov[2 * i + 1], flags, (B *)m[0], &m[1], b);
		if (r < 0) return t ? t : r;
		t += r;
		if (r < iov[2 * i + 1]) break;
	}
	return t;
}

L bs_sendmsg(L fd, L *m, L flags, struct base *b) { CHECK(fd); return msg(fd, m, flags, 1, b); }
L bs_recvmsg(L fd, L *m, L flags, struct base *b) { CHECK(fd); return msg(fd, m, flags, 0, b); }

L bs_errno(struct base *b) { return b->err; }

L bs_seterrnoptr(void *p, L size, struct base *b)
{
	if (size != 1 && size != 2 && size != 4) return fail(b, EINVAL);
	b->errp = p;
	b->errsz = size;
	return 0;
}

L bs_events(U *mask, struct base *b) { if (mask) *mask = 0; return -1; }
L bs_none(L x, struct base *b) { return 0; }

/* ---- addresses ---- */

static char *dec(char *p, U v)
{
	char t[4];
	int n = 0;
	do t[n++] = '0' + v % 10; while (v /= 10);
	while (n) *p++ = t[--n];
	return p;
}

static void quad(char *p, U a)
{
	int i;
	for (i = 24; i >= 0; i -= 8) {
		p = dec(p, (a >> i) & 255);
		*p++ = i ? '.' : 0;
	}
}

char *bs_ntoa(U a, struct base *b)
{
	quad(b->ntoa, a);
	return b->ntoa;
}

/* dotted forms a, a.b, a.b.c, a.b.c.d, each part decimal, octal or hex */
static int aton(const char *cp, U *out)
{
	U part[4], v, base;
	int n = 0, c, any;

	for (;;) {
		v = 0; base = 10; any = 0;
		if (*cp == '0') {
			base = 8; cp++; any = 1;
			if (*cp == 'x' || *cp == 'X') base = 16, cp++, any = 0;
		}
		for (;; cp++) {
			c = lower(*cp);
			if (c >= '0' && c <= '9' && c - '0' < (int)base) v = v * base + c - '0';
			else if (base == 16 && c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
			else break;
			any = 1;
		}
		if (!any || n == 4) return 0;
		part[n++] = v;
		if (*cp != '.') break;
		cp++;
	}
	if (*cp && *cp != ' ' && *cp != '\t' && *cp != '\n') return 0;
	switch (n) {
	case 1: v = part[0]; break;
	case 2: if (part[0] > 255 || part[1] > 0xffffff) return 0;
		v = part[0] << 24 | part[1]; break;
	case 3: if (part[0] > 255 || part[1] > 255 || part[2] > 0xffff) return 0;
		v = part[0] << 24 | part[1] << 16 | part[2]; break;
	default: if (part[0] > 255 || part[1] > 255 || part[2] > 255 || part[3] > 255) return 0;
		v = part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3];
	}
	*out = v;
	return 1;
}

U bs_inet_addr(const char *cp, struct base *b)
{
	U a;
	return cp && aton(cp, &a) ? a : 0xffffffffUL;
}

U bs_inet_network(const char *cp, struct base *b)
{
	U a;
	if (!cp || !aton(cp, &a)) return 0xffffffffUL;
	while (a && !(a & 0xff000000UL)) a <<= 8;
	return a;
}

U bs_lnaof(U a, struct base *b)
{
	return a & (a >> 31 == 0 ? 0xffffff : a >> 30 == 2 ? 0xffff : 0xff);
}

U bs_netof(U a, struct base *b)
{
	return a >> 31 == 0 ? a >> 24 : a >> 30 == 2 ? a >> 16 : a >> 8;
}

U bs_makeaddr(U net, U host, struct base *b)
{
	if (net < 128) return net << 24 | (host & 0xffffff);
	if (net < 65536) return net << 16 | (host & 0xffff);
	return net << 8 | (host & 0xff);
}

/* ---- host files ---- */

static int fopen_(struct base *b, const char *path)
{
	b->ffd = hsys(H_OPEN, (L)path, 0, 0, 0);
	b->fpos = b->flen = 0;
	return b->ffd >= 0;
}

static void fclose_(struct base *b)
{
	if (b->ffd >= 0) hsys(H_CLOSE, b->ffd, 0, 0, 0);
	b->ffd = -1;
}

/* the next line split into words (comments dropped); their count */
static int fwords(struct base *b, char **w, int max)
{
	int n, k, c;
	for (;;) {
		k = 0;
		for (;;) {
			if (b->fpos == b->flen) {
				b->flen = hsys(H_READ, b->ffd, (L)b->fbuf, sizeof b->fbuf, 0);
				b->fpos = 0;
				if (b->flen <= 0) {
					b->flen = 0;
					if (!k) return -1;
					break;
				}
			}
			c = b->fbuf[b->fpos++];
			if (c == '\n') break;
			if (k < (int)sizeof b->line - 1) b->line[k++] = c;
		}
		b->line[k] = 0;
		for (k = 0, n = 0; b->line[k] && b->line[k] != '#' && n < max;) {
			while (b->line[k] == ' ' || b->line[k] == '\t') b->line[k++] = 0;
			if (!b->line[k] || b->line[k] == '#') break;
			w[n++] = b->line + k;
			while (b->line[k] && b->line[k] != ' ' && b->line[k] != '\t' && b->line[k] != '#') k++;
		}
		b->line[k] = 0;
		if (n) return n;
	}
}

/* ---- hosts ---- */

static L *hostent(struct base *b, const char *name, U *a, int n)
{
	int i;
	scopy(b->name, name, sizeof b->name);
	b->alias[0] = 0;
	for (i = 0; i < n && i < NADDR; i++) {
		b->addr[i] = a[i];
		b->addrp[i] = (char *)&b->addr[i];
	}
	b->addrp[i] = 0;
	b->he[0] = (L)b->name;
	b->he[1] = (L)b->alias;
	b->he[2] = 2;
	b->he[3] = 4;
	b->he[4] = (L)b->addrp;
	b->herr = 0;
	return b->he;
}

/* /etc/hosts by name (name set) or by address */
static L *hostsfile(struct base *b, const char *name, U addr)
{
	char *w[16];
	int n, i;
	U a;
	L *h = 0;
	if (!fopen_(b, "/etc/hosts")) return 0;
	while (!h && (n = fwords(b, w, 16)) >= 0) {
		if (n < 2 || !aton(w[0], &a)) continue;
		for (i = 1; i < n; i++)
			if (name ? same(w[i], name) : a == addr) {
				h = hostent(b, w[1], &a, 1);
				break;
			}
	}
	fclose_(b);
	return h;
}

/* nameservers and the first search domain from resolv.conf */
static int resolvconf(struct base *b, U *ns, char *dom)
{
	char *w[4];
	int n, k = 0;
	*dom = 0;
	if (!fopen_(b, "/etc/resolv.conf")) return 0;
	while ((n = fwords(b, w, 4)) >= 0)
		if (n >= 2 && same(w[0], "nameserver") && k < 3 && aton(w[1], &ns[k])) k++;
		else if (n >= 2 && (same(w[0], "domain") || same(w[0], "search")) && !*dom)
			scopy(dom, w[1], 64);
	fclose_(b);
	return k;
}

static const B *skipname(const B *p, const B *e)
{
	while (p < e && *p) {
		if ((*p & 0xc0) == 0xc0) return p + 2;
		p += *p + 1;
	}
	return p + 1;
}

/* a possibly compressed name at p into out */
static void getname(const B *pkt, const B *p, const B *e, char *out, int room)
{
	int k = 0, hops = 0, i;
	while (p < e && *p && hops < 16) {
		if ((*p & 0xc0) == 0xc0) {
			p = pkt + ((p[0] & 0x3f) << 8 | p[1]);
			hops++;
			continue;
		}
		if (k && k < room - 1) out[k++] = '.';
		for (i = 1; i <= *p && p + i < e; i++)
			if (k < room - 1) out[k++] = p[i];
		p += *p + 1;
	}
	out[k] = 0;
}

/*
 * One DNS query (qtype 1 A or 12 PTR) to each name server, twice
 * round: the answers into b, or an h_errno.
 */
static L dns(struct base *b, const char *q, int qtype, U *out, int *nout)
{
	struct amigasock s;
	U ns[3], tv[2], got, a;
	int junk;
	char dom[64];
	B *p = b->pkt, *m = (B *)b->fbuf;
	const B *e, *rp, *rd;
	const char *c;
	L fd, len, n, k, r, t, an, ty, rl, err = HOST_NOT_FOUND;
	static W id;

	if (!(n = resolvconf(b, ns, dom))) return HOST_NOT_FOUND;
	zero(&s, sizeof s);
	s.bs_arg = 1;
	if (sock(&s, BSO_SOCKET, 0)) return TRY_AGAIN;
	fd = s.bs_rv;
	id += (W)(U)b->task | 1;
	p[0] = id >> 8; p[1] = id; p[2] = 1; p[3] = 0;
	p[4] = 0; p[5] = 1; zero(p + 6, 6);
	for (len = 12, c = q; *c && len < 270;) {
		for (k = 0; c[k] && c[k] != '.'; k++);
		if (k == 0 || k > 63) { err = NO_RECOVERY; goto out; }
		p[len++] = k;
		memcpy(p + len, c, k);
		len += k;
		c += k;
		if (*c) c++;
	}
	if (*c) { err = NO_RECOVERY; goto out; }
	p[len++] = 0;
	p[len++] = 0; p[len++] = qtype; p[len++] = 0; p[len++] = 1;
	err = TRY_AGAIN;
	for (t = 0; t < 2 * n; t++) {
		zero(&s, sizeof s);
		s.bs_addr[1] = 2; s.bs_addr[3] = 53;
		memcpy(s.bs_addr + 4, &ns[t % n], 4);
		s.bs_alen = 16;
		s.bs_buf = (char *)p;
		s.bs_len = len;
		if (sock(&s, BSO_SEND, fd)) continue;
		tv[0] = 2; tv[1] = 0;
		for (junk = 0; junk < 8; junk++) {
			b->pfd[0].fd = fd;
			b->pfd[0].ev = POLLIN;
			b->pfd[0].rev = 0;
			r = await(b, 1, tv, 0, &got);
			if (got) { err = TRY_AGAIN; goto out; }
			if (r <= 0) break;
			zero(&s, sizeof s);
			s.bs_buf = (char *)m;
			s.bs_len = sizeof b->fbuf;
			if (sock(&s, BSO_RECV, fd) || s.bs_rv < 12 ||
			    s.bs_addr[2] || s.bs_addr[3] != 53 || neq(s.bs_addr + 4, &ns[t % n], 4) ||
			    m[0] != (B)(id >> 8) || m[1] != (B)id || !(m[2] & 0x80))
				continue;
			if ((m[3] & 15) == 3) { err = HOST_NOT_FOUND; goto out; }
			if ((m[3] & 15) != 0) { err = NO_RECOVERY; break; }
			e = m + s.bs_rv;
			r = m[4] << 8 | m[5];
			an = m[6] << 8 | m[7];
			for (rp = m + 12; r-- > 0 && rp < e;) rp = skipname(rp, e) + 4;
			for (k = 0; an-- > 0 && rp < e;) {
				rd = skipname(rp, e);
				if (rd + 10 > e) break;
				ty = rd[0] << 8 | rd[1];
				rl = rd[8] << 8 | rd[9];
				rd += 10;
				if (rd + rl > e) break;
				if (qtype == 1 && ty == 1 && rl == 4 && k < NADDR) {
					memcpy(&a, rd, 4);
					out[k++] = a;
				} else if (qtype == 12 && ty == 12) {
					getname(m, rd, e, b->name, sizeof b->name);
					k = 1;
					break;
				}
				rp = rd + rl;
			}
			*nout = k;
			err = k ? 0 : 4;	/* NO_DATA */
			goto out;
		}
	}
out:
	hsys(H_CLOSE, fd, 0, 0, 0);
	return err;
}

L *bs_gethostbyname(const char *name, struct base *b)
{
	U a[NADDR];
	char q[256], dom[64], *d;
	L *h;
	int n, e, dot;

	if (!name || !*name) return herr(b, HOST_NOT_FOUND);
	if (aton(name, &a[0])) return hostent(b, name, a, 1);
	if ((h = hostsfile(b, name, 0)) != 0) return h;
	for (d = (char *)name, dot = 0; *d; d++) dot |= *d == '.';
	e = HOST_NOT_FOUND;
	if (!dot) {
		U ns[3];
		if (resolvconf(b, ns, dom) && *dom && slen(name) + slen(dom) < 254) {
			scopy(q, name, sizeof q);
			n = slen(q);
			q[n] = '.';
			scopy(q + n + 1, dom, sizeof q - n - 1);
			if ((e = dns(b, q, 1, a, &n)) == 0) return hostent(b, q, a, n);
		}
	}
	if ((e = dns(b, name, 1, a, &n)) == 0) return hostent(b, name, a, n);
	return herr(b, e);
}

L *bs_gethostbyaddr(const B *addr, L len, L type, struct base *b)
{
	char q[32], *p;
	U a;
	L *h;
	int i, n;

	if (!addr || len != 4 || type != 2) return herr(b, HOST_NOT_FOUND);
	memcpy(&a, addr, 4);
	if ((h = hostsfile(b, 0, a)) != 0) return h;
	for (p = q, i = 0; i < 32; i += 8) {
		p = dec(p, (a >> i) & 255);
		*p++ = '.';
	}
	scopy(p, "in-addr.arpa", q + sizeof q - p);
	if ((i = dns(b, q, 12, &a, &n)) != 0) return herr(b, i);
	memcpy(&a, addr, 4);
	return hostent(b, b->name, &a, 1);
}

L bs_gethostname(char *name, L len, struct base *b)
{
	char *w[2];
	if (!name || len <= 0) return fail(b, EINVAL);
	scopy(name, "localhost", len);
	if (fopen_(b, "/etc/nodename")) {
		if (fwords(b, w, 2) > 0) scopy(name, w[0], len);
		fclose_(b);
	}
	return 0;
}

U bs_gethostid(struct base *b)
{
	char n[64];
	L *h;
	bs_gethostname(n, sizeof n, b);
	h = bs_gethostbyname(n, b);
	return h ? **(U **)h[4] : 0x7f000001UL;
}

/* ---- services and protocols ---- */

static const struct { const char *n; W port; B tcp; } servs[] = {
	{ "ftp", 21, 1 }, { "ssh", 22, 1 }, { "telnet", 23, 1 }, { "smtp", 25, 1 },
	{ "domain", 53, 0 }, { "http", 80, 1 }, { "www", 80, 1 }, { "pop3", 110, 1 },
	{ "nntp", 119, 1 }, { "imap", 143, 1 }, { "https", 443, 1 },
};

static L *servent(struct base *b, const char *n, U port, const char *proto)
{
	scopy(b->sname, n, sizeof b->sname);
	scopy(b->sproto, proto, sizeof b->sproto);
	b->alias[0] = 0;
	b->se[0] = (L)b->sname;
	b->se[1] = (L)b->alias;
	b->se[2] = port;
	b->se[3] = (L)b->sproto;
	return b->se;
}

/* /etc/services, then a few well-known ones */
static L *serv(struct base *b, const char *name, L port, const char *proto)
{
	char *w[16], *s;
	int n, i;
	U p;
	L *r = 0;
	if (fopen_(b, "/etc/services")) {
		while (!r && (n = fwords(b, w, 16)) >= 0) {
			if (n < 2) continue;
			for (s = w[1]; *s && *s != '/'; s++);
			if (!*s) continue;
			*s++ = 0;
			if (!aton(w[1], &p) || (proto && !same(s, proto))) continue;
			if (name) {
				for (i = 0; i < n; i++)
					if (i != 1 && same(w[i], name)) break;
				if (i == n) continue;
			} else if ((L)p != port)
				continue;
			r = servent(b, w[0], p, s);
		}
		fclose_(b);
	}
	for (i = 0; !r && i < (int)(sizeof servs / sizeof servs[0]); i++)
		if ((name ? same(servs[i].n, name) : servs[i].port == port) &&
		    (!proto || same(proto, servs[i].tcp ? "tcp" : "udp")))
			r = servent(b, servs[i].n, servs[i].port, servs[i].tcp ? "tcp" : "udp");
	return r;
}

L *bs_getservbyname(const char *name, const char *proto, struct base *b)
{
	return name ? serv(b, name, 0, proto) : 0;
}

L *bs_getservbyport(L port, const char *proto, struct base *b)
{
	return serv(b, 0, port & 0xffff, proto);
}

static const struct { const char *n; B p; } protos[] = {
	{ "ip", 0 }, { "icmp", 1 }, { "tcp", 6 }, { "udp", 17 },
};

static L *proto(struct base *b, const char *name, L num)
{
	int i;
	for (i = 0; i < (int)(sizeof protos / sizeof protos[0]); i++)
		if (name ? same(protos[i].n, name) : protos[i].p == num) {
			scopy(b->sproto, protos[i].n, sizeof b->sproto);
			b->alias[0] = 0;
			b->pe[0] = (L)b->sproto;
			b->pe[1] = (L)b->alias;
			b->pe[2] = protos[i].p;
			return b->pe;
		}
	return 0;
}

L *bs_getprotobyname(const char *name, struct base *b) { return name ? proto(b, name, 0) : 0; }
L *bs_getprotobynumber(L num, struct base *b) { return proto(b, 0, num); }

/* ---- SocketBaseTagList ---- */

#define TAG_USER 0x80000000UL

L bs_tags(U *t, struct base *b)
{
	L i = 0, code, set, ref;
	U *d, v;

	for (; t; t += 2) {
		if (t[0] == 0) break;			/* TAG_DONE */
		if (t[0] == 1) continue;		/* TAG_IGNORE */
		if (t[0] == 2) { t = (U *)t[1] - 2; continue; }	/* TAG_MORE */
		if (t[0] == 3) { t += 2 * t[1]; continue; }	/* TAG_SKIP */
		i++;
		if (!(t[0] & TAG_USER)) continue;
		code = (t[0] >> 1) & 0x3fff;
		set = t[0] & 1;
		ref = (t[0] & 0x8000) != 0;
		d = ref ? (U *)t[1] : &t[1];
		if (!d) return i;
		v = *d;
		switch (code) {
		case 1: if (set) b->breakmask = v; else *d = b->breakmask; break;
		case 2: if (set) b->sigio = v; else *d = b->sigio; break;
		case 3: if (set) b->sigurg = v; else *d = b->sigurg; break;
		case 4: if (set) b->eventmask = v; else *d = b->eventmask; break;
		case 6: if (set) fail(b, v); else *d = b->err; break;
		case 7: if (set) b->herr = v; else *d = b->herr; break;
		case 8:
			if (!set) *d = b->dtsize;
			else if ((L)v < 1 || v > NFD) return i;
			else b->dtsize = v;
			break;
		case 10: case 11: case 12: case 13:	/* syslog: nothing is logged */
			if (!set) *d = 0;
			break;
		case 14:
			if (set) return i;
			*d = (U)(v < sizeof errstr / sizeof errstr[0] ? errstr[v] : "Unknown error");
			break;
		case 15:
			if (set) return i;
			*d = (U)(v < sizeof herrstr / sizeof herrstr[0] ? herrstr[v] : herrstr[0]);
			break;
		case 21: case 22: case 24:
			if (!set) *d = (U)b->errp;
			else { b->errp = (void *)v; b->errsz = code - 20; }
			break;
		case 25:
			if (!set) *d = (U)b->herrp;
			else b->herrp = (void *)v;
			break;
		case 29:
			if (set) return i;
			*d = (U)bs_idstring;
			break;
		default:
			return i;
		}
	}
	return 0;
}
