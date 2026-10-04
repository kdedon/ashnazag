/*
 * macjoin.c -- two Unix processes in one Mac session, as startmac and a
 * COFF Mac application (CommandShell) share it: the second waits for
 * the session (UI_SYNC), attaches the Mac memory 'tLOW' at 0, joins
 * (UI_ATTACHLAYER) and the two hand the Mac over with UI_SWITCH.
 *
 *	macjoin j	joiner, started before the session exists
 *	macjoin c	the session (creator); forks the other joiners
 *	macjoin k|a|w	joiners the creator kills waiting, kills running,
 *			and leaves behind when it is killed itself
 *	macjoin u	another user, its own memory at 0: may not join
 *	macjoin t	exit 0 if no session is left
 *
 * Freestanding; output as macabi's: "P name", "F name: ...", "I name v";
 * the creator ends with "done", then "K pid" with the joiner that must
 * go when the creator is killed.
 */

extern long sys15(), sys0();
long errno, sysd1, la_w0, la_w1, la_sp, la_count, be_fv, be_ea, be_ssw;

#define	A_EXIT		1
#define	A_FORK		2
#define	A_WRITE		4
#define	A_OPEN		5
#define	A_GETPID	20
#define	A_SETUID	23
#define	A_KILL		37
#define	A_SHMSYS	52
#define	A_IOCTL		54
#define	A_EXECE		59
#define	A_SETCOMPAT	128
#define	A_SIGVEC	129
#define	A_GETTIMEOFDAY	136
#define	A_WAITPID	151

#define	O_RDWR		2
#define	SIGKILL		9
#define	SIGIOT		6
#define	SIGUSR1		16
#define	EPERM		1
#define	EINVAL		22

#define	UI_SET		0x20005101
#define	UI_ROM		0x20005105
#define	UI_MAP		0x20005107
#define	UI_DELAY	0xc004510c
#define	UI_SETEVENTMASK	0x80025114
#define	UI_CREATELAYER	0x20005115
#define	UI_PHYS_SCREENS	0xc0305122
#define	UI_TIMER	0xc0045123
#define	UI_ATTACHLAYER	0x800a5125
#define	UI_SWITCH	0x80045126
#define	UI_SLEEP	0x20005127
#define	UI_SHMID	0x8004512e
#define	UI_SYNC		0x80045132
#define	UI_TEST		0x80045134

#define	TLOW		0x744c4f57
#define	MEMSIZE		0x1800000	/* past the default 16 MB VM limit */
#define	ROM		0x40800000
#define	SH		((volatile long *)0x10000)	/* shared words */
#define	S_JPID		0	/* the first joiner, once attached */
#define	S_OWNER		1	/* who is in its section */
#define	S_BAD		2	/* sections that saw the other run */
#define	S_GOTUSR	3	/* the creator's SIGUSR1 handler ran */
#define	S_ROUND		5
#define	S_JTICKS	6	/* ticks the joiner took in its sections */

static char obuf[160];
static int ufd;
static long me;
static volatile long ticks;

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
	while (*s && p < obuf + sizeof obuf - 30)
		*p++ = *s++;
	return p;
}

static char *
num(p, v)
	char *p;
	unsigned long v;
{
	char b[12];
	int n = 0;

	do
		b[n++] = '0' + v % 10;
	while (v /= 10);
	while (n)
		*p++ = b[--n];
	return p;
}

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
	}
	*p++ = '\n';
	*p = 0;
	out(obuf);
	return ok;
}

static void
line(tag, v)
	char *tag;
	long v;
{
	char *p = cat(obuf, tag);

	p = num(p, v);
	*p++ = '\n';
	*p = 0;
	out(obuf);
}

static long
ioc(cmd, arg)
	long cmd, arg;
{
	return sys0(A_IOCTL, ufd, cmd, arg);
}

static long
now_ms()
{
	long tv[2];

	sys15(A_GETTIMEOFDAY, tv);
	return tv[0] * 1000 + tv[1] / 1000;
}

static void
spin(ms)
	long ms;
{
	long t = now_ms();

	while (now_ms() - t < ms)
		;
}

/* a UI_SWITCH that a stop or kill cut short is asked again, as Patch does */
static long
swto(pid)
	long pid;
{
	long r;

	while ((r = ioc(UI_SWITCH, &pid)) == -1 && errno == 4)
		pid = 0, r = ioc(UI_SLEEP, 0);
	return r;
}

/* ~100 ms holding the Mac: the other task must not run meanwhile */
static void
section()
{
	long t = now_ms(), tk = ticks;

	SH[S_OWNER] = me;
	while (now_ms() - t < 100)
		if (SH[S_OWNER] != me) {
			SH[S_BAD]++;
			SH[S_OWNER] = me;
		}
	SH[S_ROUND]++;
	if (SH[S_JPID] == me)
		SH[S_JTICKS] += ticks - tk;
}

static void
ontick(sig)
	int sig;
{
	ticks++;
}

static void
onusr(sig)
	int sig;
{
	SH[S_GOTUSR] = 1;
}

