/*
 * file.c -- files for MiNT processes: drives and paths onto host
 * directories (the cartridge's drive table), open files over host
 * descriptors, the console, null and in-memory pipes, per-process
 * handles, directory reading.
 *
 * A pipe named tty* is a terminal pair: its creator holds the master,
 * whoever opens the name after gets the terminal, with a line
 * discipline that edits and echoes unless raw.  Sockets are the
 * environment's host sockets, blocking here so the host call never does.
 *
 * Paths: X:\a\b on a table drive is <root>/a/b, each name matched as
 * given, else ignoring case, else by its 8.3 form.  On U:, \dev and
 * \pipe are devices, \x (one letter) is drive X:, \host is /, and
 * anything else is under U:'s own root.
 */
#include "xamint.h"

#define	DTAB	((char *)0xfa0080)
#define	PTZ	(*(long *)0xfa0064)

struct drv {
	char	*root;
	char	*under;			/* read-only fallback for what root lacks */
	short	ro;
};
static struct drv drives[26];

enum { F_HOST, F_CON, F_NULL, F_PIPE };

struct pipe {
	char	buf[4096];
	long	head, tail, count;
	short	links, writers;
	char	name[32];
	struct pipe *next;
	struct pty *pty;
};

struct pty {
	struct pipe	*in, *out;	/* keys to the terminal, its output */
	struct tty	tty;
	short		masters, slaves;
	short		llen, lready;	/* the line being edited; bytes of it done */
	char		line[256];
};
static struct pipe *pipes;

short contty_state;
static struct tty contty;
static char hpbuf[PLEN];

static struct file *fnew(DEVDRV *dev, long info, int flags);

/* ---- devices ---- */

static long _cdecl host_open(FILEPTR *f) { (void)f; return 0; }

static long _cdecl
host_write(FILEPTR *f, const char *buf, long n)
{
	return merr(hc(H_WRITE, 3, f->devinfo, (long)buf, n));
}

static long _cdecl
host_read(FILEPTR *f, char *buf, long n)
{
	return merr(hc(H_READ, 3, f->devinfo, (long)buf, n));
}

static long _cdecl
host_lseek(FILEPTR *f, long where, int whence)
{
	return merr(hc(H_LSEEK, 3, f->devinfo, where, (long)whence));
}

static long _cdecl
host_ioctl(FILEPTR *f, int mode, void *buf)
{
	char st[HST_SIZE];
	long r, pos;

	switch (mode) {
	case FIONREAD:
		if (hc(H_FXSTAT, 3, 2L, f->devinfo, (long)st) < 0)
			return EINVAL;
		pos = hc(H_LSEEK, 3, f->devinfo, 0L, 1L);
		r = HL(st, HST_FSIZE) - pos;
		*(long *)buf = r > 0 ? r : 0;
		return 0;
	case FIONWRITE:
		*(long *)buf = 1;
		return 0;
	case FSTAT:
		if ((r = hc(H_FXSTAT, 3, 2L, f->devinfo, (long)st)) < 0)
			return merr(r);
		st_xattr(st, buf);
		return 0;
	case FSTAT64:
		if ((r = hc(H_FXSTAT, 3, 2L, f->devinfo, (long)st)) < 0)
			return merr(r);
		st_stat64(st, buf);
		return 0;
	}
	return ENOSYS;
}

static long _cdecl
host_datime(FILEPTR *f, ushort *t, int rw)
{
	char st[HST_SIZE];
	long r, d;

	if (rw)
		return 0;
	if ((r = hc(H_FXSTAT, 3, 2L, f->devinfo, (long)st)) < 0)
		return merr(r);
	d = dostime(HL(st, HST_MTIME) + PTZ);
	t[0] = d >> 16;
	t[1] = d;
	return 0;
}

static long _cdecl
host_close(FILEPTR *f, int pid)
{
	(void)pid;
	if (f->links <= 0)
		hc(H_CLOSE, 1, f->devinfo);
	return 0;
}

static long _cdecl
host_select(FILEPTR *f, long proc, int mode)
{
	long pfd[2];

	(void)proc;
	pfd[0] = f->devinfo;
	pfd[1] = (mode == O_RDONLY ? 1L : 4L) << 16;
	if (hc(H_POLL, 3, (long)pfd, 1L, 0L) > 0 && (pfd[1] & 0xffff))
		return 1;
	return 0;
}

static void _cdecl host_unselect(FILEPTR *f, long proc, int mode) { (void)f; (void)proc; (void)mode; }

static DEVDRV hostdev = {
	host_open, host_write, host_read, host_lseek, host_ioctl, host_datime,
	host_close, host_select, host_unselect, 0, 0, 0
};

