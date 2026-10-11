/*
 * host.c -- MiNT command-line programs as Unix processes.  The
 * environment's spawner runs each under mintrun on a pseudo-terminal
 * and passes its master back; a proxy process here stands for it, with
 * the caller's handles: it copies between them and the terminal and
 * ends with the program, taking its exit status from the spawner.
 *
 * Spawner protocol, on a STREAMS pipe:
 *	request	long n, then n bytes: dir\0 prog\0 arg\0 ... \0
 *	reply	{ 1, pid or -errno, 0 }, then the master (I_SENDFD)
 *		{ 2, pid, wait status } when a program ends
 */
#include "xamint.h"

#define	PSPAWN	(*(long *)0xfa1080)
#define	NEXIT	16

static struct { long pid, st; } exits[NEXIT];
static short busy;

/* a MiNTLib program, run on the host: a .TTP or .TOS marked MiNT in its header */
int
host_prg(const char *hp, const char *name)
{
	unsigned char h[32];
	long fd;
	int n = strlen(name);

	if (PSPAWN < 0 || n < 4 || (stricmp(name + n - 4, ".ttp") && stricmp(name + n - 4, ".tos")))
		return 0;
	if ((fd = hc(H_OPEN, 3, (long)hp, 0L, 0L)) < 0)
		return 0;
	n = hc(H_READ, 3, fd, (long)h, (long)sizeof h) == sizeof h;
	hc(H_CLOSE, 1, fd);
	return n && h[0] == 0x60 && h[1] == 0x1a && memcmp(h + 18, "MiNT", 4) == 0;
}

/* the spawner's records: exits kept for their proxies; a reply returned */
static int
host_read(long *rec)
{
	long pfd[2], r[3];
	int i;

	for (;;) {
		pfd[0] = PSPAWN;
		pfd[1] = 1L << 16;
		if (hc(H_POLL, 3, (long)pfd, 1L, 0L) <= 0 || !(pfd[1] & 0xffff))
			return 0;
		if (hc(H_READ, 3, PSPAWN, (long)r, 12L) != 12)
			return 0;
		if (r[0] == 1) {
			memcpy(rec, r, sizeof r);
			return 1;
		}
		for (i = 0; i < NEXIT && exits[i].pid; i++)
			;
		if (i == NEXIT)
			i = 0;
		exits[i].pid = r[1];
		exits[i].st = r[2];
	}
}

void
host_poll(void)
{
	long r[3];

	while (host_read(r))
		xlog("spawner: reply %ld out of turn", r[1]);
}

/* MiNT's signal as the host's; 0 for none */
int
host_sig(int sig)
{
	static const unsigned char h[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 21, 23, 24, 25, 18,
		26, 27, 22, 30, 31, 28, 29, 20, 16, 17, 19
	};

	return sig > 0 && sig < (int)sizeof h ? h[sig] : 0;
}

/* a host wait status as a MiNT exit code */
static int
mstatus(long st)
{
	static const unsigned char m[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 29, 30, 20, 31, 28,
		16, 23, 17, 18, 19, 21, 22, 26, 27, 24, 25
	};
	int s = st & 0x7f;

	if (s == 0)
		return (st >> 8) & 0xff;
	return (s < (int)sizeof m ? m[s] : SIGTERM) << 8;
}

/*
 * The proxy: terminal output to its handle 1, handle 0 to the terminal,
 * raw, so the host's line discipline edits and echoes.
 */