static void
handler(sig, f)
	int sig;
	void (*f)();
{
	long sv[3];

	sv[0] = (long)f;
	sv[1] = 0;
	sv[2] = 0;
	sys15(A_SIGVEC, sig, sv, 0);
}

static int
openui()
{
	ufd = sys15(A_OPEN, "/dev/uinter0", O_RDWR);
	me = sys15(A_GETPID);
	sys15(A_SETCOMPAT, 0x407);
	return ufd >= 0;
}

/* attach Mac memory and join; 0 on failure */
static int
join(pfx)
	char *pfx;
{
	long id, v, a[3];

	id = sys0(A_SHMSYS, 3, TLOW, 0, 0600);
	v = id >= 0 ? sys0(A_SHMSYS, 0, id, 1, 020000) : -1;
	if (!check(pfx, id >= 0 && v == 0, "id, errno", id, errno))
		return 0;
	if (pfx[0] == 'j')
		SH[S_JPID] = me;	/* for the creator's UI_SWITCH */
	handler(SIGIOT, ontick);
	a[0] = a[1] = a[2] = 0;
	v = ioc(UI_ATTACHLAYER, a);
	if (!check(pfx[0] == 'j' ? "j_attachlayer" : "attachlayer", v > 0 && v < 16, "slot, errno", v, errno))
		return 0;
	return ioc(UI_SET, 1) == 0;
}

/* the first joiner: started before the session, from outside it */
static void
joiner()
{
	long v, t, a[3], cpid;

	if (!openui())
		sys15(A_EXIT, 1);
	v = 0;
	check("sync_free_at_start", ioc(UI_TEST, &v) == 0, "errno", errno, 0);
	check("nontask_switch", ioc(UI_SWITCH, &v) == -1 && errno == EINVAL, "errno", errno, 0);
	check("nontask_sleep", ioc(UI_SLEEP, 0) == -1 && errno == EINVAL, "errno", errno, 0);
	t = now_ms();
	v = 0;
	v = ioc(UI_SYNC, &v);
	check("sync_waits", v == 0, "errno, ms", errno, now_ms() - t);
	/* the session's memory, as libmac's openSegments takes it */
	if (!join("j_attach_tlow"))
		sys15(A_EXIT, 1);
	check("rom", *(unsigned short *)(ROM + 8) >= 0x67c, "version", *(unsigned short *)(ROM + 8), 0);
	check("switch_uip", *(long *)0x3ff8 == me, "pid", *(long *)0x3ff8, me);
	a[0] = a[1] = a[2] = 0;
	check("attach_twice", ioc(UI_ATTACHLAYER, a) == -1 && errno == EINVAL, "errno", errno, 0);
	cpid = *(long *)(0x10000 + 4 * 8);	/* the creator's pid */
	check("timer", ioc(UI_TIMER, &t) == 0, "errno", errno, 0);
	for (v = 0; v < 4; v++) {
		section();
		if (v == 1) {
			/* a signal to the waiting creator waits for it to run */
			sys15(A_KILL, cpid, SIGUSR1);
			spin(100L);
			check("signal_held", SH[S_GOTUSR] == 0, "got", SH[S_GOTUSR], 0);
		}
		if (swto(cpid) != 0)
			check("switch_back", 0, "errno", errno, v);
	}
	check("joiner_ticks", SH[S_JTICKS] >= 6, "ticks", SH[S_JTICKS], 0);
	/* exits holding the Mac: the creator runs again */
	sys15(A_EXIT, 0);
}

/* the creator's joiners: k and w hand back at once, a never does */
static void
other(m)
	int m;
{
	long cpid;

	if (!openui() || !join(m == 'k' ? "k_attach_tlow" : m == 'a' ? "a_attach_tlow" :
	    "w_attach_tlow"))
		sys15(A_EXIT, 1);
	cpid = *(long *)(0x10000 + 4 * 8);
	if (m == 'a')
		for (;;)
			section();
	for (;;)
		swto(cpid);
}

/* another user with memory where the ui page is: refused */
static void
stranger()
{
	long id, v, a[3];

	if (!openui() || sys15(A_SETUID, 2) == -1)
		sys15(A_EXIT, 1);
	id = sys0(A_SHMSYS, 3, 0, 0x10000, 01000 | 0600);
	v = id >= 0 ? sys0(A_SHMSYS, 0, id, 1, 020000) : -1;
	a[0] = a[1] = a[2] = 0;
	v = v == 0 ? ioc(UI_ATTACHLAYER, a) : -2;
	check("other_user", v == -1 && errno == EPERM, "rv, errno", v, errno);
	sys15(A_EXIT, 0);
}

static long
spawn(m)
	char *m;
{
	static char *av[3] = { "/aux/bin/macjoin", 0, 0 };
	static char *ev[1] = { 0 };
	long pid;

	av[1] = m;
	pid = sys15(A_FORK);
	if (pid == 0 || sysd1) {
		sys0(A_EXECE, av[0], av, ev);
		sys15(A_EXIT, 127);
	}
	return pid;
}

/* t ticks of UI_DELAY */
static void
delay(t)
	long t;
{
	long b = 0;

	ioc(UI_DELAY, &b);
	b += t;
	ioc(UI_DELAY, &b);
}

