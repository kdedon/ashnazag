/*
 * abi.c -- A/UX system calls from a static A/UX program: BSD signals
 * (sigvec, sigstack, sigblock, sigpause, sigpending), alarm, itimers, select,
 * wait3/waitpid, flock and fcntl locks, statfs, truncate, utimes, shm,
 * setreuid, and TIOCPKT on a pty master passed as fd 3 (slave fd 4).
 * With argument "q1": raw disk and /proc transfers into quadrant 1.
 * With argument "sock": only the socket calls, on the loopback network.
 *
 * Freestanding: A/UX numbers and structures, no libc.  One line per
 * check on stdout: "P name" or "F name: detail".
 */

extern long sys15(), sys15w(), sys0();
long errno, sysd1;
char abi_id[8] = "aux-abi";	/* keeps .data non-empty */

#define	A_EXIT		1
#define	A_FORK		2
#define	A_READ		3
#define	A_WRITE		4
#define	A_OPEN		5
#define	A_CLOSE		6
#define	A_UNLINK	10
#define	A_LSEEK		19
#define	A_GETPID	20
#define	A_ALARM		27
#define	A_PAUSE		29
#define	A_KILL		37
#define	A_PIPE		42
#define	A_SHMSYS	52
#define	A_IOCTL		54
#define	A_FCNTL		62
#define	A_WAIT		7
#define	A_SELECT	82
#define	A_SETREUID	89
#define	A_FLOCK		105
#define	A_TRUNCATE	114
#define	A_FTRUNCATE	115
#define	A_STATFS	117
#define	A_FSTATFS	118
#define	A_STAT		125
#define	A_SETCOMPAT	128
#define	A_SIGVEC	129
#define	A_SIGBLOCK	130
#define	A_SIGSETMASK	131
#define	A_SIGPAUSE	132
#define	A_SIGSTACK	133
#define	A_GETITIMER	134
#define	A_SETITIMER	135
#define	A_GETTIMEOFDAY	136
#define	A_UTIMES	145
#define	A_SIGPENDING	149
#define	A_WAITPID	151
#define	A_ACCEPT	70
#define	A_BIND		71
#define	A_CONNECT	72
#define	A_GETPEERNAME	75
#define	A_GETSOCKNAME	76
#define	A_GETSOCKOPT	77
#define	A_LISTEN	78
#define	A_RECV		79
#define	A_RECVFROM	80
#define	A_SEND		83
#define	A_SENDTO	85
#define	A_SETSOCKOPT	90
#define	A_SHUTDOWN	91
#define	A_SOCKET	92
#define	MSG_PEEK	2

#define	O_RDONLY	0
#define	O_RDWR		2
#define	O_CREAT		0x100
#define	O_TRUNC		0x200

#define	SIGKILL		9
#define	SIGALRM		14
#define	SIGUSR1		16
#define	SIGUSR2		17
#define	SIGSTOP		23
#define	MASK(s)		(1L << ((s) - 1))
#define	SV_ONSTACK	1

#define	EPERM		1
#define	EINTR		4
#define	EBADF		9
#define	ECHILD		10
#define	EACCES		13
#define	EISDIR		21
#define	EINVAL		22
#define	ENOTTY		25
#define	EWOULDBLOCK	55

#define	TIOCPKT		0x80047470

static char obuf[160];
static int nfail;

static void
out(s)
	char *s;
{
	char *e = s;

	while (*e)
		e++;
	sys15(A_WRITE, 1, s, e - s);
}

static char *
cat(p, s)
	char *p, *s;
{
	while (*s && p < obuf + sizeof obuf - 16)
		*p++ = *s++;
	return p;
}

static char *
num(p, v)
	char *p;
	long v;
{
	char t[12];
	int n = 0;
	unsigned long u = v < 0 ? -v : v;

	if (v < 0)
		*p++ = '-';
	do
		t[n++] = '0' + u % 10;
	while ((u /= 10) != 0);
	while (n)
		*p++ = t[--n];
	return p;
}

/* "P name" if ok, else "F name: what a b" */
static int
check(name, ok, what, a, b)
	char *name, *what;
	int ok;
	long a, b;
{
	char *p = obuf;

	p = cat(p, ok ? "P " : "F ");
	p = cat(p, name);
	if (!ok) {
		p = cat(p, ": ");
		p = cat(p, what);
		*p++ = ' ';
		p = num(p, a);
		*p++ = ' ';
		p = num(p, b);
		nfail++;
	}
	*p++ = '\n';
	*p = 0;
	out(obuf);
	return ok;
}

static long
now_ms()
{
	long tv[2];

	sys15(A_GETTIMEOFDAY, tv);
	return tv[0] * 1000 + tv[1] / 1000;
}

static void
exit_(n)
	int n;
{
	sys15(A_EXIT, n);
}

static long
waitpid_(pid, st, opt)
	long pid, *st, opt;
{
	long r = sys15(A_WAITPID, pid, 0, opt);

	*st = sysd1;
	return r;
}