static void
proxy(void *arg)
{
	struct kp *k = KP(curproc);
	struct file *in = fd_get(k, 0), *out = fd_get(k, 1);
	struct tty *t = tty_of(in);
	short flags = t ? t->sg.sg_flags : 0;
	char buf[512];
	long n, end = 0;
	int i, moved;

	(void)arg;
	if (t)
		t->sg.sg_flags = (flags | T_RAW) & ~(T_ECHO | T_CRMOD);
	for (;;) {
		moved = 0;
		n = hc(H_READ, 3, k->hostfd, (long)buf, (long)sizeof buf);
		if (n > 0) {
			if (out)
				file_write(out, buf, n);
			moved = 1;
		} else if (n == 0 || n != -H_EAGAIN)
			break;
		if (in && file_select(in, 0)) {
			n = file_read(in, buf, sizeof buf);
			if (n > 0) {
				hc(H_WRITE, 3, k->hostfd, (long)buf, n);
				moved = 1;
			} else
				in = 0;
		}
		if (!moved)
			nap(20);
	}
	if (t)
		t->sg.sg_flags = flags;
	hc(H_CLOSE, 1, k->hostfd);
	/* its exit follows the hangup */
	for (end = now_ms() + 3000; now_ms() < end; nap(50)) {
		host_poll();
		for (i = 0; i < NEXIT; i++)
			if (exits[i].pid == k->hostpid) {
				exits[i].pid = 0;
				k->hoststat = exits[i].st;
				goto done;
			}
	}
done:
	xlog("Unix process %ld ended, status %lx", k->hostpid, k->hoststat);
	k->hostpid = 0;
	kthread_exit(k->hoststat < 0 ? 0 : mstatus(k->hoststat));
}

/* append s to the request; 0 when full */
static int
put(char *req, long *n, const char *s, int len)
{
	if (*n + len + 1 > 2048)
		return 0;
	memcpy(req + *n, s, len);
	req[*n + len] = 0;
	*n += len + 1;
	return 1;
}

/*
 * Run host program hp with TOS command tail (length byte first) as a
 * Unix process; a proxy child of the caller stands for it.  Its
 * directory: dir, else the caller's.
 */
long
host_spawn(const char *hp, const char *tail, const char *name, const char *dir, struct kp **res)
{
	struct kp *me = KP(curproc), *k;
	static char req[2048 + 4];
	char d[PLEN + 4], cwd[PLEN];
	long n = 0, rec[3], rfd[8], t0;
	const char *s, *e;
	int len, drv;

	if (dir && dir[0] && dir[1] == ':')
		strncpy(d, dir, sizeof d - 1);
	else {
		drv = me->p.p_cwd->curdrv;
		d[0] = 'A' + drv;
		d[1] = ':';
		d[2] = '\\';
		strncpy(d + 3, dir ? dir : me->dirs[drv] ? me->dirs[drv] : "", sizeof d - 4);
	}
	d[sizeof d - 1] = 0;
	if (host_path(d, cwd, 1) < 0)
		strcpy(cwd, "/");
	put(req + 4, &n, cwd, strlen(cwd));
	put(req + 4, &n, hp, strlen(hp));
	if (tail) {
		len = (unsigned char)tail[0] < 127 ? (unsigned char)tail[0] : 126;
		for (s = tail + 1; s < tail + 1 + len && *s; s = e) {
			while (*s == ' ' && s < tail + 1 + len)
				s++;
			for (e = s; e < tail + 1 + len && *e && *e != ' ' && *e != '\r'; e++)
				;
			if (e > s && !put(req + 4, &n, s, e - s))
				return ENOMEM;
			if (*e == '\r')
				break;
		}
	}
	req[4 + n++] = 0;
	*(long *)req = n;
	while (busy)
		nap(20);
	busy = 1;
	host_poll();
	if (hc(H_WRITE, 3, PSPAWN, (long)req, n + 4) != n + 4) {
		busy = 0;
		return EIO;
	}
	for (t0 = now_ms(); !host_read(rec); nap(20))
		if (now_ms() - t0 > 10000) {
			busy = 0;
			xlog("spawner: no reply");
			return EIO;
		}
	if (rec[1] < 0 || hc(H_IOCTL, 3, PSPAWN, (long)I_RECVFD, (long)rfd) < 0) {
		busy = 0;
		xlog("spawner: %s: %ld", hp, rec[1]);
		return rec[1] < 0 ? merr(rec[1]) : EIO;
	}
	busy = 0;
	hc(H_FCNTL, 3, rfd[0], 4L, (long)HO_NONBLOCK);
	k = proc_new(me, name, 1, 0);
	if (!k) {
		hc(H_CLOSE, 1, rfd[0]);
		hc(H_KILL, 2, rec[1], 9L);
		return ENOMEM;
	}
	k->hostpid = rec[1];
	k->hostfd = rfd[0];
	k->hoststat = -1;
	k->kfunc = proxy;
	strncpy(k->p.fname, hp, sizeof k->p.fname - 1);
	ready(k);
	*res = k;
	return 0;
}
