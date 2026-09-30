/*
 * macabi.c -- a static A/UX program that becomes a Mac task the way
 * startmac does (/dev/uinter0, Mac RAM at 0, the ui page, a layer, the
 * ROM) and checks what the kernel then provides: A-line traps reach
 * the handler in low memory $28 with the 8-byte frame, access faults
 * the one at $8 with the 60-byte frame, privileged
 * instructions run against the virtual SR, signals wait while the
 * virtual IPL is up, the SIGIOT tick, ROM writes private to the task.
 *
 * Freestanding: A/UX numbers and structures, no libc.  One line per
 * check on stdout: "P name" or "F name: detail"; "I name value" for
 * information; "done" at the end.
 */

extern long sys15(), sys0();
extern long get_sr(), set_sr(), eor_sr(), sr_mem(), get_cacr(), set_cacr();
extern long get_vbr(), usp_rt(), do_rte(), fp_state(), fp7_get(), aline_do(), aline_n(), buserr_do(), buserr_st();
static void t_events(), t_files(), t_mouse();
extern void fp7_set(), fsave_at(), frestore_at();
extern void ipl7(), ipl0(), do_cpush(), do_reset(), do_reset7(), do_rte7(), aline_h(), buserr_h();
extern char aline_at[];
long errno, sysd1;
long la_w0, la_w1, la_sp, la_count;
long be_fv, be_ea, be_ssw;
char macabi_id[8] = "macabi";

#define	A_EXIT		1
#define	A_FORK		2
#define	A_WRITE		4
#define	A_OPEN		5
#define	A_CLOSE		6
#define	A_GETPID	20
#define	A_KILL		37
#define	A_SHMSYS	52
#define	A_IOCTL		54
#define	A_SETCOMPAT	128
#define	A_SIGVEC	129
#define	A_GETTIMEOFDAY	136
#define	A_WAITPID	151

#define	O_RDWR		2
#define	SIGILL		4
#define	SIGIOT		6
#define	SIGBUS		10
#define	SIGSEGV		11
#define	SIGUSR1		16
#define	EINVAL		22
#define	EEXIST		17

#define	UI_GETVERSION	0x20005100
#define	UI_SET		0x20005101
#define	UI_ROM		0x20005105
#define	UI_UNROM	0x20005106
#define	UI_MAP		0x20005107
#define	UI_CREATELAYER	0x20005115
#define	UI_COPY_OUT	0x80085120
#define	UI_PHYS_SCREENS	0xc0305122
#define	UI_TIMER	0xc0045123
#define	UI_ATTACHGFD	0x20005124
#define	UI_KILLMYLAYER	0x2000512b
#define	UI_SHMID	0x8004512e
#define	UI_TEST		0x80045134
#define	UI_GET_PRODINFO	0x20005143
#define	UI_READPRAM	0x8008511c
#define	UI_WRITEPRAM	0x8008511d
#define	UI_VIDEO_STATUS	0xc0325130
#define	UI_CURSOR	0x20005109
#define	UI_DELAY	0xc004510c
#define	UI_POSTEVENT	0x80065110
#define	UI_FLUSHEVENTS	0x80045112
#define	UI_GETOSEVENT	0xc0205113
#define	UI_DEVICES	0x20005118
#define	UI_GETKEYS	0x20005145
#define	A_SLOTMGR	66
#define	A_SYSM68K	38
#define	A_GETDTABLESIZE	104
#define	A_MKDIR		108
#define	A_RMDIR		109
#define	A_FIDOP		152
#define	A_CSOP		161
#define	A_XSTAT		163
#define	A_FSETXINFO	164
#define	A_SETXINFO	165
#define	UI_GETDQEL	0xc010512a
#define	O_GLOBAL	0x80000000

#define	ROM		0x40800000
#define	LOW(a)		(*(volatile long *)(a))
#define	LOW16(a)	(*(volatile unsigned short *)(a))
#define	B16(p)		((p)[0] << 8 | (p)[1])
#define	B32(p)		((long)B16(p) << 16 | B16((p) + 2))

