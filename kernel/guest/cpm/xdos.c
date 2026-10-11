/*
 * xdos.c -- Concurrent CP/M: processes, memory, flags, queues,
 * consoles and record locks (functions 42, 43, 128-153).
 *
 * Every CP/M process is a Unix process.  They share the environment's
 * memory above XPRIV, where the table below and the partitions live; a
 * process's own vectors and BIOS tables stay private below it.  One
 * byte lock on an unlinked file guards the table; a wait polls it.
 *
 *	0	vectors, BIOS tables (private)
 *	XPRIV	this table
 *	XPART	partition 0, 1, ... XPSIZE each, up to the end of memory
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include "bdos3.h"
#include "xdos.h"

#define	XMAGIC	0x58444f53L
#define	XFAIL	0xff

#define	QNAME	8
#define	QMSG	16
#define	QDEPTH	4
#define	NQ	8
#define	NFLAG	32

struct xproc {
	long	pid;		/* 0: a free slot */
	char	name[8];
	short	con, prio, flags;
};

struct xqueue {
	short	used;
	char	name[QNAME];
	short	msglen, ndep, cnt, in, out;
	char	buf[QDEPTH][QMSG];
};

struct xtab {
	long		magic;
	struct xproc	p[NPROC];
	short		part[NPART];		/* owner slot + 1 */
	long		conown[NCON];		/* pid attached, or 0 */
	char		flag[NFLAG], fwait[NFLAG];
	struct xqueue	q[NQ];
	int		nlocks;
};

static struct xtab *xt;
static int lkfd = -1;
int xme = -1, xncon = 1, xnpart;
int xmypart0 = -1;		/* the partition the process runs in */
static int zero;
int *b3_nlocks = &zero;
static char lkdir[1100];

static void
xlock()
{
	struct flock fl;

	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = F_WRLCK;
	fl.l_len = 1;
	while (fcntl(lkfd, F_SETLKW, &fl) < 0 && errno == EINTR)
		;
}

static void
xunlock()
{
	struct flock fl;

	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = F_UNLCK;
	fl.l_len = 1;
	fcntl(lkfd, F_SETLK, &fl);
}

static void
nap()
{
	poll((struct pollfd *)0, 0L, 20);
}

/* slot i's partitions and consoles go back */
static void
xrelease(i)
	int i;
{
	int k;

	for (k = 0; k < xnpart; k++)
		if (xt->part[k] == i + 1)
			xt->part[k] = 0;
	for (k = 0; k < NCON; k++)
		if (xt->conown[k] == xt->p[i].pid)
			xt->conown[k] = 0;
	for (k = 0; k < NFLAG; k++)
		if (xt->fwait[k] == i + 1)
			xt->fwait[k] = 0;
}

/* live processes that are not waiting for a console to join; dead ones are freed */
static int
xlive()
{
	int i, n = 0;

	for (i = 0; i < NPROC; i++) {
		if (xt->p[i].pid == 0)
			continue;
		if (kill((pid_t)xt->p[i].pid, 0) < 0 && errno == ESRCH) {
			xrelease(i);
			xt->p[i].pid = 0;
			continue;
		}
		if (!(xt->p[i].flags & XP_IDLE))
			n++;
	}
	return n;
}

/* a free slot, taken for pid; -1 when none */
static int
xslot(pid)
	long pid;
{
	int i;

	xlive();
	for (i = 0; i < NPROC; i++)
		if (xt->p[i].pid == 0) {
			memset((char *)&xt->p[i], 0, sizeof xt->p[i]);
			xt->p[i].pid = pid;
			xt->p[i].prio = 200;
			return i;
		}
	return -1;
}

unsigned long
xpbase(k)
	int k;
{
	return XPART + (unsigned long)k * XPSIZE;
}

/* a free partition for slot s; -1 when none */
static int
xpalloc(s)
	int s;
{
	int k;

	xlive();
	for (k = 0; k < xnpart; k++)
		if (xt->part[k] == 0) {
			xt->part[k] = s + 1;
			return k;
		}
	return -1;
}