/* ---- BSD signals ---- */

static volatile int got, gotsig, alarms;
static volatile long hsp;
static char altstk[8192];

static void
onsig(sig, code, scp)
	int sig, code;
	long *scp;
{
	got++;
	gotsig = sig;
	hsp = (long)&sig;
}

static void
onalrm(sig)
	int sig;
{
	alarms++;
}

static void
t_signals()
{
	long sv[3], osv[3], ss[2], m, old;
	long me = sys15(A_GETPID);

	check("setcompat", sys15(A_SETCOMPAT, 0x407) >= 0, "errno", errno, 0);
	ss[0] = (long)(altstk + sizeof altstk);
	ss[1] = 0;
	check("sigstack", sys15(A_SIGSTACK, ss, 0) == 0, "errno", errno, 0);
	sv[0] = (long)onsig;
	sv[1] = 0;
	sv[2] = SV_ONSTACK;
	osv[0] = -1;
	check("sigvec", sys15(A_SIGVEC, SIGUSR1, sv, osv) == 0 && osv[0] == 0,
	    "errno, old handler", errno, osv[0]);
	sys15(A_KILL, me, SIGUSR1);
	check("sigvec_deliver", got == 1 && gotsig == SIGUSR1, "count, signal", got, gotsig);
	check("sigstack_used", hsp > (long)altstk && hsp < (long)altstk + sizeof altstk,
	    "handler sp, stack", hsp, (long)altstk);
	ss[1] = 7;
	sys15(A_SIGSTACK, 0, ss);
	check("sigstack_off", ss[1] == 0 && ss[0] == (long)(altstk + sizeof altstk),
	    "onstack, sp", ss[1], ss[0]);

	old = sys15(A_SIGBLOCK, MASK(SIGUSR1));
	sys15(A_KILL, me, SIGUSR1);
	check("sigblock", got == 1, "count", got, 0);
	m = 0;
	sys15(A_SIGPENDING, &m);
	check("sigpending", (m & MASK(SIGUSR1)) != 0, "mask", m, 0);
	check("sigpause", sys15(A_SIGPAUSE, 0) == -1 && errno == EINTR && got == 2,
	    "errno, count", errno, got);
	m = sys15(A_SIGSETMASK, old);
	check("sigpause_mask", (m & MASK(SIGUSR1)) != 0, "mask after", m, 0);
}

/*
 * A handler on a small signal stack calls select: the kernel's scratch
 * must not land below that stack.
 */
static struct {
	char	guard[1024];
	char	stk[256];
} small;
static long gaprd, gaptv[2];
static volatile long gapr;

static void
ongap(sig)
	int sig;
{
	gaprd = 1;
	gapr = sys15(A_SELECT, 1, &gaprd, 0, 0, gaptv);
}

static void
t_altgap()
{
	long sv[3], ss[2], i, bad = -1;

	for (i = 0; i < sizeof small.guard; i++)
		small.guard[i] = 0x5a;
	ss[0] = (long)(small.stk + sizeof small.stk);
	ss[1] = 0;
	sys15(A_SIGSTACK, ss, 0);
	sv[0] = (long)ongap;
	sv[1] = 0;
	sv[2] = SV_ONSTACK;
	sys15(A_SIGVEC, SIGUSR2, sv, 0);
	gapr = -2;
	sys15(A_KILL, sys15(A_GETPID), SIGUSR2);
	for (i = 0; i < sizeof small.guard; i++)
		if (small.guard[i] != 0x5a) {
			bad = i;
			break;
		}
	check("select_sigstack", gapr != -2 && bad == -1, "select, first byte changed",
	    gapr, bad);
}

/* ---- itimers ---- */

static void
t_itimer()
{
	long sv[3], v[4], o[4], t0, t;

	sv[0] = (long)onalrm;
	sv[1] = 0;
	sv[2] = 0;
	sys15(A_SIGVEC, SIGALRM, sv, 0);
	v[0] = 0;
	v[1] = 100000;
	v[2] = 0;
	v[3] = 100000;
	t0 = now_ms();
	check("setitimer", sys15(A_SETITIMER, 0, v, 0) == 0, "errno", errno, 0);
	o[0] = o[1] = o[2] = o[3] = -1;
	sys15(A_GETITIMER, 0, o);
	check("getitimer", o[0] == 0 && o[1] == 100000 && o[2] == 0 &&
	    o[3] > 0 && o[3] <= 100000, "interval, value us", o[1], o[3]);
	while (alarms < 3 && now_ms() - t0 < 3000)
		sys15(A_SIGPAUSE, 0);
	t = now_ms() - t0;
	check("itimer_real", alarms >= 3 && t >= 250 && t < 1500, "alarms, ms", alarms, t);
	v[0] = v[1] = v[2] = v[3] = 0;
	check("itimer_stop", sys15(A_SETITIMER, 0, v, o) == 0 && o[1] == 100000,
	    "old interval", o[1], 0);
	sys15(A_GETITIMER, 0, o);
	t = alarms;
	t0 = now_ms();
	while (now_ms() - t0 < 300)
		;
	check("itimer_off", o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0 && alarms == t,
	    "value, alarms after", o[3], alarms - t);
	check("itimer_virtual", sys15(A_SETITIMER, 1, v, 0) == 0, "errno", errno, 0);
}