static char obuf[160];
static int ufd;

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
hex(p, v)
	char *p;
	unsigned long v;
{
	int i;

	*p++ = '0';
	*p++ = 'x';
	for (i = 28; i >= 0; i -= 4)
		*p++ = "0123456789abcdef"[(v >> i) & 15];
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
		p = hex(p, a);
		*p++ = ' ';
		p = hex(p, b);
	}
	*p++ = '\n';
	*p = 0;
	out(obuf);
	return ok;
}

static void
info(name, v)
	char *name;
	long v;
{
	char *p = obuf;

	p = cat(p, "I ");
	p = cat(p, name);
	*p++ = ' ';
	p = hex(p, v);
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

/* run f in a child; its wait status */
static long
child(f)
	void (*f)();
{
	long pid, st = -1;

	pid = sys15(A_FORK);
	if (pid == 0 || sysd1) {
		(*f)();
		sys15(A_EXIT, 0);
	}
	if (pid > 0 && sys15(A_WAITPID, pid, 0, 0) == pid)
		st = sysd1;
	return st;
}

static void
romwrite()
{
	LOW(ROM + 8) = 0;
}

static volatile int got, gotsig;

static void
onsig(sig, code, scp)
	int sig, code;
	long *scp;
{
	got++;
	gotsig = sig;
}

static volatile int ticks;

static void
ontick(sig)
	int sig;
{
	ticks++;
}

/* a process that takes the ui page and exits without UI_UNMAP */
static void
mapexit()
{
	sys15(A_EXIT, ioc(UI_MAP, 0x3000) == 0 ? 0 : 1);
}

/* the slot $E card: frame buffer, driver Status, Slot Manager */
static void
t_screen(base)
	long base;
{
	unsigned char *fb = (unsigned char *)base, was;
	short pb[25];
	long vpi[3], sp[14], vp[11];
	unsigned char *spb = (unsigned char *)sp;
	int i, d;

	was = fb[0];
	fb[0] = 0x5a;
	for (i = 0; i < 25; i++)
		pb[i] = 0;
	pb[2] = 0x0e;
	pb[13] = 2;				/* GetMode */
	*(long *)(pb + 14) = (long)vpi;
	vpi[2] = 0;
	check("video_getmode", ioc(UI_VIDEO_STATUS, pb) == 0 && pb[2] == 0 &&
	    (vpi[0] >> 16 & 0xffff) == 0x80 && vpi[2] == base, "result, base", pb[2], vpi[2]);
	check("fb_rw", fb[0] == 0x5a, "byte", fb[0], 0);
	fb[0] = was;
	for (i = 0; i < 14; i++)
		sp[i] = 0;
	spb[49] = 0x0e;
	spb[50] = 0x80;
	check("slot_rsrcinfo", sys0(A_SLOTMGR, 0x16, sp) == 0 &&
	    ((short *)sp)[20] == 3 && ((short *)sp)[21] == 1, "category, type",
	    ((short *)sp)[20], ((short *)sp)[21]);
	spb[50] = 0x80;
	i = sys0(A_SLOTMGR, 6, sp);		/* sFindStruct: the mode list */
	spb[50] = 1;				/* mVidParams */
	sp[0] = 0;
	i |= sys0(A_SLOTMGR, 5, sp);		/* sGetBlock: size first */
	check("slot_vpblock_size", i == 0 && sp[2] == 0x2a, "err, size", (long)i, sp[2]);
	sp[0] = (long)vp;
	vp[1] = 0;
	i = sys0(A_SLOTMGR, 5, sp);
	d = vp[8] >> 16 & 0xffff;		/* pixel size */
	check("slot_vpblock", i == 0 && (vp[1] >> 16 & 0xffff) > 0 && (vp[2] & 0xffff) > 0 &&
	    d > 0 && d <= 32 && (d & (d - 1)) == 0, "row bytes, depth", vp[1] >> 16 & 0xffff, d);
	spb[49] = 0x09;
	check("slot_empty", sys0(A_SLOTMGR, 0x10, sp) == -300, "err", sys0(A_SLOTMGR, 0x10, sp), 0);
}

static void
t_setup()
{
	long v, id, b[2], s[12];
	unsigned char pi[0x34];
	int i, ok, scr;

	ufd = sys15(A_OPEN, "/dev/uinter0", O_RDWR);
	if (!check("open", ufd >= 0, "errno", errno, 0))
		sys15(A_EXIT, 1);
	check("version", ioc(UI_GETVERSION, 0) == 5, "rv", ioc(UI_GETVERSION, 0), errno);
	v = 0;
	check("test_free", ioc(UI_TEST, &v) == 0, "errno", errno, 0);
	check("createlayer_unmapped", ioc(UI_CREATELAYER, 0) == -1 && errno == EINVAL,
	    "errno", errno, 0);
	id = sys0(A_SHMSYS, 3, 0, 0x100000, 01000 | 0600);
	v = sys0(A_SHMSYS, 0, id, 1, 020000);
	if (!check("low_memory_at_0", id >= 0 && v == 0, "id, addr", id, v))
		sys15(A_EXIT, 1);
	sys0(A_SHMSYS, 1, id, 0, 0);		/* IPC_RMID: goes with the process */
	v = child(mapexit);
	check("map_holder_exits", v == 0, "status", v, 0);
	check("map_unaligned", ioc(UI_MAP, 0x3004) == -1 && errno == EINVAL, "errno", errno, 0);
	check("map", ioc(UI_MAP, 0x3000) == 0 && *(unsigned char *)0x3458 == 0x80,
	    "errno, c_button", errno, *(unsigned char *)0x3458);
	check("map_twice", ioc(UI_MAP, 0x3000) == -1 && errno == EINVAL, "errno", errno, 0);
	check("createlayer", ioc(UI_CREATELAYER, 0) == 0, "errno", errno, 0);
	v = 0;
	check("test_inuse", ioc(UI_TEST, &v) == -1 && errno == EEXIST, "errno", errno, 0);
	check("createlayer_twice", ioc(UI_CREATELAYER, 0) == -1 && errno == EEXIST,
	    "errno", errno, 0);
	check("shmid", ioc(UI_SHMID, &id) == 0 && ioc(UI_ATTACHGFD, 0) == 0, "errno", errno, 0);
	for (i = 0; i < 12; i++)
		s[i] = 0x12345678;
	ok = ioc(UI_PHYS_SCREENS, s) == 0;
	scr = ((unsigned char *)s)[0] == 0x0e;
	if (scr)
		ok &= ((unsigned char *)s)[1] == 0x80 && (s[1] & 0xf0000000) == 0xe0000000;
	for (i = scr; i < 6; i++)
		ok &= ((unsigned char *)s)[8 * i] == 0xff;
	check("phys_screens", ok, "errno", errno, s[0]);
	info("screens", (long)scr);
	if (scr)
		t_screen(s[1]);
	check("rom", ioc(UI_ROM, ROM) == 0, "errno", errno, 0);
	check("rom_version", (*(unsigned short *)(ROM + 8)) >= 0x67c &&
	    *(long *)(ROM + 0x40) >= 0x80000, "version, size",
	    *(unsigned short *)(ROM + 8), *(long *)(ROM + 0x40));
	check("rom_twice", ioc(UI_ROM, ROM) == -1 && errno == EINVAL, "errno", errno, 0);
	v = child(romwrite);
	check("rom_private", v == 0 && *(unsigned short *)(ROM + 8) >= 0x67c,
	    "status, version", v, *(unsigned short *)(ROM + 8));
	b[0] = 0xdd8;
	b[1] = 4;
	check("copy_out", ioc(UI_COPY_OUT, b) == 0 && (LOW(0xdd8) & 0xfff00000) == ROM,
	    "errno, UnivInfoPtr", errno, LOW(0xdd8));
	b[0] = 0x12f;
	b[1] = 1;
	check("copy_out_cpu", ioc(UI_COPY_OUT, b) == 0 && *(unsigned char *)0x12f == 4,
	    "errno, CPUFlag", errno, *(unsigned char *)0x12f);
	b[0] = 0x2ffe;
	b[1] = 4;
	check("copy_out_range", ioc(UI_COPY_OUT, b) == -1 && errno == EINVAL, "errno", errno, 0);
	b[0] = 0xcb3;
	b[1] = 1;
	ioc(UI_COPY_OUT, b);
	check("prodinfo", ioc(UI_GET_PRODINFO, pi) == 0 && pi[0x12] == *(unsigned char *)0xcb3 &&
	    LOW(0xdd8) - ROM + 0x34 <= *(long *)(ROM + 0x40) &&
	    *(unsigned char *)(LOW(0xdd8) + 0x12) == pi[0x12],
	    "errno, id", errno, pi[0x12]);
	info("boxflag", (long)pi[0x12]);
	b[0] = (long)s;
	b[1] = (4L << 16) | 0x10;
	ioc(UI_READPRAM, b);
	v = s[0];
	s[0] = 0x0a0b0c0d;
	check("pram", ioc(UI_WRITEPRAM, b) == 0 && (s[0] = 0) == 0 &&
	    ioc(UI_READPRAM, b) == 0 && s[0] == 0x0a0b0c0d, "errno, value", errno, s[0]);
	s[0] = v;
	ioc(UI_WRITEPRAM, b);
	b[1] = (4L << 16) | 0xfe;
	check("pram_range", ioc(UI_READPRAM, b) == -1 && errno == EINVAL, "errno", errno, 0);
	/* from the PRAM file, which t_mac marks; 32-bit mode set whatever it says */
	b[1] = (4L << 16) | 0xfc;
	check("pram_loaded", ioc(UI_READPRAM, b) == 0 && s[0] == 0x5052414dL, "errno, value",
	    errno, s[0]);
	b[1] = (1L << 16) | 0x8a;
	check("pram_32bit", ioc(UI_READPRAM, b) == 0 && (*(unsigned char *)s & 5) == 5,
	    "errno, $8A", errno, *(unsigned char *)s);
	b[1] = (4L << 16) | 0xfc;
	s[0] = 0x4d616321L;			/* "Mac!", for the file */
	ioc(UI_WRITEPRAM, b);
}

/* SIGSEGV sent by a process, with a handler at $8 */
static void
killsegv()
{
	sys15(A_KILL, sys15(A_GETPID), SIGSEGV);
}

static void
t_aline()
{
	long sp, t, pc;

	LOW(0x28) = (long)aline_h;
	check("set", ioc(UI_SET, 1) == 0, "errno", errno, 0);
	la_count = 0;
	sp = aline_do();
	check("aline", la_count == 1, "count", la_count, 0);
	pc = la_w0 << 16 | (la_w1 >> 16 & 0xffff);
	check("aline_frame_pc", pc == (long)aline_at, "pc", pc, (long)aline_at);
	check("aline_frame_fv", (la_w1 & 0xffff) == 0x0028, "fv", la_w1 & 0xffff, 0);
	check("aline_frame_sr", (la_w0 >> 16 & 0xffff) == 0x2015, "sr", la_w0 >> 16 & 0xffff, 0);
	check("aline_frame_sp", la_sp == sp - 8, "sp, expected", la_sp, sp - 8);
	t = now_ms();
	la_count = 0;
	aline_n(20000);
	t = now_ms() - t;
	check("aline_20000", la_count == 20000, "count", la_count, 0);
	info("aline_20000_ms", t);
	LOW(8) = (long)buserr_h;
	t = buserr_do(-4L);
	check("buserr_vector", t == -1 && be_fv == 0x7008 && be_ea == -4L, "fv, ea", be_fv, be_ea);
	check("buserr_read", (be_ssw & 0x100) != 0, "ssw", be_ssw, 0);
	t = buserr_st(-4L);
	check("buserr_write", t == -1 && (be_ssw & 0x100) == 0, "ssw", be_ssw, 0);
	t = child(killsegv);
	check("buserr_kill_stays_signal", (t & 0x7f) == SIGSEGV, "status", t, 0);
	LOW(8) = 0;
}

/* exits with the fsave frame size, 255 if wrong */
static void
fpchild()
{
	long v = fp_state();

	sys15(A_EXIT, v < 0 && v >= -100 ? -v : 255);
}

/* fp7 survives a switch between fsave and frestore; exits 0 if so */
static void
fpswitch()
{
	long f[26], pid;

	fp7_set();
	fsave_at(f);
	pid = sys15(A_FORK);
	if (pid == 0 || sysd1)
		sys15(A_EXIT, 0);
	sys15(A_WAITPID, pid, 0, 0);
	frestore_at(f);
	sys15(A_EXIT, fp7_get() == 7 ? 0 : 1);
}

static void
t_priv()
{
	char m[4];
	long v;

	check("sr_s", (get_sr() & 0xffff & ~0x1f) == 0x2000, "sr", get_sr(), 0);
	ipl7();
	check("ori_sr", (get_sr() & 0x2700) == 0x2700, "sr", get_sr(), 0);
	ipl0();
	check("andi_sr", (get_sr() & 0x2700) == 0x2000, "sr", get_sr(), 0);
	set_sr(0x0304L);
	v = get_sr();
	check("move_to_sr", (v & 0xffff) == 0x2304, "sr (S stays)", v, 0);
	set_sr(0x2000L);
	check("eori_sr", eor_sr() == 0x1f, "ccr", eor_sr(), 0);
	check("move_sr_mem", sr_mem(m) == (long)m + 2 && (m[0] & 0xff) == 0x20, "a0, byte",
	    sr_mem(m), m[0]);
	set_cacr(0x80008000L);
	check("movec_cacr", get_cacr() == 0x80008000L, "cacr", get_cacr(), 0);
	check("movec_vbr", get_vbr() == 0, "vbr", get_vbr(), 0);
	check("move_usp", usp_rt(0x12345670L) == 0x12345670L, "usp", usp_rt(0x12345670L), 0);
	check("rte_format0", do_rte() == 0x04 && (get_sr() & 0x2700) == 0x2000, "ccr, sr",
	    do_rte(), get_sr());
	do_cpush();
	check("cpush", 1, "", 0, 0);
	v = child(fpchild);
	if ((v & 0x7f) == SIGILL)
		info("fsave_sigill", v);	/* no FPU */
	else
		check("fsave_frestore", (v & 0xff) == 0 && ((v >> 8 & 0xff) == 4 ||
		    (v >> 8 & 0xff) == 0x34 || (v >> 8 & 0xff) == 0x64), "status", v, 0);
	v = child(fpswitch);
	if ((v & 0x7f) != SIGILL)
		check("fsave_switch_frestore", v == 0, "status", v, 0);
	v = child(do_reset);
	check("reset_sigill", (v & 0x7f) == SIGILL, "status", v, 0);
	v = child(do_reset7);
	check("reset_ipl7_sigill", (v & 0x7f) == SIGILL, "status", v, 0);
	v = child(do_rte7);
	check("rte_format7_sigill", (v & 0x7f) == SIGILL, "status", v, 0);
}

static void
t_ipl()
{
	long sv[3], me = sys15(A_GETPID), t;

	check("setcompat", sys15(A_SETCOMPAT, 0x407) >= 0, "errno", errno, 0);
	sv[0] = (long)onsig;
	sv[1] = 0;
	sv[2] = 0;
	sys15(A_SIGVEC, SIGUSR1, sv, 0);
	got = 0;
	ipl7();
	sys15(A_KILL, me, SIGUSR1);
	sys15(A_GETPID);
	check("ipl_holds", got == 0, "count", got, 0);
	ipl0();
	check("ipl0_delivers", got == 1 && gotsig == SIGUSR1, "count, signal", got, gotsig);
	sv[0] = (long)ontick;
	sys15(A_SIGVEC, SIGIOT, sv, 0);
	ticks = 0;
	check("timer", ioc(UI_TIMER, &t) == 0, "errno", errno, 0);
	t = now_ms();
	while (ticks < 10 && now_ms() - t < 3000)
		;
	check("tick", ticks >= 10, "ticks", ticks, 0);
	info("ticks_ms", now_ms() - t);
	ipl7();
	ticks = 0;
	t = now_ms();
	while (now_ms() - t < 200)
		;
	check("tick_held", ticks == 0, "ticks", ticks, 0);
	ipl0();
	check("tick_released", ticks >= 1 && ticks <= 2, "ticks", ticks, 0);
	t_events();
	t_files();
	t_mouse();
	check("killmylayer", ioc(UI_KILLMYLAYER, 0) == 0, "errno", errno, 0);
	ticks = 0;
	t = now_ms();
	while (now_ms() - t < 200)
		;
	check("tick_stopped", ticks == 0, "ticks", ticks, 0);
	t = 0;
	check("test_free_again", ioc(UI_TEST, &t) == 0, "errno", errno, 0);
}

/* post, peek, take, flush; one GetOSEvent record, what in *wp */
static long
osevent(blk, mask, wp, msgp, whenp)
	int blk, mask;
	long *wp, *msgp, *whenp;
{
	unsigned char g[32];
	long r;
	int i;

	for (i = 0; i < 32; i++)
		g[i] = 0;
	g[0] = blk;
	g[2] = mask >> 8;
	g[3] = mask;
	g[23] = 6;				/* timeOut */
	r = ioc(UI_GETOSEVENT, g);
	*wp = (short)B16(g + 4);
	*msgp = B32(g + 6);
	*whenp = B32(g + 10);
	return r;
}

static long
post(what, msg)
	int what;
	long msg;
{
	unsigned char b[6];

	b[0] = what >> 8;
	b[1] = what;
	b[2] = msg >> 24;
	b[3] = msg >> 16;
	b[4] = msg >> 8;
	b[5] = msg;
	return ioc(UI_POSTEVENT, b);
}

/* while the tick runs: Ticks, Time, the event queue, Delay */
static void
t_events()
{
	unsigned char k[128];
	long t, t0, w, m, wh, r, tv[2];
	int i, n;

	t = now_ms();
	t0 = LOW(0x16a);
	while (now_ms() - t < 500)
		;
	info("ticks_500ms", LOW(0x16a) - t0);
	check("ticks_60hz", LOW(0x16a) - t0 >= 24 && LOW(0x16a) - t0 <= 36, "ticks",
	    LOW(0x16a) - t0, 0);
	sys15(A_GETTIMEOFDAY, tv);
	t = tv[0] + 2082844800L;
	check("time_lowmem", LOW(0x20c) - t >= -2 && LOW(0x20c) - t <= 2, "Time, expected",
	    LOW(0x20c), t);
	check("mouse_lowmem", LOW16(0x830) > 0 && LOW16(0x832) > 0 && LOW(0x82c) == LOW(0x830),
	    "Mouse", LOW(0x830), LOW(0x82c));
	check("mbstate_up", *(volatile unsigned char *)0x172 == 0x80, "MBState",
	    *(volatile unsigned char *)0x172, 0);
	check("devices_cursor", ioc(UI_DEVICES, 0) == 0 && ioc(UI_CURSOR, 0) == 0, "errno", errno, 0);
	r = osevent(0, 0xffff, &w, &m, &wh);
	check("getosevent_null", r == 0 && w == 0 && wh - LOW(0x16a) <= 2 &&
	    LOW(0x16a) - wh <= 2, "what, when", w, wh);
	check("postevent", post(12, 0x12345678L) == 0 && post(13, 13L) == 0, "errno", errno, 0);
	r = osevent(2, 1 << 12, &w, &m, &wh);
	check("getosevent_avail", r == 0 && w == 12 && m == 0x12345678L, "what, msg", w, m);
	r = osevent(0, 1 << 12, &w, &m, &wh);
	check("getosevent_take", r == 0 && w == 12 && m == 0x12345678L, "what, msg", w, m);
	r = osevent(0, 1 << 12, &w, &m, &wh);
	check("getosevent_taken", r == 0 && w == 0, "what", w, 0);
	k[0] = 0x20;				/* eventMask 1 << 13, no stop */
	k[1] = k[2] = k[3] = 0;
	check("flushevents", ioc(UI_FLUSHEVENTS, k) == 0, "errno", errno, 0);
	r = osevent(0, 0xffff, &w, &m, &wh);
	check("flushed", r == 0 && w == 0, "what", w, 0);
	for (i = 0; i < 40; i++)
		post(14, (long)i);
	for (n = 0; osevent(0, 1 << 14, &w, &m, &wh) == 0 && w == 14; n++)
		t0 = m;
	check("queue_32_newest", n == 32 && t0 == 39, "events, last", (long)n, t0);
	t = now_ms();
	r = osevent(1, 0xffff, &w, &m, &wh);
	check("getosevent_block", r == 0 && w == 0 && now_ms() - t < 500, "rv, what", r, w);
	info("block_ms", now_ms() - t);
	t = now_ms();
	w = LOW(0x16a) + 12;
	while ((r = ioc(UI_DELAY, &w)) == -1 && errno == 4)
		;
	info("delay_ms", now_ms() - t);
	check("delay", r == 0 && now_ms() - t >= 150 && now_ms() - t < 1000, "rv, ms", r,
	    now_ms() - t);
	for (i = 0; i < 128; i++)
		k[i] = 0;
	r = ioc(UI_GETKEYS, k);
	for (n = i = 0; i < 128; i++)
		n += k[i] == 0xff;
	check("getkeys_up", r == 0 && n == 128, "rv, keys up", r, (long)n);
}

static int
same(a, b, n)
	char *a, *b;
	int n;
{
	while (n-- > 0)
		if (*a++ != *b++)
			return 0;
	return 1;
}

/* A/UX file calls: the File ID queues, csop, xinfo, sysm68k, global fds */
static void
t_files()
{
	long h[4], b[25], r, id;
	char buf[16];
	int fd, i;

	h[0] = 7;
	h[1] = (long)"hello";
	h[2] = 5;
	h[3] = 5;
	id = sys0(A_FIDOP, 1, h);
	check("fidop_send", id >= 0, "rv, errno", id, errno);
	h[0] = 0;
	h[1] = (long)buf;
	h[2] = sizeof buf;
	h[3] = 0;
	r = sys0(A_FIDOP, 2, h);
	check("fidop_serve", r == 0 && h[0] == 7 && h[3] == 5 && same(buf, "hello", 5),
	    "errno, type", r ? errno : 0, h[0]);
	h[0] = 9;
	h[1] = (long)"ok";
	h[2] = 2;
	h[3] = 2;
	check("fidop_reply", sys0(A_FIDOP, 3, h) >= 0, "errno", errno, 0);
	h[0] = 0;
	h[1] = (long)buf;
	h[2] = sizeof buf;
	h[3] = 0;
	r = sys0(A_FIDOP, 4, h);
	check("fidop_take_reply", r == 0 && h[0] == 9 && h[3] == 2 && same(buf, "ok", 2),
	    "errno, type", r ? errno : 0, h[0]);
	h[0] = 7;
	h[1] = (long)"x";
	h[2] = 1;
	h[3] = 1;
	id = sys0(A_FIDOP, 1, h);
	check("fidop_cancel", id >= 0 && sys0(A_FIDOP, 5, id) == 0 &&
	    sys0(A_FIDOP, 5, id) == -1 && errno == EINVAL, "id, errno", id, errno);
	h[2] = 0;
	check("fidop_short_buffer", sys0(A_FIDOP, 1, h) == -1 && errno == EINVAL, "errno",
	    errno, 0);
	check("fidop_bad_op", sys0(A_FIDOP, 6, h) == -1 && errno == EINVAL, "errno", errno, 0);
	check("csop_not_ready", sys0(A_CSOP, 8, 0, 0) == 0, "errno", errno, 0);
	r = sys0(A_CSOP, 1, buf, 4);
	check("csop_send", r == -1 && errno == 11, "rv, errno", r, errno);
	check("csop_poll", sys0(A_CSOP, 6, buf, 4) == -1 && errno == 55, "errno", errno, 0);
	check("xstat_xerror", sys0(A_XSTAT, "/", b) == 0 &&
	    ((short *)b)[0x3a / 2] == 0 && ((short *)b)[0x3c / 2] == EINVAL,
	    "errno, st_xerror", errno, ((short *)b)[0x3c / 2]);
	check("setxinfo", sys0(A_SETXINFO, "/", b) == -1 && errno == EINVAL, "errno", errno, 0);
	check("setxinfo_noent", sys0(A_SETXINFO, "/no/such", b) == -1 && errno == 2, "errno",
	    errno, 0);
	check("fsetxinfo_badf", sys0(A_FSETXINFO, 99, b) == -1 && errno == 9, "errno", errno, 0);
	/* the File Manager's change flags: a name made by a Mac task */
	sys0(A_SYSM68K, 0x6a);
	r = sys0(A_MKDIR, "/tmp/macabi.d", 0755);
	id = sys0(A_SYSM68K, 0x6a);
	check("fmgrflag_mkdir", r == 0 && (id & 1) && sys0(A_SYSM68K, 0x6a) == 0, "flags",
	    id, errno);
	sys0(A_RMDIR, "/tmp/macabi.d");
	check("cache_info", sys0(A_SYSM68K, 0x6b, b) == 0 && b[3] > 0, "errno, size", errno,
	    b[3]);
	check("bufcache", sys0(A_SYSM68K, 0x6d, b) == 0 && b[0] > 0 && b[3] > 0,
	    "buffers, memory", b[0], b[3]);
	for (i = 0; i < 4; i++)
		b[i] = 0;
	check("getdqel_end", ioc(UI_GETDQEL, b) == -1 && errno == EINVAL, "errno", errno, 0);
	check("gfd_room", sys0(A_GETDTABLESIZE) >= 192, "fds", sys0(A_GETDTABLESIZE), 0);
	fd = sys0(A_OPEN, "/", O_GLOBAL);
	check("open_global", fd >= 128, "fd, errno", fd, errno);
	if (fd >= 0)
		sys0(A_CLOSE, fd);
}

/*
 * The mouse as the Mac's cursor task keeps it: a delta in MTemp is
 * accelerated into RawMouse and Mouse over MickeyBytes; a position
 * the Mac sets is taken as it is.
 */
#define	MB	0x2e00			/* a MickeyBytes block in free low memory */

static void
waitticks(n)
	long n;
{
	long t = LOW(0x16a) + n;

	while (LOW(0x16a) - t < 0)
		;
}

static void
t_mouse()
{
	volatile unsigned char *lm = 0;
	int i;

	for (i = 0; i < 28; i++)
		lm[MB + i] = 0;
	lm[MB + 1] = 1;				/* count */
	lm[MB + 3] = 8;				/* limit */
	lm[MB + 20] = 4;			/* thresholds */
	for (i = 21; i < 28; i++)
		lm[MB + i] = 255;
	LOW(0xd6a) = MB;
	LOW(0x8d6) = -1;			/* MouseMask */
	LOW(0x8da) = 0;				/* MouseOffset */
	LOW(0x834) = 0;				/* CrsrPin */
	LOW(0x838) = 600L << 16 | 800;
	lm[0x8cd] = 0;				/* not busy */
	lm[0x8cf] = 0xff;			/* coupled */
	waitticks(2L);
	LOW(0x82c) = 100L << 16 | 100;		/* RawMouse */
	LOW(0x828) = 100L << 16 | 110;		/* MTemp: 10 right */
	lm[0x8ce] = 1;				/* CrsrNew */
	waitticks(3L);
	/* magnitude 10: smoothed to 7, twice for passing the threshold 4 */
	check("mouse_accel", LOW(0x82c) == (100L << 16 | 114) && LOW(0x830) == LOW(0x82c) &&
	    LOW(0x828) == LOW(0x82c), "RawMouse, Mouse", LOW(0x82c), LOW(0x830));
	check("mouse_accel_state", LOW16(MB) == 2 && LOW16(MB + 16) == 10 && LOW16(MB + 18) == 2,
	    "count, remainder", LOW16(MB), LOW16(MB + 18));
	LOW(0x82c) = 150L << 16 | 160;
	LOW(0x828) = 150L << 16 | 160;
	lm[0x8ce] = 1;
	waitticks(3L);
	check("mouse_set_by_mac", LOW(0x830) == (150L << 16 | 160), "Mouse", LOW(0x830), 0);
	LOW(0x82c) = 150L << 16 | 900;		/* off the pin */
	LOW(0x828) = 150L << 16 | 900;
	lm[0x8ce] = 1;
	waitticks(3L);
	check("mouse_pinned", LOW(0x830) == (150L << 16 | 799), "Mouse", LOW(0x830), 0);
	lm[0x8cf] = 0;
	LOW(0xd6a) = 0;
}

/* the same change by a process outside the Mac environment */
static void
t_fmgrother()
{
	long r, f;

	sys0(A_SYSM68K, 0x6a);
	r = sys0(A_MKDIR, "/tmp/macabi.d", 0755);
	f = sys0(A_SYSM68K, 0x6a);
	check("fmgrflag_other", r == 0 && f == 4, "flags", f, errno);
	sys0(A_RMDIR, "/tmp/macabi.d");
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	t_setup();
	t_aline();
	t_priv();
	t_ipl();
	t_fmgrother();
	out("done\n");
	return 0;
}