/* the table, made by the first process; lockfile is unlinked once open */
int
xinit(mem, ncon, lockdir)
	unsigned long mem;
	int ncon;
	char *lockdir;
{
	char path[1200];

	sprintf(path, "%s/.xlock", lockdir);
	if ((lkfd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0)
		return -1;
	unlink(path);
	write(lkfd, "x", 1);
	sprintf(lkdir, "%s/.lk", lockdir);
	mkdir(lkdir, 0700);
	xt = (struct xtab *)XPRIV;
	memset((char *)xt, 0, sizeof *xt);
	xt->magic = XMAGIC;
	b3_nlocks = &xt->nlocks;
	xncon = ncon > NCON ? NCON : ncon;
	xnpart = (mem - XPART) / XPSIZE;
	if (xnpart > NPART)
		xnpart = NPART;
	xme = xslot((long)getpid());
	xt->p[xme].flags = XP_SESSION;
	return (xmypart0 = xpalloc(xme));
}

/* this process: a forked session for console con, waiting to be joined */
void
xsession(con)
	int con;
{
	xlock();
	xme = xslot((long)getpid());
	if (xme >= 0) {
		xt->p[xme].flags = XP_SESSION | XP_IDLE;
		xt->p[xme].con = con;
	}
	xunlock();
	if (xme < 0)
		_exit(1);
}

int
xcon()
{
	return xt->p[xme].con;
}

/* the session has its console: a partition, or -1 */
int
xjoined()
{
	int k;

	xlock();
	xt->p[xme].flags &= ~XP_IDLE;
	k = xmypart0 = xpalloc(xme);
	xunlock();
	return k;
}

/* the console is gone: back to waiting, nothing held */
void
xidle()
{
	xlock();
	xrelease(xme);
	xt->p[xme].flags |= XP_IDLE;
	xunlock();
}

void
xname(n)
	char *n;
{
	int i;

	for (i = 0; i < 8; i++)
		xt->p[xme].name[i] = n[i] & 0x7f;
}

/* a session's program ended: what it took goes back, not its own partition */
void
xwarm()
{
	int k;

	xlock();
	for (k = 0; k < xnpart; k++)
		if (xt->part[k] == xme + 1 && k != xmypart0)
			xt->part[k] = 0;
	xunlock();
}

/* this process ends */
void
xexit(st)
	int st;
{
	xlock();
	xrelease(xme);
	xt->p[xme].pid = 0;
	xunlock();
	b3_exit(st);
}

/* the session ends: every other member goes */
void
xkillall()
{
	long pid[NPROC];
	int i, n = 0, t;

	if (xt == 0)
		return;
	xlock();
	for (i = 0; i < NPROC; i++)
		if (i != xme && xt->p[i].pid > 0)
			pid[n++] = xt->p[i].pid;
	xunlock();
	for (i = 0; i < n; i++)
		kill((pid_t)pid[i], SIGKILL);
	for (t = 0; t < 100; t++) {
		for (i = 0; i < n && kill((pid_t)pid[i], 0) < 0; i++)
			;
		if (i == n)
			break;
		nap();
	}
}

/* ---- consoles ---- */

/* console input waits while another live process is attached */
void
xconwait()
{
	int c = xt->p[xme].con;
	long o;

	for (;;) {
		o = xt->conown[c];
		if (o == 0 || o == (long)getpid() || (kill((pid_t)o, 0) < 0 && errno == ESRCH))
			return;
		b3_conflush();
		nap();
	}
}

/* 0 when input may be taken now */
int
xconbusy()
{
	long o = xt->conown[xt->p[xme].con];

	return o != 0 && o != (long)getpid() && !(kill((pid_t)o, 0) < 0 && errno == ESRCH);
}

static long
attach()
{
	int c = xt->p[xme].con;

	for (;;) {
		xlock();
		xlive();
		if (xt->conown[c] == 0 || xt->conown[c] == (long)getpid()) {
			xt->conown[c] = getpid();
			xunlock();
			return 0;
		}
		xunlock();
		b3_conflush();
		nap();
	}
}

static long
detach()
{
	int c = xt->p[xme].con;

	if (xt->conown[c] != (long)getpid())
		return XFAIL;
	xt->conown[c] = 0;
	return 0;
}

/* 149: the named process to console n */
static long
assign(pb)
	unsigned long pb;
{
	char *n = B3MEM(pb + 1);
	int i, c = *B3MEM(pb) & 0x0f;
	long r = XFAIL;

	if (c >= xncon)
		return XFAIL;
	xlock();
	for (i = 0; i < NPROC; i++)
		if (xt->p[i].pid && memcmp(xt->p[i].name, n, 8) == 0) {
			xt->p[i].con = c;
			r = 0;
		}
	xunlock();
	return r;
}

/* ---- flags: one waiter each; a set flag is consumed by its wait ---- */

static long
flagwait(n)
	int n;
{
	if (n >= NFLAG)
		return XFAIL;
	xlock();
	if (xt->flag[n]) {
		xt->flag[n] = 0;
		xunlock();
		return 0;
	}
	if (xt->fwait[n]) {
		xunlock();
		return XFAIL;
	}
	xt->fwait[n] = xme + 1;
	for (;;) {
		if (xt->flag[n]) {
			xt->flag[n] = 0;
			xt->fwait[n] = 0;
			xunlock();
			return 0;
		}
		if (xlive() < 2) {		/* nobody else could set it */
			xt->fwait[n] = 0;
			xunlock();
			return XFAIL;
		}
		xunlock();
		b3_conflush();
		nap();
		xlock();
	}
}

static long
flagset(n)
	int n;
{
	long r = 0;

	if (n >= NFLAG)
		return XFAIL;
	xlock();
	if (xt->flag[n] && xt->fwait[n] == 0)
		r = XFAIL;		/* overrun */
	else
		xt->flag[n] = 1;
	xunlock();
	return r;
}

/* ---- queues: made by name, used by index ---- */

static int
qfind(name)
	char *name;
{
	int i;

	for (i = 0; i < NQ; i++)
		if (xt->q[i].used && memcmp(xt->q[i].name, name, QNAME) == 0)
			return i;
	return -1;
}

/* 134: name, msglen.w, depth.w */
static long
qmake(pb)
	unsigned long pb;
{
	int len = b3_get16(pb + QNAME), dep = b3_get16(pb + QNAME + 2), i;
	struct xqueue *q;

	if (len < 1 || len > QMSG || dep < 1 || dep > QDEPTH)
		return XFAIL;
	xlock();
	if (qfind(B3MEM(pb)) >= 0) {
		xunlock();
		return 0;
	}
	for (i = 0; i < NQ && xt->q[i].used; i++)
		;
	if (i == NQ) {
		xunlock();
		return XFAIL;
	}
	q = &xt->q[i];
	memset((char *)q, 0, sizeof *q);
	memcpy(q->name, B3MEM(pb), QNAME);
	q->msglen = len;
	q->ndep = dep;
	q->used = 1;
	xunlock();
	return 0;
}

/* 135: name, id.w out; 136: the named queue goes */
static long
qopen(pb, del)
	unsigned long pb;
	int del;
{
	int i;

	xlock();
	if ((i = qfind(B3MEM(pb))) >= 0) {
		if (del)
			xt->q[i].used = 0;
		else
			b3_put16(pb + QNAME, (unsigned long)i);
	}
	xunlock();
	return i < 0 ? XFAIL : 0;
}

/* 137-140: id.w, message; cond: never wait */
static long
qio(pb, wr, cond)
	unsigned long pb;
	int wr, cond;
{
	int id = b3_get16(pb);
	struct xqueue *q;

	if (id < 0 || id >= NQ)
		return XFAIL;
	q = &xt->q[id];
	xlock();
	for (;;) {
		if (!q->used) {
			xunlock();
			return XFAIL;
		}
		if (wr ? q->cnt < q->ndep : q->cnt > 0)
			break;
		if (cond || xlive() < 2) {
			xunlock();
			return XFAIL;
		}
		xunlock();
		b3_conflush();
		nap();
		xlock();
	}
	if (wr) {
		memcpy(q->buf[q->in], B3MEM(pb + 2), q->msglen);
		q->in = (q->in + 1) % q->ndep;
		q->cnt++;
	} else {
		memcpy(B3MEM(pb + 2), q->buf[q->out], q->msglen);
		q->out = (q->out + 1) % q->ndep;
		q->cnt--;
	}
	xunlock();
	return 0;
}

/* ---- memory: whole partitions; base.l, length.l ---- */

static long
memreq(pb, abs)
	unsigned long pb;
	int abs;
{
	unsigned long base = b3_get32(pb), len = b3_get32(pb + 4);
	int k;

	if (len == 0 || len > XPSIZE)
		return XFAIL;
	xlock();
	if (abs) {
		k = base >= XPART ? (base - XPART) / XPSIZE : -1;
		if (k < 0 || k >= xnpart || xpbase(k) != base || (xlive(), xt->part[k])) {
			xunlock();
			return XFAIL;
		}
		xt->part[k] = xme + 1;
	} else if ((k = xpalloc(xme)) < 0) {
		xunlock();
		return XFAIL;
	}
	xunlock();
	b3_put32(pb, xpbase(k));
	b3_put32(pb + 4, XPSIZE);
	return 0;
}

static long
memfree(pb)
	unsigned long pb;
{
	unsigned long base = b3_get32(pb);
	int k = base >= XPART ? (base - XPART) / XPSIZE : -1;
	long r = XFAIL;

	xlock();
	if (k >= 0 && k < xnpart && xpbase(k) == base && xt->part[k] == xme + 1 && k != xmypart0) {
		xt->part[k] = 0;
		r = 0;
	}
	xunlock();
	return r;
}

/* ---- 144: a program, fcb[36] tlen tail[128], in a partition of its own ---- */

static long
create(pb)
	unsigned long pb;
{
	unsigned long lo, bp, sp;
	int s, k;
	pid_t pid;

	xlock();
	if ((s = xslot((long)getpid())) < 0) {
		xunlock();
		return XFAIL;
	}
	if ((k = xpalloc(s)) < 0) {
		xt->p[s].pid = 0;
		xunlock();
		return XFAIL;
	}
	xt->p[s].con = xt->p[xme].con;
	xt->p[s].prio = xt->p[xme].prio;
	memcpy(xt->p[s].name, B3MEM(pb + 1), 8);
	xunlock();
	lo = xpbase(k);
	if (b3_ldfile(pb, lo, lo + XPSIZE, &bp, &sp) != 0 || (pid = fork()) < 0) {
		xlock();
		xrelease(s);
		xt->p[s].pid = 0;
		xunlock();
		return XFAIL;
	}
	if (pid == 0) {
		xme = s;
		xt->p[s].pid = getpid();
		xmypart0 = k;
		b3_child(bp, sp, *B3MEM(pb + 36) & 0x7f, B3MEM(pb + 37));
	}
	b3_conshared = 1;
	return 0;
}

/* ---- record locks: fcntl byte locks, a byte a record, on a file per host file ---- */

#define	NLOCK	16

static struct {
	long	dev, ino, rec;
} lk[NLOCK];
static int nlk;

/* lock files held open: closing one would drop its locks */
static struct {
	long	dev, ino;
	int	fd;
} lkf[NLOCK];
static int nlkf;

/* the lock file's descriptor, kept when make is set; -1 when there is none */
static int
lkfile(d, h, dev, ino, make, keep)
	int d, make, *keep;
	struct hfile *h;
	long *dev, *ino;
{
	char dir[1200], path[1400];
	struct stat sb;
	int i, fd;

	*keep = 0;
	if (hf_dir(d, h->user, dir, 0) < 0)
		return -1;
	sprintf(path, "%s/%s", dir, h->host);
	if (stat(path, &sb) < 0)
		return -1;
	*dev = sb.st_dev;
	*ino = sb.st_ino;
	for (i = 0; i < nlkf; i++)
		if (lkf[i].dev == *dev && lkf[i].ino == *ino) {
			*keep = 1;
			return lkf[i].fd;
		}
	sprintf(path, "%s/%lx.%lx", lkdir, (long)sb.st_dev, (long)sb.st_ino);
	if ((fd = open(path, make ? O_RDWR | O_CREAT : O_RDWR, 0600)) < 0)
		return -1;
	fcntl(fd, F_SETFD, 1);
	if (make && nlkf < NLOCK) {
		lkf[nlkf].dev = *dev;
		lkf[nlkf].ino = *ino;
		lkf[nlkf++].fd = fd;
		*keep = 1;
	}
	return fd;
}

static int
lkop(fd, type, rec, n)
	int fd, type;
	long rec, n;
{
	struct flock fl;

	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = type;
	fl.l_start = rec;
	fl.l_len = n;
	return fcntl(fd, F_SETLK, &fl);
}

/* a lock file with none of our locks left is closed */
static void
lkgc(dev, ino)
	long dev, ino;
{
	int i, j;

	for (j = 0; j < nlk && !(lk[j].dev == dev && lk[j].ino == ino); j++)
		;
	if (j < nlk)
		return;
	for (i = 0; i < nlkf; i++)
		if (lkf[i].dev == dev && lkf[i].ino == ino) {
			close(lkf[i].fd);
			lkf[i] = lkf[--nlkf];
			return;
		}
}

long
b3_hlock(d, h, rec, n, op)
	int d, op;
	struct hfile *h;
	long rec, n;
{
	struct flock fl;
	long dev, ino, i, j;
	int fd, keep, r = 0;

	if ((fd = lkfile(d, h, &dev, &ino, op == LK_LOCK, &keep)) < 0)
		return op == LK_LOCK ? XFAIL : 0;
	switch (op) {
	case LK_WRITE:
		memset((char *)&fl, 0, sizeof fl);
		fl.l_type = F_WRLCK;
		fl.l_start = rec;
		fl.l_len = 1;
		if (fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK)
			r = 8;
		break;
	case LK_LOCK:
		xt->nlocks = 1;
		for (i = 0; i < n; i++) {
			for (j = 0; j < nlk && !(lk[j].dev == dev && lk[j].ino == ino && lk[j].rec == rec + i); j++)
				;
			if (j < nlk)
				continue;
			if (nlk == NLOCK) {
				r = 14;
				break;
			}
			if (lkop(fd, F_WRLCK, rec + i, 1L) < 0) {
				r = (int)i << 8 | 8;
				break;
			}
			lk[nlk].dev = dev;
			lk[nlk].ino = ino;
			lk[nlk++].rec = rec + i;
		}
		break;
	case LK_UNLOCK:
	case LK_CLOSE:
		for (j = 0; j < nlk; j++)
			if (lk[j].dev == dev && lk[j].ino == ino &&
			    (op == LK_CLOSE || (lk[j].rec >= rec && lk[j].rec < rec + n))) {
				lkop(fd, F_UNLCK, lk[j].rec, 1L);
				lk[j--] = lk[--nlk];
			}
		break;
	}
	if (!keep)
		close(fd);
	else
		lkgc(dev, ino);
	return r;
}

/*
 * 145: MP/M's priorities run from 0 (first) to 255; a user process
 * starts at 200.  Lower ones become a higher nice; Unix gives none back.
 */
static long
setprio(pr)
	int pr;
{
	static int niced;
	int want = pr > 200 ? (pr - 200) / 3 : 0;

	xt->p[xme].prio = pr;
	if (want > 19)
		want = 19;
	if (want > niced) {
		nice(want - niced);
		niced = want;
	}
	return 0;
}

/* a new process holds no locks: what it inherited are its parent's */
void
xforked()
{
	while (nlkf)
		close(lkf[--nlkf].fd);
	nlk = 0;
}

/* ---- the dispatcher ---- */

long
b3_xdos(fn, d1)
	long fn;
	unsigned long d1;
{
	switch (fn) {
	case 128:
	case 129:
		return memreq(d1, fn == 128);
	case 130:
		return memfree(d1);
	case 131:
		if ((d1 & 0xffff) > 2)
			return XFAIL;
		if ((d1 & 0xffff) == 0)
			while (!b3_constat())
				nap();
		return 0;
	case 132:
		return flagwait((int)(d1 & 0xff));
	case 133:
		return flagset((int)(d1 & 0xff));
	case 134:
		return qmake(d1);
	case 135:
	case 136:
		return qopen(d1, fn == 136);
	case 137:
	case 138:
		return qio(d1, 0, fn == 138);
	case 139:
	case 140:
		return qio(d1, 1, fn == 140);
	case 142:
		poll((struct pollfd *)0, 0L, 0);
		return 0;
	case 144:
		return create(d1);
	case 145:
		return setprio((int)(d1 & 0xff));
	case 146:
		return attach();
	case 147:
		return detach();
	case 148:
		return (d1 & 0x0f) == xt->p[xme].con ? 0 : XFAIL;
	case 149:
		return assign(d1);
	case 150:
	case 151:
		return b3_ccpsv(d1, fn == 151);
	case 153:
		return xt->p[xme].con;
	}
	return 0;
}