/* ---- alarm ---- */

/* ms from alarm(1) to SIGALRM */
static long
alarm1()
{
	long old, t0;

	alarms = 0;
	old = sys15(A_SIGBLOCK, MASK(SIGALRM));
	t0 = now_ms();
	sys15(A_ALARM, 1);
	while (!alarms && now_ms() - t0 < 5000)
		sys15(A_SIGPAUSE, old);
	sys15(A_SIGSETMASK, old);
	return now_ms() - t0;
}

/* never early, also once children's itimers hold every timer callout */
static void
t_alarm()
{
	long sv[3], v[4], p[2], pid[100], n, k, t, st;
	char c;
	int full = 0;

	sv[0] = (long)onalrm;
	sv[1] = 0;
	sv[2] = 0;
	sys15(A_SIGVEC, SIGALRM, sv, 0);
	t = alarm1();
	check("alarm_1", alarms == 1 && t >= 1000 && t < 1500, "alarms, ms", alarms, t);
	sys15(A_ALARM, 5);
	t = sys15(A_ALARM, 0);
	check("alarm_left", t == 5, "seconds", t, 0);
	p[0] = sys0(A_PIPE);
	p[1] = sysd1;
	for (n = 0; n < 100 && !full; n++) {
		if ((pid[n] = sys15(A_FORK)) == 0 || sysd1) {
			v[0] = v[1] = v[3] = 0;
			v[2] = 1000;
			c = sys15(A_SETITIMER, 0, v, 0) == 0 ? 'y' : 'n';
			sys15(A_WRITE, p[1], &c, 1);
			for (;;)
				sys15(A_PAUSE);
		}
		if (pid[n] < 0 || sys15(A_READ, p[0], &c, 1) != 1)
			break;
		full = c == 'n';
	}
	check("alarm_slots_full", full, "children", n, 0);
	t = alarm1();
	check("alarm_1_full", alarms == 1 && t >= 1000 && t < 2500, "alarms, ms", alarms, t);
	for (k = 0; k < n; k++)
		if (pid[k] > 0) {
			sys15(A_KILL, pid[k], SIGKILL);
			waitpid_(pid[k], &st, 0);
		}
	sys15(A_CLOSE, p[0]);
	sys15(A_CLOSE, p[1]);
}

/* ---- select ---- */

static void
t_select()
{
	long p[2], rd, wr, tv[2], t0, t, n;

	p[0] = sys0(A_PIPE);
	p[1] = sysd1;
	if (!check("pipe", p[0] >= 0, "errno", errno, 0))
		return;
	rd = MASK(p[0] + 1);
	tv[0] = 0;
	tv[1] = 200000;
	t0 = now_ms();
	n = sys15(A_SELECT, p[0] + 1, &rd, 0, 0, tv);
	t = now_ms() - t0;
	check("select_timeout", n == 0 && rd == 0 && t >= 150 && t < 1000, "n, ms", n, t);
	sys15(A_WRITE, p[1], "x", 1);
	rd = MASK(p[0] + 1);
	tv[0] = 1;
	tv[1] = 0;
	n = sys15(A_SELECT, p[0] + 1, &rd, 0, 0, tv);
	check("select_read", n == 1 && rd == MASK(p[0] + 1), "n, set", n, rd);
	wr = MASK(p[1] + 1);
	tv[0] = tv[1] = 0;
	n = sys15(A_SELECT, p[1] + 1, 0, &wr, 0, tv);
	check("select_write", n == 1 && wr == MASK(p[1] + 1), "n, set", n, wr);
	rd = MASK(20);
	n = sys15(A_SELECT, 20, &rd, 0, 0, tv);
	check("select_ebadf", n == -1 && errno == EBADF, "n, errno", n, errno);
	sys15(A_CLOSE, p[0]);
	sys15(A_CLOSE, p[1]);
}

/* ---- wait3, waitpid ---- */

static void
t_wait()
{
	long pid, r, st;

	if ((pid = sys15(A_FORK)) == 0 || sysd1)
		exit_(7);
	r = waitpid_(pid, &st, 0);
	check("waitpid_exit", r == pid && st == 0x700, "pid ok, status", r == pid, st);
	if ((pid = sys15(A_FORK)) == 0 || sysd1)
		for (;;)
			sys15(A_PAUSE);
	r = waitpid_(pid, &st, 1);
	check("waitpid_wnohang", r == 0, "result", r, 0);
	sys15(A_KILL, pid, SIGSTOP);
	r = waitpid_(pid, &st, 2);
	check("waitpid_stopped", r == pid && st == (SIGSTOP << 8 | 0x7f), "pid ok, status",
	    r == pid, st);
	sys15(A_KILL, pid, SIGKILL);
	r = sys15w(A_WAIT, 0, 0);
	check("wait3_killed", r == pid && sysd1 == SIGKILL, "pid ok, status", r == pid, sysd1);
	r = waitpid_(30000L, &st, 0);
	check("waitpid_30000", r == -1 && errno == ECHILD, "result, errno", r, errno);
	r = waitpid_(-1L, &st, 4);
	check("waitpid_badopt", r == -1 && errno == EINVAL, "result, errno", r, errno);
	r = sys15w(A_WAIT, 1, 0);
	check("wait3_nochild", r == -1 && errno == ECHILD, "result, errno", r, errno);
}

