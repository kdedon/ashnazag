/*
 * pingd -- the echo service (pingio.h): ICMP echo requests sent for
 * clients that may not open raw sockets, each client getting the replies
 * to its own requests.  Echo only.  Each user may send RATE requests a
 * second, BURST at once; the rest are dropped, as a lossy network would.
 *
 *   pingd [-f]
 *	-f: stay in the foreground
 *
 * Without /dev/icmp the service waits, idle, so init does not respawn it.
 */
#include <sys/types.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/tihdr.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "pingio.h"

extern int gettimeofday(), chmod(), fattach(), fdetach(), putmsg(), getmsg();

#define NCL	32
#define CLMAX	8		/* clients of one user */
#define NPEND	256		/* requests awaiting replies, by sequence */
#define TMO	10		/* seconds a reply is waited for */
#define RATE	10		/* requests a second, per user */
#define BURST	10
#define NUID	16
#define HDR	12		/* struct pingreq before its data */

struct cl {
	int	fd;		/* -1: free */
	long	uid;
	long	gen;		/* bumped when the slot is freed */
};

struct pend {
	int		cl;	/* -1: none */
	long		gen;
	unsigned short	seq, cid, cseq;
	long		t;
};

struct bucket {
	long	uid;		/* -1: free */
	long	tok;		/* thousandths of a request */
	long	ms;		/* last refill */
};

static struct cl cl[NCL];
static struct pend pend[NPEND];
static struct bucket bk[NUID];
static unsigned short myid, nseq;
static char ib[4096];

