/*
 * t_sound.c -- host sound: /dev/asc and the sound service.
 *
 * Starts sndd, plays through /dev/sound and checks that the chip's
 * interrupts pace playback, that a client of a session in the back is
 * dropped at the chip's rate without reaching the chip, and that a
 * switch makes it the one heard.
 *
 * Then /dev/snd through sndaux, as A/UX's toolbox drives it, and the Mac
 * OS 7.6.1 and System 6 environments with the SndTest INIT: SysBeep and
 * SndPlay reach the chip, an asynchronous SndPlay returns at once and its
 * callBackCmd fires after the sound.
 */
#include <sys/types.h>
#include <sys/stropts.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mkdev.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <sys/procfs.h>
#include "sys/mod.h"
#include "t.h"
#include "dsio.h"
#include "sndio.h"

#define SNDD	"/usr/lib/sndd"
#define CHIPHZ	22254

static pid_t sndd = -1, sndaux = -1;

#define STARTMAC "/mac/bin/startmac"
#define MACSYS	"/macsys/S761"
#define ROOT6	"/a201"
#define SYS6	ROOT6 "/mac/sys/Sys6"
#define SYS6DIR	ROOT6 "/mac/sys/System Folder"
#define SNDTEST	"/mac/lib/SndTest"
#define SNDAUX	"/usr/lib/sndaux"
#define MACDEV	"/dev/dsk/c1d0s0"
#define TESTF	"/tmp/sndtest"
#define TLOW	0x744c4f57		/* 'tLOW', the Mac RAM segment */
#define MD	"/tests/aux/mod.d"
#define MACLOG	"/tmp/sndmac.log"
#define FIDD	"/etc/aux/fidd"

extern int getksym();

static int
kset(char *name, long v)
{
	unsigned long a = 0, info;
	int fd, ok;

	if (getksym(name, &a, &info) < 0 || (fd = open("/dev/kmem", O_RDWR)) < 0)
		return -1;
	ok = lseek(fd, (off_t)a, 0) != -1 && write(fd, (char *)&v, 4) == 4;
	close(fd);
	return ok ? 0 : -1;
}

/* SoundOut's self-test in a 7.6.1 session, as a Quadra 800 */
static int sstat(int, struct sndstat *);