/* ---- flock, fcntl locks ---- */

#define	LK	"/tmp/abi.lk"

/* in a child: 0 if fn succeeds */
static long
child(fn, arg)
	long (*fn)(), arg;
{
	long pid, st;

	if ((pid = sys15(A_FORK)) == 0 || sysd1)
		exit_((int)(*fn)(arg));
	waitpid_(pid, &st, 0);
	return st >> 8;
}

static long
c_flock_nb(op)
	long op;
{
	long fd = sys15(A_OPEN, LK, O_RDONLY);

	if (sys15(A_FLOCK, fd, op) == 0)
		return 0;
	return errno == EWOULDBLOCK ? 55 : 1;
}

static long
c_getlk(parent)
	long parent;
{
	char b[16];
	long fd = sys15(A_OPEN, LK, O_RDONLY);

	b[0] = 0;
	b[1] = 1;			/* F_RDLCK */
	*(short *)(b + 2) = 0;
	*(long *)(b + 4) = 0;
	*(long *)(b + 8) = 0;
	if (sys15(A_FCNTL, fd, 5, b) != 0)
		return 1;
	if (*(short *)b != 2 || *(long *)(b + 12) != parent)
		return 2;
	b[1] = 1;
	if (sys15(A_FCNTL, fd, 6, b) != -1 || errno != EACCES)
		return 3;
	return 0;
}