/* the console: BIOS keyboard in, output to the log */
static long
bios(int fn, int dev)
{
	register long r __asm__("d0");

	__asm__ __volatile__(
		"move.w %2,-(sp)\n\tmove.w %1,-(sp)\n\ttrap #13\n\taddq.l #4,sp"
		: "=r"(r) : "g"(fn), "g"(dev) : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
	return r;
}

static long _cdecl
con_write(FILEPTR *f, const char *buf, long n)
{
	(void)f;
	if (logfd >= 0)
		hc(H_WRITE, 3, (long)logfd, (long)buf, n);
	return n;
}

static long _cdecl
con_read(FILEPTR *f, char *buf, long n)
{
	long i;

	(void)f;
	for (i = 0; i < n; i++) {
		while (!bios(1, 2))
			nap(20);
		buf[i] = bios(2, 2);
		if ((contty.sg.sg_flags & (T_RAW | T_CBREAK)) == 0 && buf[i] == '\r') {
			buf[i++] = '\n';
			break;
		}
		if (!(contty.sg.sg_flags & (T_RAW | T_CBREAK)) && i + 1 < n)
			continue;
		i++;
		break;
	}
	return i;
}

static long _cdecl con_lseek(FILEPTR *f, long w, int h) { (void)f; (void)w; (void)h; return 0; }

static long _cdecl
con_ioctl(FILEPTR *f, int mode, void *buf)
{
	(void)f;
	switch (mode) {
	case FIONREAD:
		*(long *)buf = bios(1, 2) ? 1 : 0;
		return 0;
	case FIONWRITE:
		*(long *)buf = 1;
		return 0;
	case TIOCGETP:
		*(struct sgttyb *)buf = contty.sg;
		return 0;
	case TIOCSETN: case TIOCSETP:
		contty.sg = *(struct sgttyb *)buf;
		return 0;
	case TIOCGPGRP:
		*(long *)buf = contty.pgrp;
		return 0;
	case TIOCSPGRP:
		contty.pgrp = *(long *)buf;
		return 0;
	case TIOCGWINSZ:
		bzero(buf, sizeof contty.wsiz);
		((struct winsize *)buf)->ws_row = 25;
		((struct winsize *)buf)->ws_col = 80;
		return 0;
	case FSTAT: {
		XATTR *x = buf;

		bzero(x, sizeof *x);
		x->mode = S_IFCHR | 0666;
		x->nlink = 1;
		x->blksize = 1;
		return 0;
	}
	}
	return ENOSYS;
}

static long _cdecl con_datime(FILEPTR *f, ushort *t, int rw) { (void)f; (void)rw; t[0] = t[1] = 0; return 0; }
static long _cdecl con_close(FILEPTR *f, int pid) { (void)f; (void)pid; return 0; }

static long _cdecl
con_select(FILEPTR *f, long proc, int mode)
{
	(void)f;
	(void)proc;
	if (mode != O_RDONLY)
		return 1;
	return bios(1, 2) ? 1 : 0;
}

static DEVDRV condev = {
	host_open, con_write, con_read, con_lseek, con_ioctl, con_datime,
	con_close, con_select, host_unselect, 0, 0, 0
};

static long _cdecl null_write(FILEPTR *f, const char *b, long n) { (void)f; (void)b; return n; }
static long _cdecl null_read(FILEPTR *f, char *b, long n) { (void)f; (void)b; (void)n; return 0; }
static long _cdecl null_select(FILEPTR *f, long p, int m) { (void)f; (void)p; (void)m; return 1; }

static DEVDRV nulldev = {
	host_open, null_write, null_read, con_lseek, con_ioctl, con_datime,
	con_close, null_select, host_unselect, 0, 0, 0
};

static long _cdecl
pipe_write(FILEPTR *f, const char *buf, long n)
{
	struct pipe *p = (struct pipe *)f->devinfo;
	long i;

	for (i = 0; i < n; i++) {
		while (p->count == sizeof p->buf) {
			if (f->flags & O_NDELAY)
				return i ? i : EAGAIN;
			sleep(IO_Q, (long)p);
		}
		p->buf[p->head] = buf[i];
		p->head = (p->head + 1) % sizeof p->buf;
		p->count++;
	}
	wake(IO_Q, (long)p);
	return n;
}

static long _cdecl
pipe_read(FILEPTR *f, char *buf, long n)
{
	struct pipe *p = (struct pipe *)f->devinfo;
	long i;

	while (p->count == 0) {
		if ((f->flags & O_NDELAY) || p->writers == 0)
			return 0;
		sleep(IO_Q, (long)p);
	}
	for (i = 0; i < n && p->count; i++) {
		buf[i] = p->buf[p->tail];
		p->tail = (p->tail + 1) % sizeof p->buf;
		p->count--;
	}
	wake(IO_Q, (long)p);
	return i;
}

static long _cdecl
pipe_ioctl(FILEPTR *f, int mode, void *buf)
{
	struct pipe *p = (struct pipe *)f->devinfo;

	switch (mode) {
	case FIONREAD:
		*(long *)buf = p->count;
		return 0;
	case FIONWRITE:
		*(long *)buf = sizeof p->buf - p->count;
		return 0;
	}
	return con_ioctl(f, mode, buf);
}

static long _cdecl
pipe_close(FILEPTR *f, int pid)
{
	struct pipe *p = (struct pipe *)f->devinfo;

	(void)pid;
	if (f->links > 0)
		return 0;
	if ((f->flags & O_RWMODE) != O_RDONLY)
		p->writers--;
	wake(IO_Q, (long)p);
	if (--p->links == 0 && !p->name[0])
		kfree_any(p);
	return 0;
}

static long _cdecl
pipe_select(FILEPTR *f, long proc, int mode)
{
	struct pipe *p = (struct pipe *)f->devinfo;

	(void)proc;
	if (mode == O_RDONLY)
		return p->count > 0 || p->writers == 0;
	return p->count < (long)sizeof p->buf;
}

static DEVDRV pipedev = {
	host_open, pipe_write, pipe_read, con_lseek, pipe_ioctl, con_datime,
	pipe_close, pipe_select, host_unselect, 0, 0, 0
};

/* ---- terminal pairs ---- */

static int
pipe_put(struct pipe *p, const char *b, long n)
{
	long i;

	for (i = 0; i < n; i++) {
		while (p->count == sizeof p->buf)
			sleep(IO_Q, (long)p);
		p->buf[p->head] = b[i];
		p->head = (p->head + 1) % sizeof p->buf;
		p->count++;
	}
	wake(IO_Q, (long)p);
	return n;
}

/* the next key, -1 at end, EAGAIN when none and nonblocking */
static long
pipe_get(struct pipe *p, int ndelay)
{
	unsigned char c;

	while (p->count == 0) {
		if (p->writers == 0)
			return -1;
		if (ndelay)
			return EAGAIN;
		sleep(IO_Q, (long)p);
	}
	c = p->buf[p->tail];
	p->tail = (p->tail + 1) % sizeof p->buf;
	p->count--;
	wake(IO_Q, (long)p);
	return c;
}

static long _cdecl
ptm_write(FILEPTR *f, const char *buf, long n)
{
	struct pty *t = (struct pty *)f->devinfo;
	struct kp *k;
	struct file *s;
	long i;

	for (i = 0; i < n; i++) {
		if (buf[i] == 3 && !(t->tty.sg.sg_flags & T_RAW)) {
			for (k = allprocs; k; k = k->all)
				if (!k->zombie && (s = fd_get(k, 0)) && s->devinfo == (long)t &&
				    s->dev != f->dev)
					post_sig(k, SIGINT);
			continue;
		}
		pipe_put(t->in, buf + i, 1);
	}
	return n;
}

static long _cdecl
ptm_read(FILEPTR *f, char *buf, long n)
{
	struct pty *t = (struct pty *)f->devinfo;
	long i, c;

	for (i = 0; i < n; i++) {
		c = pipe_get(t->out, i || (f->flags & O_NDELAY));
		if (c < 0)
			return i ? i : c == -1 ? 0 : c;
		buf[i] = c;
	}
	return i;
}

static long _cdecl
ptm_select(FILEPTR *f, long proc, int mode)
{
	struct pty *t = (struct pty *)f->devinfo;

	(void)proc;
	if (mode == O_RDONLY)
		return t->out->count > 0 || t->out->writers == 0;
	return t->in->count < (long)sizeof t->in->buf;
}

static void
pty_free(struct pty *t)
{
	struct pipe **pp;

	if (t->masters || t->slaves)
		return;
	for (pp = &pipes; *pp; pp = &(*pp)->next)
		if (*pp == t->in) {
			*pp = t->in->next;
			break;
		}
	kfree_any(t->in);
	kfree_any(t->out);
	kfree_any(t);
}

static long _cdecl
ptm_close(FILEPTR *f, int pid)
{
	struct pty *t = (struct pty *)f->devinfo;

	(void)pid;
	if (f->links > 0)
		return 0;
	t->masters--;
	t->in->writers--;
	wake(IO_Q, (long)t->in);
	pty_free(t);
	return 0;
}

static long _cdecl
pts_write(FILEPTR *f, const char *buf, long n)
{
	struct pty *t = (struct pty *)f->devinfo;
	long i, j;

	for (i = j = 0; i < n; i++)
		if (buf[i] == '\n' && (t->tty.sg.sg_flags & T_CRMOD)) {
			pipe_put(t->out, buf + j, i - j);
			pipe_put(t->out, "\r", 1);
			j = i;
		}
	pipe_put(t->out, buf + j, n - j);
	return n;
}

static void
echo(struct pty *t, const char *s, int n)
{
	if (t->tty.sg.sg_flags & T_ECHO)
		pipe_put(t->out, s, n);
}

/* raw: what has come; cooked: a line, edited and echoed as it comes */
static long _cdecl
pts_read(FILEPTR *f, char *buf, long n)
{
	struct pty *t = (struct pty *)f->devinfo;
	long i, c;

	if (t->tty.sg.sg_flags & (T_RAW | T_CBREAK)) {
		for (i = 0; i < n; i++) {
			c = pipe_get(t->in, i || (f->flags & O_NDELAY));
			if (c < 0)
				return i ? i : c == -1 ? 0 : c;
			buf[i] = c;
			if (!(t->tty.sg.sg_flags & T_RAW) && (t->tty.sg.sg_flags & T_ECHO))
				echo(t, buf + i, 1);
		}
		return i;
	}
	while (!t->lready) {
		c = pipe_get(t->in, f->flags & O_NDELAY);
		if (c == EAGAIN)
			return c;
		if (c < 0 || (c == 4 && t->llen == 0))
			return 0;
		if (c == 8 || c == 127) {
			if (t->llen > 0) {
				t->llen--;
				echo(t, "\b \b", 3);
			}
			continue;
		}
		if (c == '\r' || c == '\n') {
			echo(t, "\r\n", 2);
			t->line[t->llen++] = '\n';
			t->lready = t->llen;
			break;
		}
		if (t->llen < (int)sizeof t->line - 1) {
			t->line[t->llen++] = c;
			echo(t, (char *)&t->line[t->llen - 1], 1);
		}
	}
	if (n > t->lready)
		n = t->lready;
	memcpy(buf, t->line, n);
	for (i = n; i < t->llen; i++)
		t->line[i - n] = t->line[i];
	t->llen -= n;
	t->lready -= n;
	return n;
}

static long _cdecl
pts_select(FILEPTR *f, long proc, int mode)
{
	struct pty *t = (struct pty *)f->devinfo;

	(void)proc;
	if (mode == O_RDONLY)
		return t->lready || t->in->count > 0 || t->in->writers == 0;
	return t->out->count < (long)sizeof t->out->buf;
}

static long _cdecl
pts_ioctl(FILEPTR *f, int mode, void *buf)
{
	struct pty *t = (struct pty *)f->devinfo;

	switch (mode) {
	case FIONREAD:
		*(long *)buf = t->lready ? t->lready : t->in->count;
		return 0;
	case FIONWRITE:
		*(long *)buf = sizeof t->out->buf - t->out->count;
		return 0;
	case TIOCGETP:
		*(struct sgttyb *)buf = t->tty.sg;
		return 0;
	case TIOCSETN: case TIOCSETP:
		t->tty.sg = *(struct sgttyb *)buf;
		return 0;
	case TIOCGPGRP:
		*(long *)buf = t->tty.pgrp;
		return 0;
	case TIOCSPGRP:
		t->tty.pgrp = *(long *)buf;
		return 0;
	case TIOCGWINSZ:
		*(struct winsize *)buf = t->tty.wsiz;
		return 0;
	case TIOCSWINSZ:
		t->tty.wsiz = *(struct winsize *)buf;
		return 0;
	}
	return con_ioctl(f, mode, buf);
}

static long _cdecl
pts_close(FILEPTR *f, int pid)
{
	struct pty *t = (struct pty *)f->devinfo;

	(void)pid;
	if (f->links > 0)
		return 0;
	t->slaves--;
	t->out->writers--;
	wake(IO_Q, (long)t->out);
	pty_free(t);
	return 0;
}

static DEVDRV ptmdev = {
	host_open, ptm_write, ptm_read, con_lseek, pts_ioctl, con_datime,
	ptm_close, ptm_select, host_unselect, 0, 0, 0
};

static DEVDRV ptsdev = {
	host_open, pts_write, pts_read, con_lseek, pts_ioctl, con_datime,
	pts_close, pts_select, host_unselect, 0, 0, 0
};

struct tty *
tty_of(struct file *f)
{
	return f && f->dev == &ptsdev ? &((struct pty *)f->devinfo)->tty : 0;
}

int
is_tty(struct file *f)
{
	return f && (f->dev == &ptsdev || f->dev == &condev);
}

/* ---- sockets ---- */

struct tsock {
	long	op, fd, arg;
	char	*buf;
	long	len, rv;
	char	addr[16];
	long	alen;
	char	*gap;
};

/* until host descriptor hfd can read (or write), unless nonblocking */
long
host_wait(long hfd, int wr, int nonblock)
{
	long pfd[2];

	if (nonblock)
		return EAGAIN;
	for (;;) {
		pfd[0] = hfd;
		pfd[1] = (wr ? 4L : 1L) << 16;
		if (hc(H_POLL, 3, (long)pfd, 1L, 0L) > 0 && (pfd[1] & 0xffff))
			return 0;
		if (curproc->sigpending & ~curproc->p_sigmask)
			return EINTR;
		nap(20);
	}
}

/*
 * A socket call of the environment's (op: the host's TSO_*), waiting
 * here while it would block.  addr (16 bytes) goes in with *alenp and
 * comes back from accept, recv and the names.
 */
long
sock_op(int op, long hfd, long arg, char *buf, long len, char *addr, long *alenp, int ndelay)
{
	static char gap[576];
	struct tsock so;
	long r;

	for (;;) {
		bzero(&so, sizeof so);
		so.op = op;
		so.fd = hfd;
		so.arg = arg;
		so.buf = buf;
		so.len = len;
		so.gap = gap;
		if (addr && alenp && *alenp > 0 && op != 5 && op != 7 && op != 9) {
			so.alen = *alenp > 16 ? 16 : *alenp;
			memcpy(so.addr, addr, so.alen);
		}
		r = hc(H_IOCTL, 3, *(long *)0xfa0078, (long)TOSIOC_SOCK, (long)&so);
		if (r >= 0) {
			if (addr && alenp && (op == 5 || op == 7 || op == 9)) {
				memcpy(addr, so.addr, *alenp < 0 ? 0 : *alenp < 16 ? *alenp : 16);
				*alenp = 16;
			}
			return so.rv;
		}
		if ((op != 5 && op != 6 && op != 7) || r != -H_EAGAIN ||
		    (r = host_wait(hfd, op == 6, ndelay)) != 0) {
			if (r != EAGAIN && r != EINTR && r != -H_EINPROGRESS)
				xlog("socket call %d on %ld: host error %ld", op, hfd, -r);
			return r == EAGAIN || r == EINTR ? r : merr(r);
		}
	}
}

static long _cdecl
sock_write(FILEPTR *f, const char *buf, long n)
{
	return sock_op(6, f->devinfo, 0, (char *)buf, n, 0, 0, f->flags & O_NDELAY);
}

static long _cdecl
sock_read(FILEPTR *f, char *buf, long n)
{
	return sock_op(7, f->devinfo, 0, buf, n, 0, 0, f->flags & O_NDELAY);
}

static long _cdecl
sock_ioctl(FILEPTR *f, int mode, void *buf)
{
	long n = 0;

	switch (mode) {
	case FIONREAD:
		if (hc(H_IOCTL, 3, f->devinfo, 0x5301L, (long)&n) < 0)
			n = 0;
		*(long *)buf = n;
		return 0;
	case FIONWRITE:
		*(long *)buf = 1;
		return 0;
	}
	return host_ioctl(f, mode, buf);
}

static DEVDRV sockdev = {
	host_open, sock_write, sock_read, host_lseek, sock_ioctl, host_datime,
	host_close, host_select, host_unselect, 0, 0, 0
};

/* host socket hfd as a file */
long
file_sock(long hfd, int flags, struct file **res)
{
	hc(H_FCNTL, 3, hfd, 4L, (long)HO_NONBLOCK);
	*res = fnew(&sockdev, hfd, O_RDWR | (flags & O_NDELAY));
	if (!*res) {
		hc(H_CLOSE, 1, hfd);
		return ENOMEM;
	}
	return 0;
}

/* a socket file's host descriptor, -1 for other files */
long
sock_hfd(struct file *f)
{
	return f && f->dev == &sockdev ? f->devinfo : -1;
}

/* ---- the drive table ---- */

void
file_init(void)
{
	char *p = DTAB;
	int d;

	while (*p) {
		d = (*p | 0x20) - 'a';
		if (d >= 0 && d < 26) {
			if (p[1] & 2)
				drives[d].under = p + 2;
			else {
				drives[d].root = p + 2;
				drives[d].ro = p[1] & 1;
			}
		}
		p += 2;
		p += strlen(p) + 1;
	}
	contty.sg.sg_flags = T_ECHO | T_CRMOD | T_TOS;
	contty.state = 0;
}

long
drvmap(void)
{
	long m = 0;
	int d;

	for (d = 0; d < 26; d++)
		if (drives[d].root)
			m |= 1L << d;
	return m;
}

/* ---- names ---- */

static int
up(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int
okc(int c)
{
	const char *p;

	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return 1;
	for (p = "!#$%&'()-@^_`{}~"; *p; p++)
		if (*p == c)
			return 1;
	return 0;
}

/* the 8.3 name TOS sees for host name n */
void
short83(const char *n, char *o)
{
	const char *d = 0, *p;
	int i, b = 0, e = 0, lo = 0, hi = 0, bad = n[0] == '.';
	unsigned long h = 0;

	if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) {
		strcpy(o, n);
		return;
	}
	for (p = n; *p; p++) {
		h = h * 31 + (*p & 0xff);
		if (*p == '.') {
			bad |= d != 0;
			d = p;
		} else if (!okc(*p))
			bad = 1;
		else if (d)
			e++;
		else
			b++;
		lo |= *p >= 'a' && *p <= 'z';
		hi |= *p >= 'A' && *p <= 'Z';
	}
	if (!bad && !(lo && hi) && b >= 1 && b <= 8 && e <= 3 && !(d && e == 0)) {
		for (p = n; *p; p++)
			*o++ = up(*p);
		*o = 0;
		return;
	}
	d = 0;
	for (p = n + 1; *p; p++)
		if (*p == '.')
			d = p;
	for (i = 0, p = n; *p && p != d && i < 3; p++)
		if (okc(*p))
			o[i++] = up(*p);
	if (i == 0)
		o[i++] = '_';
	o[i++] = '~';
	for (b = 12; b >= 0; b -= 4)
		o[i++] = "0123456789ABCDEF"[(h >> b) & 15];
	if (d) {
		o[i++] = '.';
		for (e = 0, p = d + 1; *p && e < 3; p++)
			if (okc(*p)) {
				o[i++] = up(*p);
				e++;
			}
		if (e == 0)
			i--;
	}
	o[i] = 0;
}

long
hstat(const char *hp, char *st, int link)
{
	return hc(link ? H_LXSTAT : H_XSTAT, 3, 2L, (long)hp, (long)st);
}

/* name n in host directory dir: as given, else ignoring case or as 8.3 */
static void
match(char *dir, const char *n)
{
	char st[HST_SIZE], s[14], buf[512];
	long fd, cnt, i, dl = strlen(dir);
	struct { long ino, off; unsigned short reclen; char name[1]; } *de;

	strcat(dir, "/");
	strcat(dir, n);
	if (hstat(dir, st, 1) == 0)
		return;
	dir[dl] = 0;
	fd = hc(H_OPEN, 3, (long)(dl ? dir : "/"), 0L, 0L);
	if (fd >= 0) {
		while ((cnt = hc(H_GETDENTS, 3, fd, (long)buf, (long)sizeof buf)) > 0)
			for (i = 0; i < cnt; i += de->reclen) {
				de = (void *)(buf + i);
				if (stricmp(de->name, n) == 0)
					goto found;
				short83(de->name, s);
				if (stricmp(s, n) == 0)
					goto found;
			}
		hc(H_CLOSE, 1, fd);
	}
	strcat(dir, "/");
	strcat(dir, n);
	return;
found:
	strcat(dir, "/");
	strcat(dir, de->name);
	hc(H_CLOSE, 1, fd);
}

/* host path of components s below root r, into out */
static void
below(const char *r, const char *s, char *out)
{
	char n[PLEN];
	int i;

	strcpy(out, r);
	i = strlen(out);
	if (i && out[i - 1] == '/')
		out[i - 1] = 0;
	while (*s) {
		while (*s == '\\' || *s == '/')
			s++;
		for (i = 0; *s && *s != '\\' && *s != '/' && i < PLEN - 1; )
			n[i++] = *s++;
		n[i] = 0;
		if (i && strlen(out) + i + 2 < PLEN)
			match(out, n);
	}
	if (!out[0])
		strcpy(out, "/");
}

/*
 * A MiNT path to an absolute one, "X:\a\b" with . and .. gone; its
 * drive.  -1 for a bad drive.
 */
static int
canon(const char *m, char *out)
{
	struct kp *k = KP(curproc);
	char t[PLEN * 2], *c[64], *s;
	int drv, n = 0, i;

	if (m[0] && m[1] == ':') {
		drv = (m[0] | 0x20) - 'a';
		m += 2;
	} else
		drv = k->p.p_cwd->curdrv;
	if (drv < 0 || drv >= 26)
		return -1;
	t[0] = 0;
	if (*m != '\\' && *m != '/' && k->dirs[drv])
		strcpy(t, k->dirs[drv]);
	strcat(t, "\\");
	if (strlen(t) + strlen(m) >= sizeof t - 1)
		return -1;
	strcat(t, m);
	for (s = t; *s; ) {
		while (*s == '\\' || *s == '/')
			*s++ = 0;
		if (!*s)
			break;
		if (s[0] == '.' && (s[1] == 0 || s[1] == '\\' || s[1] == '/'))
			;
		else if (s[0] == '.' && s[1] == '.' && (s[2] == 0 || s[2] == '\\' || s[2] == '/')) {
			if (n)
				n--;
		} else if (n < 64)
			c[n++] = s;
		while (*s && *s != '\\' && *s != '/')
			s++;
	}
	out[0] = 'A' + drv;
	out[1] = ':';
	out[2] = 0;
	for (i = 0; i < n; i++) {
		strcat(out, "\\");
		strcat(out, c[i]);
	}
	if (n == 0)
		strcat(out, "\\");
	return drv;
}

/*
 * A MiNT path as a host path in out: 0, or 1 for u:\dev\x and 2 for
 * u:\pipe\x (out the name), -1 for none.
 */
int
host_path(const char *mpath, char *out, int follow)
{
	char a[PLEN * 2], st[HST_SIZE], alt[PLEN];
	const char *rest;
	int drv;

	(void)follow;
	drv = canon(mpath, a);
	if (drv < 0)
		return -1;
	rest = a + 2;
	if (drv == 'u' - 'a') {
		const char *s = rest + 1;

		if (strnicmp(s, "dev\\", 4) == 0 || stricmp(s, "dev") == 0) {
			strcpy(out, s[3] ? s + 4 : "");
			return 1;
		}
		if (strnicmp(s, "pipe\\", 5) == 0) {
			strcpy(out, s + 5);
			return 2;
		}
		if (strnicmp(s, "host", 4) == 0 && (s[4] == 0 || s[4] == '\\')) {
			below("/", s + 4, out);
			return 0;
		}
		if (s[0] && (s[1] == 0 || s[1] == '\\')) {
			drv = (s[0] | 0x20) - 'a';
			if (drv >= 0 && drv < 26 && drives[drv].root && drv != 'u' - 'a')
				rest = s + 1;
			else
				drv = 'u' - 'a';
		}
	}
	if (!drives[drv].root)
		return -1;
	below(drives[drv].root, rest, out);
	if (drives[drv].under && hstat(out, st, 1) < 0) {
		below(drives[drv].under, rest, alt);
		if (hstat(alt, st, 1) == 0)
			strcpy(out, alt);
	}
	return 0;
}

int
drive_ro(const char *mpath)
{
	char a[PLEN * 2];
	int d = canon(mpath, a);

	return d >= 0 && drives[d].ro;
}

/* the current directory of drive drv, below its root */
void
dir_set(struct kp *k, int drv, const char *path)
{
	char *n;

	if (drv < 0 || drv >= 26)
		return;
	while (*path == '\\' || *path == '/')
		path++;
	n = kmalloc_o(strlen(path) + 2, 0);
	if (!n)
		return;
	strcpy(n, path);
	if (k->dirs[drv])
		kfree_any(k->dirs[drv]);
	k->dirs[drv] = n;
}

long
dir_change(const char *mpath)
{
	char a[PLEN * 2], st[HST_SIZE];
	int drv;

	drv = canon(mpath, a);
	if (drv < 0)
		return EDRIVE;
	if (host_path(a, hpbuf, 1) == 0) {
		if (hstat(hpbuf, st, 0) < 0 || (HL(st, HST_MODE) & 0170000) != 0040000)
			return EPATH;
	}
	dir_set(KP(curproc), drv, a + 3);
	return 0;
}

/* ---- stat ---- */

static ushort
mmode(long m)
{
	ushort r = m & 07777;

	switch (m & 0170000) {
	case 0040000: r |= S_IFDIR; break;
	case 0020000: r |= S_IFCHR; break;
	case 0060000: r |= S_IFBLK; break;
	case 0010000: r |= S_IFIFO; break;
	case 0120000: r |= S_IFLNK; break;
	case 0140000: r |= S_IFSOCK; break;
	default: r |= S_IFREG;
	}
	return r;
}

void
st_xattr(char *st, XATTR *x)
{
	long t;

	bzero(x, sizeof *x);
	x->mode = mmode(HL(st, HST_MODE));
	x->index = HL(st, HST_INO);
	x->dev = HL(st, HST_DEV);
	x->rdev = HL(st, HST_RDEV);
	x->nlink = HL(st, HST_NLINK);
	x->uid = HL(st, HST_UID);
	x->gid = HL(st, HST_GID);
	x->size = HL(st, HST_FSIZE);
	x->blksize = HL(st, HST_BLKSIZE);
	x->nblocks = (HL(st, HST_BLOCKS) * 512 + x->blksize - 1) / (x->blksize ? x->blksize : 1);
	t = dostime(HL(st, HST_MTIME) + PTZ);
	x->mtime = t >> 16;
	x->mdate = t;
	t = dostime(HL(st, HST_ATIME) + PTZ);
	x->atime = t >> 16;
	x->adate = t;
	t = dostime(HL(st, HST_CTIME) + PTZ);
	x->ctime = t >> 16;
	x->cdate = t;
	if (S_ISDIR(x->mode))
		x->attr |= FA_DIR;
	if (!(x->mode & 0222))
		x->attr |= FA_RDONLY;
}

void
st_stat64(char *st, STAT *s)
{
	bzero(s, sizeof *s);
	s->dev = HL(st, HST_DEV);
	s->ino = HL(st, HST_INO);
	s->mode = mmode(HL(st, HST_MODE));
	s->nlink = HL(st, HST_NLINK);
	s->uid = HL(st, HST_UID);
	s->gid = HL(st, HST_GID);
	s->rdev = HL(st, HST_RDEV);
	s->atime.time = HL(st, HST_ATIME);
	s->mtime.time = HL(st, HST_MTIME);
	s->ctime.time = HL(st, HST_CTIME);
	s->size = HL(st, HST_FSIZE);
	s->blocks = HL(st, HST_BLOCKS);
	s->blksize = HL(st, HST_BLKSIZE);
}

/* ---- open files ---- */

static struct file *
fnew(DEVDRV *dev, long info, int flags)
{
	struct file *f = kmalloc_o(sizeof *f, 0);

	if (!f)
		return 0;
	bzero(f, sizeof *f);
	f->links = 1;
	f->flags = flags;
	f->devinfo = info;
	f->dev = dev;
	return f;
}

static struct pipe *
pipe_new(const char *name)
{
	struct pipe *p = kmalloc_o(sizeof *p, 0);

	if (!p)
		return 0;
	bzero(p, sizeof *p);
	strncpy(p->name, name, sizeof p->name - 1);
	if (name[0]) {
		p->next = pipes;
		pipes = p;
		p->links = 1;
	}
	return p;
}

static long
hflags(int m)
{
	long h = m & O_RWMODE;

	if (h == 3)
		h = HO_RDWR;
	if (m & O_APPEND)
		h |= HO_APPEND;
	if (m & O_CREAT)
		h |= HO_CREAT;
	if (m & O_TRUNC)
		h |= HO_TRUNC;
	if (m & O_EXCL)
		h |= HO_EXCL;
	return h | HO_NOCTTY;
}

/* a terminal pair named name: the master for its creator */
static long
pty_new(const char *name, int mode, struct file **res)
{
	struct pty *t = kmalloc_o(sizeof *t, 0);

	if (!t)
		return ENOMEM;
	bzero(t, sizeof *t);
	t->in = pipe_new(name);
	t->out = pipe_new("");
	*res = fnew(&ptmdev, (long)t, (mode & ~O_RWMODE) | O_RDWR);
	if (!t->in || !t->out || !*res)
		return ENOMEM;
	t->in->pty = t;
	t->in->writers = 1;
	t->masters = 1;
	t->tty.sg.sg_flags = T_ECHO | T_CRMOD | T_TOS;
	t->tty.sg.sg_erase = 8;
	t->tty.sg.sg_kill = 21;
	t->tty.wsiz.ws_row = 24;
	t->tty.wsiz.ws_col = 80;
	return 0;
}

/* an open file for a MiNT path */
long
file_open(const char *path, int mode, int perm, struct file **res)
{
	char hp[PLEN];
	struct pipe *p;
	struct file *f;
	long fd;
	int r = host_path(path, hp, 1);

	*res = 0;
	if (r < 0)
		return EPATH;
	if (r == 1) {
		if (stricmp(hp, "console") == 0 || stricmp(hp, "tty") == 0 || stricmp(hp, "stdin") == 0)
			f = fnew(&condev, 0, mode);
		else if (stricmp(hp, "null") == 0)
			f = fnew(&nulldev, 0, mode);
		else {
			char d[PLEN];

			strcpy(d, "/dev/");
			strcat(d, hp);
			fd = hc(H_OPEN, 3, (long)d, hflags(mode) | HO_NONBLOCK, 0666L);
			if (fd < 0)
				return merr(fd);
			f = fnew(&hostdev, fd, mode);
		}
		*res = f;
		return f ? 0 : ENOMEM;
	}
	if (r == 2) {
		for (p = pipes; p; p = p->next)
			if (stricmp(p->name, hp) == 0)
				break;
		if (p && p->pty) {
			f = fnew(&ptsdev, (long)p->pty, mode);
			if (!f)
				return ENOMEM;
			p->pty->slaves++;
			p->pty->out->writers++;
			*res = f;
			return 0;
		}
		if (!p && (mode & O_CREAT) && strnicmp(hp, "tty", 3) == 0)
			return pty_new(hp, mode, res);
		if (!p) {
			if (!(mode & O_CREAT))
				return EFILNF;
			p = pipe_new(hp);
			if (!p)
				return ENOMEM;
		}
		f = fnew(&pipedev, (long)p, mode);
		if (!f)
			return ENOMEM;
		p->links++;
		if ((mode & O_RWMODE) != O_RDONLY)
			p->writers++;
		*res = f;
		return 0;
	}
	if ((mode & (O_RWMODE | O_CREAT | O_TRUNC)) && drive_ro(path))
		return EACCES;
	fd = hc(H_OPEN, 3, (long)hp, hflags(mode), (long)(perm & 07777));
	if (fd < 0)
		return merr(fd);
	f = fnew(&hostdev, fd, mode);
	if (!f) {
		hc(H_CLOSE, 1, fd);
		return ENOMEM;
	}
	*res = f;
	return 0;
}

long
file_close(struct file *f)
{
	if (!f)
		return EBADF;
	if (--f->links > 0)
		return 0;
	(*f->dev->close)(f, curproc->pid);
	kfree_any(f);
	return 0;
}

long
file_read(struct file *f, char *buf, long n)
{
	if (!f)
		return EBADF;
	if ((f->flags & O_RWMODE) == O_WRONLY)
		return EACCES;
	return (*f->dev->read)(f, buf, n);
}

long
file_write(struct file *f, const char *buf, long n)
{
	if (!f)
		return EBADF;
	if ((f->flags & O_RWMODE) == O_RDONLY && f->dev == &hostdev)
		return EACCES;
	return (*f->dev->write)(f, buf, n);
}

long
file_select(struct file *f, int wr)
{
	if (!f)
		return 1;
	return (*f->dev->select)(f, (long)curproc, wr ? O_WRONLY : O_RDONLY);
}

long
file_getchar(struct file *f, int mode)
{
	unsigned char c;
	long r;

	if (!f)
		return EBADF;
	if (f->dev == &condev) {
		while (!bios(1, 2))
			nap(20);
		r = bios(2, 2);
		return mode == RAW ? r : r & 0xff;
	}
	if (f->dev == &ptsdev) {
		r = pipe_get(((struct pty *)f->devinfo)->in, 0);
		return r < 0 ? MiNTEOF : r;
	}
	r = file_read(f, (char *)&c, 1);
	return r == 1 ? c : MiNTEOF;
}

/* a pipe pair: [0] reads, [1] writes */
long
file_pipe(struct file **rd, struct file **wr)
{
	struct pipe *p = pipe_new("");

	if (!p)
		return ENOMEM;
	*rd = fnew(&pipedev, (long)p, O_RDONLY);
	*wr = fnew(&pipedev, (long)p, O_WRONLY);
	p->links = 2;
	p->writers = 1;
	return 0;
}

int
is_console(struct file *f)
{
	return f && f->dev == &condev;
}

/* ---- handles ---- */

struct file *
fd_get(struct kp *k, int fd)
{
	if (fd < MIN_HANDLE || fd >= NOFILE)
		return 0;
	return k->p.p_fd->ofiles[fd];
}

long
fd_alloc(struct kp *k, struct file *f, int from)
{
	int i;

	for (i = from; i < NOFILE; i++)
		if (!k->p.p_fd->ofiles[i]) {
			k->p.p_fd->ofiles[i] = f;
			k->p.p_fd->ofileflags[i] = 0;
			return i;
		}
	return EMFILE;
}

/* a child's handles: the parent's, shared */
void
fd_inherit(struct kp *child, struct kp *parent)
{
	int i;
	struct file *f;

	for (i = MIN_HANDLE; i < NOFILE; i++) {
		f = parent->p.p_fd->ofiles[i];
		if (f && !(parent->p.p_fd->ofileflags[i] & FD_CLOEXEC)) {
			f->links++;
			child->p.p_fd->ofiles[i] = f;
		}
	}
}

void
fd_closeall(struct kp *k)
{
	int i;

	if (k->p.p_fd != &k->fd) {
		k->p.p_fd->links--;
		return;
	}
	for (i = MIN_HANDLE; i < NOFILE; i++)
		if (k->fd.ofiles[i]) {
			file_close(k->fd.ofiles[i]);
			k->fd.ofiles[i] = 0;
		}
}

/* standard handles of the first process: console in, log out */
void
fd_std(struct kp *k)
{
	struct file *f;
	int i;

	for (i = MIN_HANDLE; i < 3; i++) {
		f = fnew(&condev, 0, i == 0 ? O_RDONLY : O_RDWR);
		k->p.p_fd->ofiles[i] = f;
	}
}

/* ---- directories (MiNT's DIR, our state in fsstuff) ---- */

struct hdir {
	long	fd;
	long	pos, len;
	short	tos;
	char	path[PLEN];
	char	buf[1024];
};

#define	HD(d)	(*(struct hdir **)(d)->fsstuff)

long
dir_open(DIR *d, const char *mpath, int tos)
{
	struct hdir *h;
	char st[HST_SIZE];

	bzero(d, sizeof *d);
	if (host_path(mpath, hpbuf, 1) != 0)
		return EPATH;
	if (hstat(hpbuf, st, 0) < 0 || (HL(st, HST_MODE) & 0170000) != 0040000)
		return EPATH;
	h = kmalloc_o(sizeof *h, 0);
	if (!h)
		return ENOMEM;
	bzero(h, 32);
	strcpy(h->path, hpbuf);
	h->fd = hc(H_OPEN, 3, (long)hpbuf, 0L, 0L);
	if (h->fd < 0) {
		long e = merr(h->fd);

		kfree_any(h);
		return e;
	}
	h->tos = tos;
	d->flags = tos ? TOS_SEARCH : 0;
	HD(d) = h;
	return 0;
}

/* the next host name, or 0 */
static char *
dir_next(DIR *d, long *ino)
{
	struct hdir *h = HD(d);
	struct { long ino, off; unsigned short reclen; char name[1]; } *de;

	for (;;) {
		if (h->pos >= h->len) {
			h->len = hc(H_GETDENTS, 3, h->fd, (long)h->buf, (long)sizeof h->buf);
			h->pos = 0;
			if (h->len <= 0)
				return 0;
		}
		de = (void *)(h->buf + h->pos);
		h->pos += de->reclen;
		d->index++;
		if (de->name[0] == '.' && (de->name[1] == 0 || (de->name[1] == '.' && de->name[2] == 0)))
			if (h->tos)
				continue;
		if (strcmp(de->name, ".env") == 0)
			continue;
		*ino = de->ino;
		return de->name;
	}
}

/* Dreaddir's entry: a 4-byte index then the name, or the 8.3 name alone */
long
dir_read(DIR *d, char *buf, int len, char *st)
{
	struct hdir *h = HD(d);
	char *n, s[14], hp[PLEN];
	long ino;

	if (!h)
		return EBADF;
	n = dir_next(d, &ino);
	if (!n)
		return ENMFILES;
	if (st) {
		strcpy(hp, h->path);
		strcat(hp, "/");
		strcat(hp, n);
		if (hstat(hp, st, 1) < 0)
			bzero(st, HST_SIZE);
	}
	if (h->tos) {
		short83(n, s);
		if ((long)strlen(s) >= len)
			return ENAMETOOLONG;
		strcpy(buf, s);
		return 0;
	}
	if ((long)strlen(n) + 4 >= len)
		return ENAMETOOLONG;
	*(long *)buf = ino;
	strcpy(buf + 4, n);
	return 0;
}

long
dir_rewind(DIR *d)
{
	struct hdir *h = HD(d);

	if (!h)
		return EBADF;
	hc(H_LSEEK, 3, h->fd, 0L, 0L);
	h->pos = h->len = 0;
	d->index = 0;
	return 0;
}

long
dir_close(DIR *d)
{
	struct hdir *h = HD(d);

	if (!h)
		return EBADF;
	hc(H_CLOSE, 1, h->fd);
	kfree_any(h);
	HD(d) = 0;
	return 0;
}

long _cdecl
kernel_opendir(struct dirstruct *dirh, const char *name)
{
	return dir_open(dirh, name, 0);
}

long _cdecl
kernel_readdir(struct dirstruct *dirh, char *buf, int len)
{
	return dir_read(dirh, buf, len, 0);
}

void _cdecl
kernel_closedir(struct dirstruct *dirh)
{
	dir_close(dirh);
}

/* ---- the module interface's files ---- */

struct file *_cdecl
kernel_open(const char *path, int rwmode, long *err, XATTR *x)
{
	struct file *f;
	long r = file_open(path, rwmode, 0666, &f);

	if (err)
		*err = r;
	if (r == 0 && x)
		(*f->dev->ioctl)(f, FSTAT, x);
	return r ? 0 : f;
}

long _cdecl
kernel_read(struct file *f, void *buf, long n)
{
	return file_read(f, buf, n);
}

long _cdecl
kernel_write(struct file *f, const void *buf, long n)
{
	return file_write(f, buf, n);
}

long _cdecl
kernel_lseek(FILEPTR *f, long where, int whence)
{
	return (*f->dev->lseek)(f, where, whence);
}

void _cdecl
kernel_close(struct file *f)
{
	file_close(f);
}