static long
nowms()
{
	struct timeval tv;

	gettimeofday(&tv, (void *)0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static unsigned short
cksum(p, n)
	unsigned char *p;
	int n;
{
	unsigned long s = 0;

	for (; n > 1; p += 2, n -= 2)
		s += p[0] << 8 | p[1];
	if (n)
		s += p[0] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s & 0xffff;
}

/* one request's worth of the user's tokens */
static int
allowed(uid)
	long uid;
{
	struct bucket *b, *o = bk;
	long t = nowms();

	for (b = bk; b < bk + NUID; b++) {
		if (b->uid == uid)
			break;
		if (b->ms < o->ms)
			o = b;
	}
	if (b == bk + NUID) {
		b = o;
		b->uid = uid;
		b->tok = BURST * 1000L;
		b->ms = t;
	}
	b->tok += (t - b->ms) * RATE;
	b->ms = t;
	if (b->tok > BURST * 1000L)
		b->tok = BURST * 1000L;
	if (b->tok < 1000)
		return 0;
	b->tok -= 1000;
	return 1;
}

/* a raw ICMP endpoint, bound */
static int
rawopen()
{
	struct T_bind_req br;
	struct strbuf c;
	long ack[16];
	int fd, fl = 0;

	if ((fd = open("/dev/icmp", O_RDWR)) < 0)
		return -1;
	br.PRIM_type = T_BIND_REQ;
	br.ADDR_length = br.ADDR_offset = 0;
	br.CONIND_number = 0;
	c.len = sizeof br;
	c.buf = (char *)&br;
	if (putmsg(fd, &c, (struct strbuf *)0, 0) < 0)
		return -1;
	c.maxlen = sizeof ack;
	c.buf = (char *)ack;
	if (getmsg(fd, &c, (struct strbuf *)0, &fl) < 0 || c.len < 4 || ack[0] != T_BIND_ACK) {
		errno = EPROTO;
		return -1;
	}
	return fd;
}

static void
send1(rfd, k, q, n)
	int rfd, k, n;
	struct pingreq *q;
{
	unsigned char pk[8 + PING_DATA];
	long cb[(sizeof (struct T_unitdata_req) + 16) / 4];
	struct T_unitdata_req *u = (struct T_unitdata_req *)cb;
	unsigned char *a = (unsigned char *)(u + 1);
	struct strbuf c, d;
	struct pend *p;
	unsigned long dst = q->pq_dst;
	unsigned short s, ck;

	/* unicast only */
	if (dst == 0 || dst >= 0xe0000000L || q->pq_len > n - HDR || q->pq_len > PING_DATA)
		return;
	if (!allowed(cl[k].uid))
		return;
	s = nseq++;
	p = &pend[s % NPEND];
	p->cl = k;
	p->gen = cl[k].gen;
	p->seq = s;
	p->cid = q->pq_id;
	p->cseq = q->pq_seq;
	p->t = nowms() / 1000;
	pk[0] = 8;
	pk[1] = pk[2] = pk[3] = 0;
	pk[4] = myid >> 8;
	pk[5] = myid;
	pk[6] = s >> 8;
	pk[7] = s;
	memcpy((char *)pk + 8, q->pq_data, q->pq_len);
	ck = cksum(pk, 8 + q->pq_len);
	pk[2] = ck >> 8;
	pk[3] = ck;
	memset((char *)a, 0, 16);
	a[1] = 2;
	memcpy((char *)a + 4, (char *)&dst, 4);
	u->PRIM_type = T_UNITDATA_REQ;
	u->DEST_length = 16;
	u->DEST_offset = sizeof *u;
	u->OPT_length = u->OPT_offset = 0;
	c.len = sizeof *u + 16;
	c.buf = (char *)cb;
	d.len = 8 + q->pq_len;
	d.buf = (char *)pk;
	putmsg(rfd, &c, &d, 0);
}

/* a datagram from the raw endpoint: an echo reply goes to its client */
static void
input(rfd)
	int rfd;
{
	struct T_unitdata_ind *ui;
	struct strbuf c, d;
	struct pingrep r;
	struct pend *p;
	long cb[32];
	unsigned char *b = (unsigned char *)ib, *a;
	int fl = 0, n, hl, more;
	unsigned short s;

	c.maxlen = sizeof cb;
	c.buf = (char *)cb;
	d.maxlen = sizeof ib;
	d.buf = ib;
	if ((more = getmsg(rfd, &c, &d, &fl)) < 0)
		return;
	/* the rest of an oversized message is not ours */
	while (more & MOREDATA) {
		struct strbuf x;

		x.maxlen = sizeof ib;
		x.buf = ib;
		fl = 0;
		if ((more = getmsg(rfd, (struct strbuf *)0, &x, &fl)) < 0)
			return;
		d.len = 0;
	}
	ui = (struct T_unitdata_ind *)cb;
	if (c.len < sizeof *ui || ui->PRIM_type != T_UNITDATA_IND || (n = d.len) < 8)
		return;
	memset((char *)&r, 0, sizeof r);
	/* raw input carries its IP header */
	if (b[0] >> 4 == 4) {
		hl = (b[0] & 15) * 4;
		if (n < hl + 8)
			return;
		r.pr_ttl = b[8];
		memcpy((char *)&r.pr_src, (char *)b + 12, 4);
		b += hl;
		n -= hl;
	} else if (ui->SRC_length >= 8 && ui->SRC_offset + 8 <= c.len) {
		a = (unsigned char *)cb + ui->SRC_offset;
		memcpy((char *)&r.pr_src, (char *)a + 4, 4);
	}
	if (b[0] != 0 || b[1] != 0 || (b[4] << 8 | b[5]) != myid)
		return;
	s = b[6] << 8 | b[7];
	p = &pend[s % NPEND];
	if (p->cl < 0 || p->seq != s || cl[p->cl].fd < 0 || cl[p->cl].gen != p->gen ||
	    nowms() / 1000 - p->t > TMO)
		return;
	r.pr_id = p->cid;
	r.pr_seq = p->cseq;
	r.pr_len = n - 8 > PING_DATA ? PING_DATA : n - 8;
	memcpy(r.pr_data, (char *)b + 8, r.pr_len);
	write(cl[p->cl].fd, (char *)&r, sizeof r);
	p->cl = -1;
}

static void
drop(k)
	int k;
{
	close(cl[k].fd);
	cl[k].fd = -1;
	cl[k].gen++;
}

static void
accept1(lfd)
	int lfd;
{
	struct strrecvfd r;
	int k, f = -1, same = 0;

	if (ioctl(lfd, I_RECVFD, &r) < 0)
		return;
	for (k = 0; k < NCL; k++)
		if (cl[k].fd < 0) {
			if (f < 0)
				f = k;
		} else if (cl[k].uid == r.uid)
			same++;
	if (f < 0 || same >= CLMAX) {
		close(r.fd);
		return;
	}
	cl[f].fd = r.fd;
	cl[f].uid = r.uid;
	fcntl(r.fd, F_SETFL, O_NONBLOCK);
	fcntl(r.fd, F_SETFD, 1);
	ioctl(r.fd, I_SRDOPT, RMSGD);
}

static void
quit()
{
	fdetach(PINGPATH);
	unlink(PINGPATH);
	exit(0);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct pollfd pf[2 + NCL];
	int pk[2 + NCL];
	struct pingreq q;
	int rfd, fd, p[2], i, k, n, fg = argc > 1 && strcmp(argv[1], "-f") == 0;

	if ((rfd = rawopen()) < 0) {
		if (errno == ENOENT || errno == ENXIO || errno == ENODEV)
			for (;;)
				pause();
		perror("pingd: /dev/icmp");
		exit(1);
	}
	fcntl(rfd, F_SETFD, 1);
	myid = getpid();
	nseq = (unsigned short)nowms();
	for (k = 0; k < NCL; k++)
		cl[k].fd = -1;
	for (k = 0; k < NPEND; k++)
		pend[k].cl = -1;
	for (k = 0; k < NUID; k++)
		bk[k].uid = -1;
	if ((fd = open(PINGPATH, O_RDWR | O_CREAT, 0666)) >= 0)
		close(fd);
	chmod(PINGPATH, 0666);
	if (pipe(p) < 0 || ioctl(p[1], I_PUSH, "connld") < 0) {
		perror("pingd: pipe");
		exit(1);
	}
	if (fattach(p[1], PINGPATH) < 0 && (fdetach(PINGPATH), fattach(p[1], PINGPATH) < 0)) {
		perror("pingd: fattach");
		exit(1);
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, quit);
	if (!fg) {
		if (fork() != 0)
			exit(0);
		setsid();
	}
	for (;;) {
		pf[0].fd = p[0];
		pf[1].fd = rfd;
		pf[0].events = pf[1].events = POLLIN;
		n = 2;
		for (k = 0; k < NCL; k++)
			if (cl[k].fd >= 0) {
				pk[n] = k;
				pf[n].fd = cl[k].fd;
				pf[n++].events = POLLIN;
			}
		if (poll(pf, (unsigned long)n, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("pingd: poll");
			exit(1);
		}
		if (pf[0].revents & POLLIN)
			accept1(p[0]);
		if (pf[1].revents & POLLIN)
			input(rfd);
		for (i = 2; i < n; i++) {
			k = pk[i];
			if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			if ((fd = read(cl[k].fd, (char *)&q, sizeof q)) >= HDR)
				send1(rfd, k, &q, fd);
			else if (fd == 0 || (fd < 0 && errno != EAGAIN))
				drop(k);
		}
	}
}