/* hand the Mac to pid once it has joined; 0 when it handed back */
static long
first(pid)
	long pid;
{
	long i, r = -1;

	for (i = 0; i < 300 && (r = ioc(UI_SWITCH, &pid)) == -1 && errno == EINVAL; i++)
		delay(2L);
	return r;
}

static void
creator()
{
	long id, v, t, s[12], tk, maxd = 0, jpid, pid, st;
	short m;
	int i;

	if (!openui())
		sys15(A_EXIT, 1);
	id = sys0(A_SHMSYS, 3, TLOW, MEMSIZE, 01000 | 0600);
	v = sys0(A_SHMSYS, 0, id, 1, 020000);
	if (!check("c_tlow", id >= 0 && v == 0, "id, errno", id, errno))
		sys15(A_EXIT, 1);
	SH[S_JPID] = SH[S_OWNER] = SH[S_BAD] = SH[S_GOTUSR] = SH[S_ROUND] = SH[S_JTICKS] = 0;
	*(long *)(0x10000 + 4 * 8) = me;
	if (ioc(UI_MAP, 0x3000) || ioc(UI_CREATELAYER, 0) || ioc(UI_SHMID, &id) ||
	    ioc(UI_ROM, ROM) || ioc(UI_SET, 1)) {
		check("c_setup", 0, "errno", errno, 0);
		sys15(A_EXIT, 1);
	}
	for (i = 0; i < 12; i++)
		s[i] = 0;
	ioc(UI_PHYS_SCREENS, s);
	handler(SIGUSR1, onusr);
	handler(SIGIOT, ontick);
	ioc(UI_TIMER, &t);
	/* the joiner from outside; it has waited in UI_SYNC */
	for (i = 0; i < 300 && SH[S_JPID] == 0; i++)
		delay(2L);
	jpid = SH[S_JPID];
	for (i = 0; i < 5; i++) {
		tk = ticks;
		v = i ? swto(jpid) : first(jpid);
		if (i && ticks - tk > maxd)
			maxd = ticks - tk;
		if (i == 1)
			check("signal_delivered", SH[S_GOTUSR] == 1, "got", SH[S_GOTUSR], 0);
		if (i < 4 && !check("switch", v == 0, "errno, round", errno, i))
			break;
		section();
	}
	check("one_at_a_time", SH[S_BAD] == 0 && SH[S_ROUND] >= 8, "overlaps, rounds",
	    SH[S_BAD], SH[S_ROUND]);
	check("tick_only_running", maxd <= 2, "ticks while waiting", maxd, 0);
	/* the joiner exited holding the Mac */
	check("joiner_exit", ioc(UI_SWITCH, &jpid) == -1 && errno == EINVAL, "errno", errno, 0);
	m = 0xffff;
	check("running_again", ioc(UI_SETEVENTMASK, &m) == 0, "errno", errno, 0);

	/* killed while waiting */
	pid = spawn("k");
	check("k_switch", first(pid) == 0, "errno", errno, 0);
	sys15(A_KILL, pid, SIGKILL);
	st = sys15(A_WAITPID, pid, 0, 0) == pid ? sysd1 : -1;
	check("k_killed", (st & 0x7f) == SIGKILL && ioc(UI_SWITCH, &pid) == -1 && errno == EINVAL,
	    "status, errno", st, errno);

	/* killed while running: a child of ours, no task, kills it */
	pid = spawn("a");
	if ((v = sys15(A_FORK)) == 0 || sysd1) {
		t = now_ms();
		while (SH[S_OWNER] != pid && now_ms() - t < 15000)
			;
		spin(300L);
		sys15(A_KILL, pid, SIGKILL);
		sys15(A_EXIT, 0);
	}
	SH[S_BAD] = 0;
	t = now_ms();
	check("a_switch", first(pid) == 0, "errno", errno, 0);
	section();
	check("a_killed_running", SH[S_BAD] == 0 && now_ms() - t >= 300, "overlaps, ms",
	    SH[S_BAD], now_ms() - t);
	sys15(A_WAITPID, v, 0, 0);
	st = sys15(A_WAITPID, pid, 0, 0) == pid ? sysd1 : -1;
	check("a_status", (st & 0x7f) == SIGKILL, "status", st, 0);

	pid = spawn("u");
	st = sys15(A_WAITPID, pid, 0, 0) == pid ? sysd1 : -1;
	check("u_status", st == 0, "status", st, 0);

	/* left waiting; t_mac kills us and checks it goes too */
	pid = spawn("w");
	check("w_switch", first(pid) == 0, "errno", errno, 0);
	out("done\n");
	line("K ", pid);
	for (;;)
		delay(60L);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	long v = 0;

	if (argc < 2)
		return 2;
	switch (argv[1][0]) {
	case 'j':
		joiner();
	case 'c':
		creator();
	case 'u':
		stranger();
	case 't':
		openui();
		return ioc(UI_TEST, &v) == 0 ? 0 : 1;
	}
	other(argv[1][0]);
	return 0;
}