static void
t_locks()
{
	char b[16];
	long fd, r;

	fd = sys15(A_OPEN, LK, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (!check("lock_open", fd >= 0, "errno", errno, 0))
		return;
	check("flock_ex", sys15(A_FLOCK, fd, 2) == 0, "errno", errno, 0);
	r = child(c_flock_nb, 2 | 4);
	check("flock_conflict", r == 55, "child", r, 0);
	check("flock_un", sys15(A_FLOCK, fd, 8) == 0, "errno", errno, 0);
	r = child(c_flock_nb, 2 | 4);
	check("flock_ex_rdonly", r == 0, "child", r, 0);
	b[0] = 0;
	b[1] = 2;			/* F_WRLCK */
	*(short *)(b + 2) = 0;
	*(long *)(b + 4) = 0;
	*(long *)(b + 8) = 0;
	check("fcntl_setlk", sys15(A_FCNTL, fd, 6, b) == 0, "errno", errno, 0);
	r = child(c_getlk, sys15(A_GETPID));
	check("fcntl_getlk", r == 0, "child", r, 0);
	sys15(A_CLOSE, fd);
	sys15(A_UNLINK, LK);
}

/* ---- statfs, truncate, utimes ---- */

#define	TR	"/tmp/abi.tr"

static void
t_files()
{
	long sf[16], sf2[16], st[15], fd, tv[4];
	char b[100];

	sf[1] = sf[2] = -1;
	check("statfs", sys15(A_STATFS, "/", sf) == 0 && sf[0] == 0 && sf[1] >= 512 &&
	    sf[2] > 0 && sf[3] <= sf[2] && sf[4] <= sf[2] && sf[5] > 0,
	    "bsize, blocks", sf[1], sf[2]);
	fd = sys15(A_OPEN, TR, O_RDWR | O_CREAT | O_TRUNC, 0644);
	check("fstatfs", sys15(A_FSTATFS, fd, sf2) == 0 && sf2[1] == sf[1] && sf2[2] == sf[2] &&
	    sf2[7] == sf[7], "bsize, fsid", sf2[1], sf2[7]);
	sys15(A_WRITE, fd, b, 100);
	check("truncate", sys15(A_TRUNCATE, TR, 10) == 0 && sys15(A_LSEEK, fd, 0, 2) == 10,
	    "errno", errno, 0);
	check("ftruncate", sys15(A_FTRUNCATE, fd, 3) == 0 && sys15(A_LSEEK, fd, 0, 2) == 3,
	    "errno", errno, 0);
	check("truncate_dir", sys15(A_TRUNCATE, "/tmp", 0) == -1 && errno == EISDIR,
	    "errno", errno, 0);
	sys15(A_CLOSE, fd);
	fd = sys15(A_OPEN, TR, O_RDONLY);
	check("ftruncate_rdonly", sys15(A_FTRUNCATE, fd, 0) == -1 && errno == EINVAL,
	    "errno", errno, 0);
	sys15(A_CLOSE, fd);
	tv[0] = 1000000;
	tv[1] = 5;
	tv[2] = 2000000;
	tv[3] = 7;
	check("utimes", sys15(A_UTIMES, TR, tv) == 0 && sys15(A_STAT, TR, st) == 0 &&
	    *(long *)((char *)st + 0x12) == 1000000 && *(long *)((char *)st + 0x1a) == 2000000,
	    "errno, mtime", errno, *(long *)((char *)st + 0x1a));
	sys15(A_UNLINK, TR);
	check("setreuid", sys15(A_SETREUID, -1, -1) == 0, "errno", errno, 0);
}

/* ---- setreuid(r, r) drops for good; flock needs access ---- */

#define	ID	"/tmp/abi.id"

/* as root: ruid 1 with euid 0 (a set-uid program), then drop */
static long
c_ids(fd)
	long fd;
{
	if (sys15(A_SETREUID, 1, -1) != 0)
		return 50;
	if (sys15(A_SETREUID, 1, 1) != 0)
		return 1;
	if (sys15(A_FLOCK, fd, 2 | 4) != -1 || errno != EBADF)
		return 3;
	if (sys15(A_FLOCK, fd, 1 | 4) != 0)
		return 4;
	if (sys15(A_SETREUID, -1, 0) != -1 || errno != EPERM)
		return 2;
	return 0;
}

static void
t_ids()
{
	long fd, r;

	fd = sys15(A_OPEN, ID, O_RDWR | O_CREAT | O_TRUNC, 0644);
	sys15(A_CLOSE, fd);
	fd = sys15(A_OPEN, ID, O_RDONLY);
	r = child(c_ids, fd);
	if (r == 50)
		out("S setreuid_drop: not root\n");
	else {
		check("setreuid_drop", r != 1 && r != 2, "child", r, 0);
		check("flock_noaccess", r != 1 && r != 3 && r != 4, "child", r, 0);
	}
	sys15(A_CLOSE, fd);
	sys15(A_UNLINK, ID);
}

/* ---- shm ---- */

static void
t_shm()
{
	char ds[44];
	long id, a, pid, st;

	id = sys0(A_SHMSYS, 3, 0, 8192, 01000 | 0600);
	if (!check("shmget", id >= 0, "errno", errno, 0))
		return;
	a = sys0(A_SHMSYS, 0, id, 0, 0);
	if (check("shmat", a != -1, "errno", errno, 0)) {
		((long *)a)[0] = 0x12345678;
		if ((pid = sys15(A_FORK)) == 0 || sysd1) {
			((long *)a)[1] = ((long *)a)[0] + 1;
			exit_(0);
		}
		waitpid_(pid, &st, 0);
		check("shm_shared", ((long *)a)[1] == 0x12345679, "value", ((long *)a)[1], 0);
	}
	check("shmctl_stat", sys0(A_SHMSYS, 1, id, 2, ds) == 0 && *(long *)(ds + 16) == 8192 &&
	    (*(unsigned short *)(ds + 8) & 0777) == 0600 &&
	    *(unsigned short *)(ds + 26) == sys15(A_GETPID),
	    "segsz, mode", *(long *)(ds + 16), *(unsigned short *)(ds + 8));
	*(unsigned short *)(ds + 8) = 0640;
	check("shmctl_set", sys0(A_SHMSYS, 1, id, 1, ds) == 0 &&
	    sys0(A_SHMSYS, 1, id, 2, ds) == 0 && (*(unsigned short *)(ds + 8) & 0777) == 0640,
	    "mode", *(unsigned short *)(ds + 8), errno);
	if (a != -1)
		check("shmdt", sys0(A_SHMSYS, 2, a) == 0, "errno", errno, 0);
	check("shmctl_rmid", sys0(A_SHMSYS, 1, id, 0, 0) == 0 &&
	    sys0(A_SHMSYS, 1, id, 2, ds) == -1 && errno == EINVAL, "errno", errno, 0);
}

/* ---- quadrant 1: raw disk and /proc into A/UX-only addresses ---- */

#define	Q1	0x48000000L
#define	Q1B	(Q1 + 0x100000)

static char q1ref[2048];

/* first index where a and b differ over n bytes, -1 if none */
static long
diff(a, b, n)
	char *a, *b;
	long n;
{
	long i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return i;
	return -1;
}

static void
t_q1()
{
	char path[16], pb[32], zero[32], *q = (char *)(Q1 + 4096 - 1024), *p;
	long id, id2, a, fd, n, m, pid = sys15(A_GETPID), i;

	id = sys0(A_SHMSYS, 3, 0, 8192, 01000 | 0600);
	id2 = sys0(A_SHMSYS, 3, 0, 8192, 01000 | 0600);
	a = sys0(A_SHMSYS, 0, id, Q1, 0);
	if (!check("q1_shmat", a == Q1 && sys0(A_SHMSYS, 0, id2, Q1B, 0) == Q1B,
	    "address, errno", a, errno))
		goto out;
	fd = sys15(A_OPEN, "/dev/rdsk/rd0", O_RDONLY);
	n = fd < 0 ? -1 : sys15(A_READ, fd, q, 2048L);
	sys15(A_CLOSE, fd);
	fd = sys15(A_OPEN, "/dev/dsk/rd0", O_RDONLY);
	m = fd < 0 ? -1 : sys15(A_READ, fd, q1ref, 2048L);
	sys15(A_CLOSE, fd);
	check("q1_raw_read", n == 2048 && m == 2048 && diff(q, q1ref, 2048L) == -1,
	    "raw, first difference", n, diff(q, q1ref, 2048L));

	for (p = path, i = 0; i < 6; i++)
		*p++ = "/proc/"[i];
	for (i = 10000; i; i /= 10)
		*p++ = '0' + pid / i % 10;
	*p = 0;
	for (i = 0; i < 32; i++)
		zero[i] = 0;
	fd = sys15(A_OPEN, path, O_RDONLY);
	n = -1;
	if (fd >= 0 && sys15(A_LSEEK, fd, Q1 + 4096 - 16, 0) != -1)
		n = sys15(A_READ, fd, pb, 32L);
	check("q1_proc_read", n == 32 && diff(pb, q + 1024 - 16, 32L) == -1,
	    "n, first difference", n, diff(pb, q + 1024 - 16, 32L));
	n = -1;
	for (i = 0; i < 32; i++)
		pb[i] = 1;
	if (fd >= 0 && sys15(A_LSEEK, fd, Q1B + 4096 - 16, 0) != -1)
		n = sys15(A_READ, fd, pb, 32L);
	check("q1_proc_fault", n == 32 && diff(pb, zero, 32L) == -1,
	    "n, first nonzero", n, diff(pb, zero, 32L));
	sys15(A_CLOSE, fd);
	sys0(A_SHMSYS, 2, Q1);
	sys0(A_SHMSYS, 2, Q1B);
out:
	sys0(A_SHMSYS, 1, id, 0, 0);
	sys0(A_SHMSYS, 1, id2, 0, 0);
}

/* ---- TIOCPKT: fd 3 pty master, fd 4 its slave ---- */

static long
rdwait(fd, b, n)
	long fd, n;
	char *b;
{
	long rd = MASK(fd + 1), tv[2];

	tv[0] = 3;
	tv[1] = 0;
	if (sys15(A_SELECT, fd + 1, &rd, 0, 0, tv) != 1)
		return -2;
	return sys15(A_READ, fd, b, n);
}

static void
t_tiocpkt()
{
	char b[64];
	long on = 1, n;

	if (sys15(A_FCNTL, 3, 1) == -1 || sys15(A_FCNTL, 4, 1) == -1) {
		out("S tiocpkt: no pty on fds 3 and 4\n");
		return;
	}
	check("tiocpkt_on", sys15(A_IOCTL, 3, TIOCPKT, &on) == 0, "errno", errno, 0);
	sys15(A_WRITE, 4, "hi", 2);
	n = rdwait(3L, b, 64L);
	check("tiocpkt_data", n == 3 && b[0] == 0 && b[1] == 'h' && b[2] == 'i',
	    "n, first byte", n, b[0]);
	on = 0;
	check("tiocpkt_off", sys15(A_IOCTL, 3, TIOCPKT, &on) == 0, "errno", errno, 0);
	sys15(A_WRITE, 4, "yo", 2);
	n = rdwait(3L, b, 64L);
	check("tiocpkt_plain", n == 2 && b[0] == 'y', "n, first byte", n, b[0]);
	on = 1;
	check("tiocpkt_notpty", sys15(A_IOCTL, 4, TIOCPKT, &on) == -1 && errno == ENOTTY,
	    "errno", errno, 0);
}

/* ---- sockets, A/UX numbering: STREAM 1, DGRAM 2 ---- */

#define	EINPROGRESS	56
#define	ENETDOWN	70
#define	ECONNREFUSED	81
#define	SIGIO		31
#define	FIONBIO		0x8004667e
#define	FIOASYNC	0x8004667d
#define	FIONREAD	0x4004667f
#define	SIOCSPGRP	0x80047308
#define	SIOCGIFCONF	0xc0086914
#define	SIOCGIFFLAGS	0xc0206911
#define	SIOCGIFNETMASK	0xc0206917

static volatile int sigios;

static void
onsigio(sig)
	int sig;
{
	sigios++;
}

/* sockaddr_in 127.0.0.1:port */
static void
lo(a, port)
	char *a;
	int port;
{
	int i;

	for (i = 0; i < 16; i++)
		a[i] = 0;
	a[1] = 2;
	a[2] = port >> 8;
	a[3] = port;
	a[4] = 127;
	a[7] = 1;
}

static int
port(a)
	char *a;
{
	return (a[2] & 0xff) << 8 | (a[3] & 0xff);
}

static long
rdready(fd, ms)
	long fd, ms;
{
	long rd = MASK(fd + 1), tv[2];

	tv[0] = ms / 1000;
	tv[1] = ms % 1000 * 1000;
	return sys15(A_SELECT, fd + 1, &rd, 0, 0, tv) == 1 && rd == MASK(fd + 1);
}

static void
t_sock()
{
	char la[16], ca[16], pa[16], b[64], ifb[320];
	long l, c, a, u1, u2, n, v, len, sv[3], ifc[2], wr, tv[2], t0, pid, st;
	int i;

	sv[0] = (long)onalrm;
	sv[1] = 0;
	sv[2] = 0;
	sys15(A_SIGVEC, SIGALRM, sv, 0);
	sys15(A_ALARM, 20);
	l = sys15(A_SOCKET, 2, 1, 0);
	if (l < 0 && errno == ENETDOWN) {
		out("S sock: no network\n");
		return;
	}
	if (!check("sock_socket", l >= 0, "errno", errno, 0))
		return;
	len = 4;
	check("sock_type", sys15(A_GETSOCKOPT, l, 0xffff, 0x1008, &v, &len) == 0 &&
	    v == 1 && len == 4, "errno, type", errno, v);
	v = 1;
	check("sock_reuseaddr", sys15(A_SETSOCKOPT, l, 0xffff, 4, &v, 4) == 0, "errno", errno, 0);
	lo(la, 0);
	check("sock_bind", sys15(A_BIND, l, la, 16) == 0, "errno", errno, 0);
	len = 16;
	check("sock_getsockname", sys15(A_GETSOCKNAME, l, la, &len) == 0 && len == 16 &&
	    la[1] == 2 && port(la) != 0, "errno, port", errno, port(la));
	n = sys15(A_LISTEN, l, 5);
	check("sock_listen", n == 0, "n, errno", n, errno);

	c = sys15(A_SOCKET, 2, 1, 0);
	v = 1;
	sys15(A_IOCTL, c, FIONBIO, &v);
	n = sys15(A_CONNECT, c, la, 16);
	check("sock_connect_nb", n == 0 || errno == EINPROGRESS, "n, errno", n, errno);
	len = 16;
	a = rdready(l, 2000L) ? sys15(A_ACCEPT, l, pa, &len) : -2;
	check("sock_accept", a >= 0 && len == 16 && pa[4] == 127, "errno, len", errno, len);
	wr = MASK(c + 1);
	tv[0] = 2;
	tv[1] = 0;
	check("sock_connect_writable", sys15(A_SELECT, c + 1, 0, &wr, 0, tv) == 1,
	    "errno", errno, 0);
	len = 16;
	check("sock_getpeername", sys15(A_GETPEERNAME, c, ca, &len) == 0 &&
	    port(ca) == port(la), "errno, port", errno, port(ca));
	v = 0;
	sys15(A_IOCTL, c, FIONBIO, &v);
	if (a < 0)
		return;

	check("sock_send", sys15(A_SEND, c, "hello", 5, 0) == 5, "errno", errno, 0);
	check("sock_select", rdready(a, 2000L), "errno", errno, 0);
	v = -1;
	check("sock_fionread", sys15(A_IOCTL, a, FIONREAD, &v) == 0 && v == 5,
	    "errno, n", errno, v);
	n = sys15(A_RECV, a, b, 64, 0);
	check("sock_recv", n == 5 && b[0] == 'h' && b[4] == 'o', "n, errno", n, errno);
	check("sock_write_read", sys15(A_WRITE, a, "abc", 3) == 3 &&
	    rdready(c, 2000L) && sys15(A_READ, c, b, 64) == 3 && b[2] == 'c',
	    "errno", errno, 0);

	v = 1;
	sys15(A_IOCTL, a, FIONBIO, &v);
	n = sys15(A_RECV, a, b, 64, 0);
	check("sock_ewouldblock", n == -1 && errno == EWOULDBLOCK, "n, errno", n, errno);
	v = 0;
	sys15(A_IOCTL, a, FIONBIO, &v);

	sv[0] = (long)onsigio;
	sv[1] = 0;
	sv[2] = 0;
	sys15(A_SIGVEC, SIGIO, sv, 0);
	v = sys15(A_GETPID);
	check("sock_siocspgrp", sys15(A_IOCTL, a, SIOCSPGRP, &v) == 0, "errno", errno, 0);
	v = 1;
	check("sock_fioasync", sys15(A_IOCTL, a, FIOASYNC, &v) == 0, "errno", errno, 0);
	sys15(A_SEND, c, "x", 1, 0);
	for (t0 = now_ms(); sigios == 0 && now_ms() - t0 < 2000; )
		rdready(a, 100L);
	check("sock_sigio", sigios > 0, "count", sigios, 0);
	sys15(A_RECV, a, b, 64, 0);

	check("sock_shutdown", sys15(A_SHUTDOWN, c, 1) == 0, "errno", errno, 0);
	n = rdready(a, 2000L) ? sys15(A_RECV, a, b, 64, 0) : -2;
	check("sock_eof", n == 0, "n, errno", n, errno);
	n = sys15(A_RECV, a, b, 64, 0);
	check("sock_eof_sticky", n == 0 && rdready(a, 100L), "n, errno", n, errno);
	n = sys15(A_SEND, a, "pk", 2, 0) == 2 && rdready(c, 2000L) ?
	    sys15(A_RECV, c, b, 64, MSG_PEEK) : -2;
	check("sock_peek_shut", n == 2 && b[0] == 'p' && sys15(A_RECV, c, b, 64, 0) == 2,
	    "n, errno", n, errno);
	sys15(A_SHUTDOWN, a, 1);
	n = rdready(c, 2000L) ? sys15(A_RECV, c, b, 64, MSG_PEEK) : -2;
	check("sock_peek_eof", n == 0 && sys15(A_RECV, c, b, 64, 0) == 0, "n, errno", n, errno);
	sys15(A_CLOSE, a);
	sys15(A_CLOSE, c);
	/* closed sockets give back their pending-connect state */
	for (i = 0, n = 0; i < 24 && n == 0; i++) {
		c = sys15(A_SOCKET, 2, 1, 0);
		v = 1;
		sys15(A_IOCTL, c, FIONBIO, &v);
		if (sys15(A_CONNECT, c, la, 16) != 0 && errno != EINPROGRESS)
			n = errno;
		sys15(A_CLOSE, c);
	}
	check("sock_connect_reclaim", n == 0, "i, errno", i, n);
	sys15(A_CLOSE, l);

	if ((pid = sys15(A_FORK)) == 0 || sysd1) {
		sys15(A_SETREUID, 100, 100);
		c = sys15(A_SOCKET, 2, 1, 0);
		lo(ca, 23);
		n = sys15(A_BIND, c, ca, 16);
		exit_(n == -1 && errno == EACCES ? 0 : 1);
	}
	waitpid_(pid, &st, 0L);
	check("sock_resvport", st == 0, "status", st, 0);

	sys15(A_ALARM, 10);
	c = sys15(A_SOCKET, 2, 1, 0);
	lo(ca, 1);
	n = sys15(A_CONNECT, c, ca, 16);
	check("sock_refused", n == -1 && errno == ECONNREFUSED, "n, errno", n, errno);
	sys15(A_CLOSE, c);
	c = sys15(A_SOCKET, 2, 1, 0);
	ifc[0] = sizeof ifb;
	ifc[1] = (long)ifb;
	n = sys15(A_IOCTL, c, SIOCGIFCONF, ifc);
	check("sock_ifconf", n == 0 && ifc[0] >= 32 && ifc[0] % 32 == 0, "errno, len",
	    errno, ifc[0]);
	check("sock_ifconf_lolast", n == 0 && (ifc[0] == 32 || (ifb[20] & 0xff) != 127),
	    "errno, first byte", errno, ifb[20]);
	if (n == 0 && ifc[0] >= 32) {
		check("sock_ifflags", sys15(A_IOCTL, c, SIOCGIFFLAGS, ifb) == 0 &&
		    (ifb[17] & 1), "errno, flags", errno, ifb[17]);
		check("sock_ifnetmask", sys15(A_IOCTL, c, SIOCGIFNETMASK, ifb) == 0 &&
		    (ifb[20] & 0xff) == 255, "errno, first byte", errno, ifb[20]);
	}
	sys15(A_CLOSE, c);

	u1 = sys15(A_SOCKET, 2, 2, 0);
	u2 = sys15(A_SOCKET, 2, 2, 0);
	lo(la, 0);
	len = 16;
	check("sock_udp", u1 >= 0 && u2 >= 0 && sys15(A_BIND, u1, la, 16) == 0 &&
	    sys15(A_GETSOCKNAME, u1, la, &len) == 0 && port(la) != 0, "errno", errno, 0);
	check("sock_sendto", sys0(A_SENDTO, u2, "dgram", 5L, 0L, la, 16L) == 5,
	    "errno", errno, 0);
	len = 16;
	pa[1] = 0;
	n = rdready(u1, 2000L) ? sys0(A_RECVFROM, u1, b, 64L, 0L, pa, &len) : -2;
	check("sock_recvfrom", n == 5 && b[0] == 'd' && len == 16 && pa[1] == 2 &&
	    pa[4] == 127, "n, errno", n, errno);
	sys15(A_CLOSE, u1);
	sys15(A_CLOSE, u2);
	sys15(A_ALARM, 0);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	if (argc > 1 && argv[1][0] == 's') {
		sys15(A_SETCOMPAT, 0x407);
		t_sock();
		out("done\n");
		return nfail != 0;
	}
	t_signals();
	t_altgap();
	t_itimer();
	t_alarm();
	t_select();
	t_wait();
	t_locks();
	t_files();
	t_ids();
	t_shm();
	t_tiocpkt();
	if (argc > 1 && argv[1][0] == 'q' && argv[1][1] == '1')
		t_q1();
	else
		out("S q1: kernel without the vtop override\n");
	out("done\n");
	return nfail != 0;
}