/* every fidd: it forks into a process group of its own */
static void
fiddkill(void)
{
	char path[32];
	prpsinfo_t ps;
	struct dirent *de;
	DIR *d;
	int fd;

	if ((d = opendir("/proc")) == 0)
		return;
	while ((de = readdir(d)) != 0) {
		sprintf(path, "/proc/%s", de->d_name);
		if (de->d_name[0] == '.' || (fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_zomb == 0 &&
		    strcmp(ps.pr_fname, "fidd") == 0)
			kill(ps.pr_pid, SIGKILL);
		close(fd);
	}
	closedir(d);
}

/* the File ID daemon, which the Mac's file system waits on, with A/UX's mount table */
static void
fidd(void)
{
	struct stat sb, mb;
	char l[120];
	int on = 0, st;
	pid_t pid;
	FILE *f;

	if (stat(FIDD, &sb) < 0 || stat("/", &sb) < 0 || stat(MACSYS, &mb) < 0)
		return;
	if ((f = fopen("/etc/mtab", "r")) != 0) {
		while (fgets(l, sizeof l, f))
			on |= strstr(l, " /macsys ") != 0;
		fclose(f);
	} else if ((f = fopen("/etc/mtab", "w")) != 0) {
		fprintf(f, "/dev/dsk/c0d0s0 / 4.2 rw,noquota,dev=%x 1 1\n",
		    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
		fclose(f);
	}
	if (!on && (f = fopen("/etc/mtab", "a")) != 0) {
		fprintf(f, MACDEV " /macsys 4.2 rw,noquota,dev=%x 1 1\n",
		    (int)(major(mb.st_dev) << 8 | minor(mb.st_dev)));
		fclose(f);
	}
	mkdir("/Desktop Folder", 0777);
	fiddkill();
	if ((pid = fork()) == 0) {
		setpgrp();
		execl(FIDD, "fidd", "-d", (char *)0);
		_exit(127);
	}
	t_waitchild(pid, &st, 10);
	t_info("fidd", "status %#x", st);
}

/* where each process sleeps: a stopped Mac's syscall (d0) and pc */
static void
psdump(void)
{
	char path[32];
	prpsinfo_t ps;
	prstatus_t st;
	struct dirent *de;
	DIR *d;
	int fd;

	if ((d = opendir("/proc")) == 0)
		return;
	while ((de = readdir(d)) != 0) {
		sprintf(path, "/proc/%s", de->d_name);
		if (de->d_name[0] == '.' || (fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_zomb == 0 && ps.pr_pid > 3 &&
		    ioctl(fd, PIOCSTATUS, &st) == 0)
			t_info("ps", "%d %s %c wchan %#lx pc %#lx instr %#lx d0 %#lx ut %ld",
			    (int)ps.pr_pid, ps.pr_fname, ps.pr_sname, (long)ps.pr_wchan,
			    (long)st.pr_reg[16], st.pr_instr, (long)st.pr_reg[0],
			    (long)st.pr_utime.tv_sec);
		close(fd);
	}
	closedir(d);
}

/* len bytes of the process at a, as hex */
static void
memdump(int fd, char *tag, unsigned long a, int len)
{
	unsigned char b[32];
	char h[80];
	int i, k;

	for (; len > 0; len -= 32, a += 32) {
		if (lseek(fd, (off_t)a, 0) == -1 || (k = read(fd, (char *)b, 32)) <= 0)
			return;
		for (i = 0; i < k; i++)
			sprintf(h + 2 * i, "%02x", b[i]);
		t_info(tag, "%#lx %s", a, h);
	}
}

/* a spinning Mac: its registers, code and stack, three times */
static void
macdump(pid_t pid)
{
	char path[32];
	prstatus_t st;
	int fd, i, j;

	sprintf(path, "/proc/%d", (int)pid);
	for (j = 0; j < 3 && (fd = open(path, O_RDONLY)) >= 0; j++) {
		if (ioctl(fd, PIOCSTATUS, &st) == 0) {
			for (i = 0; i < 18; i += 6)
				t_info("macreg", "%d: %08lx %08lx %08lx %08lx %08lx %08lx", i,
				    (long)st.pr_reg[i], (long)st.pr_reg[i + 1], (long)st.pr_reg[i + 2],
				    (long)st.pr_reg[i + 3], (long)st.pr_reg[i + 4], (long)st.pr_reg[i + 5]);
			memdump(fd, "maccode", ((unsigned long)st.pr_reg[16] & ~31UL) - 96, 224);
			memdump(fd, "macstack", (unsigned long)st.pr_reg[15], 96);
			if (j == 0)
				memdump(fd, "maclow", 0xcc0UL, 32);
		}
		close(fd);
		sleep(1);
	}
}

/* the end of startmac's output */
static void
maclog(void)
{
	char b[200];
	int fd, n, i;

	if ((fd = open(MACLOG, O_RDONLY)) < 0)
		return;
	lseek(fd, (off_t)-(long)(sizeof b - 1), 2);
	n = read(fd, b, sizeof b - 1);
	close(fd);
	b[n > 0 ? n : 0] = 0;
	for (i = 0; i < n; i++)
		if (b[i] == '\n')
			b[i] = '|';
	t_info("mac_log", "%s", b);
}

static volatile int gotpoll;

static void
onpoll(int sig)
{
	gotpoll++;
	signal(SIGPOLL, onpoll);
}

/*
 * /dev/snd as A/UX's toolbox drives it: half a second of samples, then a
 * callBackCmd, which raises SIGPOLL once heard; a note on the square synth.
 */
static void
auxdev(int a)
{
	unsigned char h[24], cmd[8], cb[8], *b;
	struct sndstat s0, s1;
	long n = CHIPHZ / 2, t0, ms = -1, m;
	int fd, i;

	signal(SIGPOLL, onpoll);
	/* until sndaux serves it */
	for (t0 = t_now_ms(); (fd = open("/dev/snd/samp", O_RDWR)) < 0 && errno == ENXIO &&
	    t_now_ms() - t0 < 5000; )
		poll((struct pollfd *)0, 0UL, 50);
	if (!t_check("auxsnd_open", fd >= 0,
	    "/dev/snd/samp: %s", T_ERR))
		return;
	t_check("auxsnd_busy", open("/dev/snd/samp", O_RDWR) < 0 && errno == EBUSY,
		"second open: %s", T_ERR);
	memset(h, 0, sizeof h);
	h[4] = n >> 24; h[5] = n >> 16; h[6] = n >> 8; h[7] = n;	/* length */
	h[8] = 0x56; h[9] = 0xEE; h[10] = 0x8B; h[11] = 0xA3;		/* rate */
	h[21] = 60;
	b = (unsigned char *)malloc(n);
	for (i = 0; i < n; i++)
		b[i] = (i / 25) & 1 ? 0xA0 : 0x60;
	s0.ss_fed = 0;
	sstat(a, &s0);
	t0 = t_now_ms();
	t_check("auxsnd_hdr", ioctl(fd, 0xC0187702, h) == 0, "SND_HDR: %s", T_ERR);
	t_check("auxsnd_write", write(fd, (char *)b, n) == n, "write: %s", T_ERR);
	memset(cmd, 0, sizeof cmd);
	cmd[1] = 81;	/* bufferCmd at the chip's rate */
	cmd[5] = 1;
	t_check("auxsnd_cmd", ioctl(fd, 0xC0087703, cmd) == 0, "SND_CMD_I: %s", T_ERR);
	memset(cmd, 0, sizeof cmd);
	cmd[1] = 13;	/* callBackCmd */
	cmd[6] = 0x5A;
	cmd[7] = 0x5A;
	gotpoll = 0;
	t_check("auxsnd_queue", ioctl(fd, 0x20007708, 0) == 0 && ioctl(fd, 0xC0087704, cmd) == 0,
		"SND_QFULL, SND_CMD_Q: %s", T_ERR);
	while (!gotpoll && t_now_ms() - t0 < 5000)
		poll((struct pollfd *)0, 0UL, 20);
	if (gotpoll)
		ms = t_now_ms() - t0;
	m = ioctl(fd, 0x20007705, 0);
	memset(cb, 0, sizeof cb);
	t_check("auxsnd_callback", gotpoll && m == 1 << 5 && ioctl(fd, 0xC0087709, cb) == 0 &&
		memcmp(cb, cmd, 8) == 0 && ioctl(fd, 0x20007705, 0) == 0,
		"SIGPOLL %d after %ld ms, SND_CALLBACK %#lx, command %02x%02x %02x%02x%02x%02x",
		gotpoll, ms, m, cb[0], cb[1], cb[4], cb[5], cb[6], cb[7]);
	t_check("auxsnd_paced", ms >= 400 && ms <= 3000, "0.5 s called back after %ld ms", ms);
	s1.ss_fed = 0;
	sstat(a, &s1);
	t_check("auxsnd_heard", s1.ss_fed - s0.ss_fed >= n - 2, "chip fed %lu of %ld",
		s1.ss_fed - s0.ss_fed, n);
	close(fd);
	free(b);

	/* noteCmd: 440 Hz for 200 ms, then a callback */
	if (!t_check("auxsnd_note", (fd = open("/dev/snd/note", O_RDWR)) >= 0,
	    "/dev/snd/note: %s", T_ERR))
		return;
	sstat(a, &s0);
	t0 = t_now_ms();
	memset(cmd, 0, sizeof cmd);
	cmd[1] = 40;
	cmd[2] = 400 >> 8;
	cmd[3] = 400 & 255;
	cmd[5] = 331706 >> 16; cmd[6] = (331706 >> 8) & 255; cmd[7] = 331706 & 255;
	ioctl(fd, 0xC0087704, cmd);
	memset(cmd, 0, sizeof cmd);
	cmd[1] = 13;
	gotpoll = 0;
	ioctl(fd, 0xC0087704, cmd);
	while (!gotpoll && t_now_ms() - t0 < 5000)
		poll((struct pollfd *)0, 0UL, 20);
	ms = gotpoll ? t_now_ms() - t0 : -1;
	sstat(a, &s1);
	t_check("auxsnd_tone", ms >= 150 && ms <= 2000 && s1.ss_fed - s0.ss_fed >= CHIPHZ / 5 - 2,
		"200 ms note: callback after %ld ms, chip fed %lu", ms, s1.ss_fed - s0.ss_fed);
	close(fd);
	signal(SIGPOLL, SIG_DFL);
}

/* copy a file; 0 on success */
static int
cp(char *from, char *to)
{
	char b[4096];
	int i, o, n, ok = 0;

	if ((i = open(from, O_RDONLY)) < 0)
		return -1;
	if ((o = creat(to, 0644)) >= 0) {
		while ((n = read(i, b, sizeof b)) > 0)
			if (write(o, b, n) != n)
				break;
		ok = n == 0 ? 0 : -1;
		close(o);
	} else
		ok = -1;
	close(i);
	return ok;
}

/* the System 6 Folder's names with spaces, which the test root has without */
static char *sys6sp[] = { "%AUX Resources", "DA Handler", "Key Layout", "Scrapbook File", 0 };

static void
sys6links(int on)
{
	char a[96], b[96], *s, *d;
	int i;

	for (i = 0; sys6sp[i]; i++) {
		sprintf(b, "%s/%s", SYS6DIR, sys6sp[i]);
		for (s = sys6sp[i], d = a + sprintf(a, "%s/", SYS6DIR); *s; s++)
			if (*s != ' ')
				*d++ = *s;
		*d = 0;
		if (on)
			link(a, b);
		else
			unlink(b);
	}
}

/*
 * startmac with SndTest in the System Folder: SysBeep and a synchronous
 * SndPlay reach the chip, an asynchronous one returns at once and its
 * callBackCmd runs after it.  sys6: A/UX 2.0.1's System 6 in /a201.
 */
static void
macsnd(int a, int sys6)
{
	static char *av[] = { STARTMAC, 0 };
	static char *env7[] = { "PATH=/aux/bin:/usr/bin:/sbin", "HOME=/tmp",
		"TBSYSTEM=" MACSYS, "TBMEMORY=16M", "TBVERBOSE=1", "TBWARN=1", 0 };
	static char *env6[] = { "PATH=/aux/bin:/usr/bin:/sbin", "HOME=/tmp",
		"TBSYSTEM=/mac/sys/System Folder", "TBMEMORY=4M", "TBVERBOSE=1", "TBWARN=1", 0 };
	struct sndstat s;
	char line[256], tn[16], init[96], inith[96], *root = sys6 ? ROOT6 : "";
	char tf[64], tout[64], ttrace[64];
	unsigned long fed[4], tsync, tasync, tcb;
	long serr, err, t0, tpart = -1, tdone = -1;
	int cb, ui, fd, n, st, id, k, lf, nm = 0, p[2];
	off_t tsz = 0;
	struct stat sb;
	struct mod_mreg reg;
	struct mod_execreg er;
	int mj = 54;
	pid_t pid;

	sprintf(tn, "mac%s_", sys6 ? "6" : "76");
	/* A/UX programs and the Mac's device, unless an earlier test registered them */
	if (modpath(MD) == 0) {
		strcpy(reg.md_modname, "auxexec");
		reg.md_typedata = (caddr_t)&er;
		er.er_magic = 0x150;
		er.er_flags = EXF_FIRST;
		modadm(MOD_TY_EXEC, MOD_C_MREG, &reg);
		strcpy(reg.md_modname, "uinter");
		reg.md_typedata = (caddr_t)&mj;
		modadm(MOD_TY_CDEV, MOD_C_MREG, &reg);
	}
	if (!sys6 && stat(MACSYS, &sb) < 0 && stat(MACDEV, &sb) == 0)
		system("/sbin/mount -F ufs " MACDEV " /macsys");
	sprintf(line, "%s%s", root, STARTMAC);
	if (stat(line, &sb) < 0 || stat(sys6 ? ROOT6 "/etc/aux/rom" : "/etc/aux/rom", &sb) < 0 ||
	    stat(sys6 ? SYS6 "/System" : MACSYS "/System", &sb) < 0 ||
	    stat(SNDTEST "/%SndTest", &sb) < 0 || (ui = open("/dev/uinter0", O_RDWR)) < 0) {
		sprintf(line, "%sasync", tn);
		t_skip(line, "no startmac, ROM, System, uinter or SndTest");
		return;
	}
	if (sys6) {
		rename(SYS6, SYS6DIR);
		sys6links(1);
		sprintf(init, "%s/SndTest", SYS6DIR);
		sprintf(inith, "%s/%%SndTest", SYS6DIR);
	} else {
		sprintf(init, "%s/Extensions/SndTest", MACSYS);
		sprintf(inith, "%s/Extensions/%%SndTest", MACSYS);
	}
	cp(SNDTEST "/SndTest", init);
	cp(SNDTEST "/%SndTest", inith);
	sprintf(tf, "%s%s", root, TESTF);
	sprintf(tout, "%s.out", tf);
	sprintf(ttrace, "%s.trace", tf);
	fidd();
	t_rearm(240);
	unlink(tout);
	unlink(ttrace);
	close(creat(tf, 0666));
	kset("uinter_boxflag", sys6 ? 5L : 29L);
	/* as a login starts it: output to a pipe, nothing else inherited */
	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		setpgid(0, 0);	/* its own group, the console still its terminal */
		dup2(p[1], 1);
		dup2(p[1], 2);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		if (sys6 && (chroot(ROOT6) < 0 || chdir("/") < 0))
			_exit(126);
		execve(av[0], av, sys6 ? env6 : env7);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NDELAY);
	lf = creat(MACLOG, 0644);
	/* the chip's count at each of the INIT's marks, then at the end */
	n = 0;
	memset((char *)fed, 0, sizeof fed);
	for (t0 = t_now_ms(); pid > 0 && t_now_ms() - t0 < 120000; poll((struct pollfd *)0, 0UL, 50)) {
		while ((k = read(p[0], line, sizeof line)) > 0)
			write(lf, line, k);
		if (nm < 3 && stat(ttrace, &sb) == 0 && sb.st_size > tsz) {
			s.ss_fed = 0;
			sstat(a, &s);
			fed[nm++] = s.ss_fed;
			tsz = sb.st_size;
			t_info("mac_mark", "%d at %ld ms, fed %lu", nm, t_now_ms() - t0, s.ss_fed);
		}
		if ((fd = open(tout, O_RDONLY)) >= 0) {
			n = read(fd, line, sizeof line - 1);
			close(fd);
			if (n > 0 && tpart < 0)
				tpart = t_now_ms();
			if (n > 0 && line[n - 1] == '\n') {
				tdone = t_now_ms();
				break;
			}
		}
	}
	close(p[0]);
	close(lf);
	s.ss_fed = 0;
	sstat(a, &s);
	fed[3] = s.ss_fed;
	if (n <= 0) {
		psdump();
		macdump(pid);
	}
	if (pid > 0) {
		kill(-pid, SIGKILL);
		t_waitchild(pid, &st, 20);
	}
	if ((id = shmget(TLOW, 0, 0)) >= 0)
		shmctl(id, IPC_RMID, (struct shmid_ds *)0);
	kset("uinter_boxflag", -1L);
	fiddkill();
	close(ui);
	unlink(tf);
	unlink(init);
	unlink(inith);
	if (sys6) {
		sys6links(0);
		rename(SYS6DIR, SYS6);
	}
	line[n > 0 ? n : 0] = 0;
	sprintf(tn, "mac%s_", sys6 ? "6" : "76");
#define NM(x)	(sprintf(init, "%s%s", tn, x), init)
	t_info(NM("selftest"), "%s", line);
	if (!t_check(NM("selftest"), n > 0 && sscanf(line,
	    "sync_err=%ld sync_ticks=%lu err=%ld async_ticks=%lu cb=%d cb_ticks=%lu",
	    &serr, &tsync, &err, &tasync, &cb, &tcb) == 6, "no result in 120 s")) {
		maclog();
		return;
	}
	t_check(NM("beep"), nm == 3 && fed[1] - fed[0] >= CHIPHZ / 20,
		"SysBeep fed the chip %lu frames", fed[1] - fed[0]);
	t_check(NM("sync"), serr == 0 && fed[2] - fed[1] >= CHIPHZ * 9 / 20,
		"SndPlay error -%ld, %lu ticks, chip fed %lu frames", serr, tsync, fed[2] - fed[1]);
	t_check(NM("async"), err == 0 && tasync <= 6, "SndPlay of 1 s: error -%ld after %lu ticks",
		err, tasync);
	/* by the clock here: System 6 holds Ticks while INITs run */
	t_check(NM("callback"), cb && tdone - tpart >= 800 && tdone - tpart < 5000,
		"callBackCmd %s %ld ms after SndPlay (%lu ticks)", cb ? "ran" : "never ran",
		tdone - tpart, tcb);
	t_check(NM("heard"), fed[3] - fed[2] >= CHIPHZ * 9 / 10,
		"chip fed %lu frames", fed[3] - fed[2]);
#undef NM
}

static int
put(int fd, unsigned long cmd, char *p, unsigned long n)
{
	struct sndrec r;

	r.r_cmd = cmd;
	r.r_len = n;
	if (write(fd, (char *)&r, sizeof r) != sizeof r)
		return -1;
	return n && write(fd, p, n) != n ? -1 : 0;
}

/* the reply to cmd, its body into b; -1 if none in 10 s */
static int
get(int fd, unsigned long cmd, char *b, int n)
{
	struct sndrec r;
	long t0 = t_now_ms();
	int k, got = 0;

	while (got < sizeof r && t_now_ms() - t0 < 10000) {
		if ((k = read(fd, (char *)&r + got, sizeof r - got)) <= 0)
			return -1;
		got += k;
	}
	if (got < sizeof r || r.r_cmd != cmd || r.r_len != n)
		return -1;
	for (got = 0; got < n; got += k)
		if ((k = read(fd, b + got, n - got)) <= 0)
			return -1;
	return 0;
}

static int
sstat(int fd, struct sndstat *st)
{
	if (put(fd, SNDR_STAT, 0, 0) < 0)
		return -1;
	return get(fd, SNDR_STAT, (char *)st, sizeof *st);
}

/* drain; ms it took, -1 on failure */
static long
drain(int fd)
{
	long t0 = t_now_ms();

	if (put(fd, SNDR_DRAIN, 0, 0) < 0 || get(fd, SNDR_DRAIN, 0, 0) < 0)
		return -1;
	return t_now_ms() - t0;
}

static int
fmt(int fd, unsigned long rate, int enc, int chans)
{
	struct sndfmt f;

	f.f_rate = rate;
	f.f_enc = enc;
	f.f_chans = chans;
	return put(fd, SNDR_FMT, (char *)&f, sizeof f);
}

/* n frames of a 440 Hz-ish triangle at the chip's rate, 8-bit offset binary */
static int
tone(int fd, long n)
{
	char b[4096];
	long i, k;

	if (fmt(fd, ASC_RATE, SNDE_U8, 1) < 0)
		return -1;
	for (; n > 0; n -= k) {
		k = n > sizeof b ? sizeof b : n;
		for (i = 0; i < k; i++)
			b[i] = 0x80 + ((i % 50) < 25 ? (i % 25) * 4 : 100 - (i % 25) * 4) - 50;
		if (put(fd, SNDR_PCM, b, k) < 0)
			return -1;
	}
	return 0;
}

static int
sconnect(void)
{
	long t0 = t_now_ms();
	int fd, n;

	while (t_now_ms() - t0 < 5000) {
		if ((fd = open(SNDPATH, O_RDWR)) >= 0) {
			if (ioctl(fd, I_NREAD, &n) >= 0)
				return fd;
			close(fd);
		}
		sleep(1);
	}
	return -1;
}

static long
acquire(int fd, char *name)
{
	struct fbacq a;

	memset(&a, 0, sizeof a);
	a.fa_kind = FBK_USER;
	strncpy(a.fa_name, name, sizeof a.fa_name - 1);
	return ioctl(fd, FBIOACQUIRE, &a) < 0 ? -1 : (long)a.fa_id;
}

static void
stopd(void)
{
	int st;

	if (sndd > 0) {
		kill(sndd, SIGTERM);
		t_waitchild(sndd, &st, 10);
		sndd = -1;
	}
	if (sndaux > 0) {
		kill(sndaux, SIGTERM);
		t_waitchild(sndaux, &st, 10);
		sndaux = -1;
	}
}

int
main()
{
	struct sndstat s0, s1;
	long ms, k0, k1, id, sess;
	int a, c, fb, fd;
	char b[4096];

	t_init("sound", 120);
	if (access(SNDD, X_OK) < 0 || ((fd = open("/dev/asc", O_RDWR)) < 0 && errno != EBUSY)) {
		t_skip("present", "no %s or /dev/asc: %s", SNDD, T_ERR);
		return t_done();
	}
	if (fd >= 0)
		close(fd);
	if ((sndd = fork()) == 0) {
		execl(SNDD, "sndd", "-f", (char *)0);
		_exit(127);
	}
	if (!t_check("connect", (a = sconnect()) >= 0, "%s: %s", SNDPATH, T_ERR)) {
		stopd();
		return t_done();
	}
	t_check("asc_exclusive", open("/dev/asc", O_RDWR) < 0 && errno == EBUSY,
		"second open of /dev/asc: %s", T_ERR);

	/* one second through the chip */
	if (!t_check("stat", sstat(a, &s0) == 0, "no STAT reply")) {
		stopd();
		return t_done();
	}
	t_check("bound_front", s0.ss_sess == s0.ss_front, "bound to %ld, front %ld",
		s0.ss_sess, s0.ss_front);
	k0 = t_kmem("snd_ev+4");
	ms = tone(a, CHIPHZ) < 0 ? -1 : drain(a);
	k1 = t_kmem("snd_ev+4");
	t_info("play_ms", "%ld", ms);
	t_check("play_paced", ms >= 800 && ms <= 3000, "1 s of samples drained in %ld ms", ms);
	sstat(a, &s1);
	t_check("fifo_fed", s1.ss_heard - s0.ss_heard == CHIPHZ && s1.ss_fed - s0.ss_fed >= CHIPHZ,
		"heard %lu fed %lu", s1.ss_heard - s0.ss_heard, s1.ss_fed - s0.ss_fed);
	t_check("chip_irq", k0 >= 0 && k1 - k0 >= 20, "chip interrupts %ld -> %ld", k0, k1);
	t_info("underruns", "%lu", s1.ss_under);

	/* 44.1 kHz 16-bit stereo, a quarter second, resampled */
	sstat(a, &s0);
	fmt(a, 44100L << 16, SNDE_S16, 2);
	memset(b, 0, sizeof b);
	for (k0 = 44100 / 4 * 4; k0 > 0; k0 -= k1) {
		k1 = k0 > sizeof b ? sizeof b : k0;
		put(a, SNDR_PCM, b, k1);
	}
	drain(a);
	sstat(a, &s1);
	t_check("resample", s1.ss_heard - s0.ss_heard >= CHIPHZ / 4 - 2 &&
		s1.ss_heard - s0.ss_heard <= CHIPHZ / 4 + 2, "heard %lu frames for 0.25 s",
		s1.ss_heard - s0.ss_heard);

	/* flush drops what waits */
	tone(a, CHIPHZ / 2);
	put(a, SNDR_FLUSH, 0, 0);
	ms = drain(a);
	t_check("flush", ms >= 0 && ms < 400, "drain after flush took %ld ms", ms);

	/* before any display session of this test's own */
	if ((sndaux = fork()) == 0) {
		execl(SNDAUX, "sndaux", "-f", (char *)0);
		_exit(127);
	}
	auxdev(a);
	macsnd(a, 0);
	macsnd(a, 1);

	/* a session in the back: paced, never at the chip */
	if ((fb = open("/dev/fb0", O_RDWR)) < 0 || (id = acquire(fb, "sndback")) < 0) {
		t_skip("back_muted", "no display session: %s", T_ERR);
		goto out;
	}
	c = sconnect();
	sess = id;
	put(c, SNDR_BIND, (char *)&sess, sizeof sess);
	sstat(a, &s0);
	sstat(c, &s1);
	k0 = s1.ss_muted;
	ms = tone(c, CHIPHZ / 2) < 0 ? -1 : drain(c);
	sstat(c, &s1);
	t_check("back_muted", s1.ss_sess == id && s1.ss_heard == 0 &&
		s1.ss_muted - k0 == CHIPHZ / 2 && s1.ss_fed == s0.ss_fed,
		"session %ld: heard %lu muted %lu, chip fed %lu more",
		s1.ss_sess, s1.ss_heard, s1.ss_muted - k0, s1.ss_fed - s0.ss_fed);
	t_check("back_paced", ms >= 350 && ms <= 2000, "0.5 s drained in %ld ms", ms);

	/* to the front: now it is heard and the console's client is not */
	if (t_check("switch", ioctl(fb, FBIOSWITCH, id) == 0, "FBIOSWITCH: %s", T_ERR)) {
		sstat(a, &s0);
		sstat(c, &s1);
		k0 = s1.ss_heard;
		tone(c, CHIPHZ / 4);
		tone(a, CHIPHZ / 4);
		drain(c);
		drain(a);
		sstat(c, &s1);
		t_check("front_heard", s1.ss_heard - k0 == CHIPHZ / 4, "heard %lu",
			s1.ss_heard - k0);
		sstat(a, &s1);
		t_check("console_muted", s1.ss_heard == s0.ss_heard &&
			s1.ss_muted - s0.ss_muted == CHIPHZ / 4, "heard %lu muted %lu",
			s1.ss_heard - s0.ss_heard, s1.ss_muted - s0.ss_muted);
		ioctl(fb, FBIOSWITCH, 0L);
	}
	close(c);
	close(fb);
out:
	close(a);
	stopd();
	fd = open("/dev/asc", O_RDWR);
	t_check("asc_released", fd >= 0, "reopen after sndd exits: %s", T_ERR);
	if (fd >= 0)
		close(fd);
	return t_done();
}
