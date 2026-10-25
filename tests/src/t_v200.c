/*
 * t_v200: the tiplatform module (registered, loaded, opened), then AMS
 * from the user's Flash dump on SCSI disk 4 (tests/v200/mkvol.sh):
 * boot from reset to the Home screen, 12+34 typed on the host keyboard
 * gives 46, OFF blanks the LCD and ON restores it, the archived variable
 * is a host file that survives a restart, no hardware-page fault on the way.  The LCD is read from the calculator's
 * RAM file; screen dumps of each step go to the display results.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/termio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include "profile.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	DEV	"/dev/dsk/c4d0s0"
#define	MNT	"/v200"
#define	ROOT	"/tmp/v200"
#define	LOG	"/tmp/v200.log"
#define	ARC	ROOT "/archive/a.v2e"
#define	TOP	20		/* LCD rows hashed: below the clock, above the entry line's cursor */
#define	BOTTOM	109

/*
 * The LCD's CRC between TOP and BOTTOM, per Flash dump: the desktop
 * after reset, and Home after 12+34.
 */
static struct { unsigned long image, home, sum; } known[] = {
	{ 0xf805c7a6UL, 0xda5c4b31UL, 0x56ac31c9UL },	/* AMS 2.09 */
	{ 0xed4cbfd2UL, 0xda5c4b31UL, 0x56ac31c9UL },	/* AMS 3.10 */
	{ 0, 0, 0 }
};

static int hfd = -1, hseq;
static char hrep[128];

/* a host request (tests/display/hostio.py); the answer, 0 if none in 20 s */
static char *
host(req)
	char *req;
{
	char buf[160], line[256], *s;
	struct pollfd p;
	int n, len = 0, seq;
	long t0;

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	t0 = t_now_ms();
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		if (poll(&p, 1, 500) <= 0 || (n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		if ((s = strstr(line, "@@ok ")) != 0 && strchr(s, '\n') &&
		    sscanf(s + 5, "%d", &seq) == 1 && seq == hseq) {
			s = strchr(s + 5, ' ');
			strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
			hrep[strcspn(hrep, "\r\n")] = 0;
			return hrep;
		}
		if (strchr(line, '\n') && strstr(line, "@@err "))
			return 0;
		if (strchr(line, '\n'))
			len = 0;
	}
	return 0;
}

static void
hostopen()
{
	struct termio t;

	if ((hfd = open("/dev/term/b", O_RDWR | O_NOCTTY)) < 0)
		return;
	if (ioctl(hfd, TCGETA, &t) == 0) {
		t.c_iflag = IGNCR;
		t.c_oflag = 0;
		t.c_lflag = ICANON;
		t.c_cflag |= CREAD | CLOCAL;
		ioctl(hfd, TCSETAF, &t);
	}
	if (host("ping") == 0 || strcmp(hrep, "pong") != 0) {
		close(hfd);
		hfd = -1;
	}
}

static unsigned long
crc(p, n, c)
	unsigned char *p;
	long n;
	unsigned long c;
{
	int b;

	c = ~c;
	while (n-- > 0) {
		c ^= *p++;
		for (b = 0; b < 8; b++)
			c = c >> 1 ^ (c & 1 ? 0xedb88320UL : 0);
	}
	return ~c;
}

static unsigned long
filecrc(path)
	char *path;
{
	static unsigned char buf[8192];
	unsigned long c = 0;
	int fd, n;

	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	while ((n = read(fd, buf, sizeof buf)) > 0)
		c = crc(buf, (long)n, c);
	close(fd);
	return c;
}

static int dev = -1;
static pid_t pid;		/* the calculator */

extern int getksym();

/* a kernel long through /dev/kmem, -1 if unreadable */
static long
getsym(name)
	char *name;
{
	unsigned long a = 0, i;
	long v;
	int fd, ok;

	if (getksym(name, &a, &i) < 0 || (fd = open("/dev/kmem", O_RDONLY)) < 0)
		return -1;
	ok = lseek(fd, (off_t)a, 0) != -1 && read(fd, (char *)&v, 4) == 4;
	close(fd);
	return ok ? v : -1;
}

static struct ti_info info;

/* the LCD from the RAM file: 1, or 0 if the calculator is not running */
static int
lcd(buf)
	unsigned char *buf;
{
	int fd, ok;

	if (ioctl(dev, TI_INFO, &info) < 0 || (fd = open(ROOT "/ram", O_RDONLY)) < 0)
		return 0;
	ok = lseek(fd, (long)info.lcd, 0) == (long)info.lcd && read(fd, buf, 3840) == 3840;
	close(fd);
	return ok;
}

/* the LCD's CRC once it and Flash hold still for 3 s, 0 if never within secs */
static unsigned long
settle(secs)
	int secs;
{
	unsigned char buf[3840];
	unsigned long c, last = 0, flash = 0;
	int i, same = 0, lit;

	for (i = 0; i < secs; i++) {
		sleep(1);
		if (waitpid(pid, (int *)0, WNOHANG) != 0)
			return 0;
		if (!lcd(buf))
			continue;
		for (lit = 0, c = 30 * TOP; c < 30 * BOTTOM; c++)
			lit |= buf[c];
		c = crc(buf + 30 * TOP, 30L * (BOTTOM - TOP), 0UL);
		if (lit && c == last && info.flash == flash) {
			if (++same == 3)
				return c;
		} else
			same = 0;
		last = c;
		flash = info.flash;
	}
	return 0;
}

static void
shot(name)
	char *name;
{
	char req[64];

	sprintf(req, "shot %s", name);
	host(req);
}

/* the host's complaints, if any */
static void
logtail()
{
	char buf[200];
	int fd, n;

	if ((fd = open(LOG, O_RDONLY)) < 0)
		return;
	if ((n = read(fd, buf, sizeof buf - 1)) > 0) {
		buf[n] = 0;
		buf[strcspn(buf, "\n")] = 0;
		t_info("log", "%s", buf);
	}
	close(fd);
}

/* a TI-92 Plus / Voyage 200 variable file */
static int
sig(path)
	char *path;
{
	char buf[8];
	int fd, ok;

	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	ok = read(fd, buf, 8) == 8 && memcmp(buf, "**TI92P*", 8) == 0;
	close(fd);
	return ok;
}

/* the calculator on the volume's dump, keeping the archive files of the last run */
static pid_t
start()
{
	pid_t pid;
	int fd;

	unlink(LOG);
	if ((pid = fork()) == 0) {
		fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		setpgrp();
		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fd, 1);
		dup2(fd, 2);
		putenv("NATIVEHOST_ROOT=" ROOT);
		execl(MNT "/native", "native", MNT "/media", (char *)0);
		_exit(127);
	}
	return pid;
}

/* the calculator and its display process, gone before the next start */
static void
stop(pid)
	pid_t pid;
{
	int i, status;

	kill(-pid, SIGTERM);
	waitpid(pid, &status, 0);
	for (i = 0; i < 100 && kill(-pid, 0) == 0; i++)
		poll((struct pollfd *)0, 0, 100);
}

static void
type(keys)
	char **keys;
{
	char req[32];

	for (; *keys; keys++) {
		sprintf(req, "key %s", *keys);
		host(req);
		poll((struct pollfd *)0, 0, 200);
	}
}

static char *sum[] = { "ret", "1", "2", "kp_add", "3", "4", "ret", 0 };
/* 5 STO a, then Archive a */
static char *store[] = { "5", "apostrophe", "a", "ret",
	"a", "r", "c", "h", "i", "v", "e", "spc", "a", "ret", 0 };
/* Clear Home (F1 8), then a */
static char *recall[] = { "f1", "8", "a", "ret", 0 };
static char *explore[][3] = {
	{ "f10", "v200_mode", "esc" }, { "insert", "v200_catalog", "esc" },
	{ "f2", "v200_f2", "esc" }, { "f9", "v200_apps", 0 },
	{ "home", "v200_homekey", 0 }, { "end", "v200_off", 0 }, { "f12", "v200_on", 0 },
	{ 0, 0, 0 }
};

static void
calculator()
{
	long npriv0, bad0;
	unsigned long image, home, sum1, kept, recall1, writes;
	struct stat st;
	char req[32];
	int i, k, status, alive;

	if (access(DEV, 0) != 0 || system("/sbin/mount -F ufs -r " DEV " " MNT) != 0) {
		t_skip("calculator", "no Voyage 200 volume");
		return;
	}
	if (access(MNT "/native", X_OK) != 0 || access(MNT "/media", R_OK) != 0) {
		t_skip("calculator", "no native or Flash dump on the volume");
		system("/sbin/umount " MNT);
		return;
	}
	image = filecrc(MNT "/media");
	for (k = 0; known[k].image && known[k].image != image; k++)
		;
	t_info("image", "crc %08lx%s", image, known[k].image ? "" : ", no stored screens");
	hostopen();
	mkdir(ROOT, 0700);
	unlink(ARC);
	rmdir(ROOT "/archive");
	pid = start();
	home = settle(120);
	npriv0 = getsym("guest_npriv");
	bad0 = getsym("guest_nprivbad");
	shot("v200_home");
	t_info("first_boot", "flash writes %lu", info.flash);
	if (t_check("home", home != 0, "LCD never settled") && known[k].image)
		t_check("home_screen", home == known[k].home, "LCD crc %08lx", home);
	else if (home)
		t_info("home_crc", "%08lx", home);
	if (home) {
		long b = t_busy(30000L);

		t_info("idle_busy", "%ld.%ld%%", b / 10, b % 10);
	}
	if (!home || hfd < 0) {
		logtail();
		t_skip("sum", "no Home screen or no host keyboard");
		stop(pid);
		system("/sbin/umount " MNT);
		return;
	}
	type(sum);
	sum1 = settle(60);
	shot("v200_sum");
	if (known[k].image)
		t_check("sum_46", sum1 == known[k].sum, "LCD crc %08lx", sum1);
	else if (t_check("sum", sum1 != 0 && sum1 != home, "LCD crc %08lx", sum1))
		t_info("sum_crc", "%08lx", sum1);
	type(store);
	settle(60);
	shot("v200_archive");
	type(recall);
	recall1 = settle(60);
	shot("v200_recall1");
	for (i = 0; explore[i][0]; i++) {
		sprintf(req, "key %s", explore[i][0]);
		host(req);
		settle(20);
		shot(explore[i][1]);
		t_info(explore[i][1], "flash %lu", info.flash);
		if (strcmp(explore[i][1], "v200_off") == 0)
			t_check("off_dark", info.dark != 0, "LCD still on");
		else if (strcmp(explore[i][1], "v200_on") == 0)
			t_check("on_lit", info.dark == 0, "LCD still off");
		if (!explore[i][2])
			continue;
		sprintf(req, "key %s", explore[i][2]);
		host(req);
		settle(20);
	}
	alive = waitpid(pid, &status, WNOHANG) == 0;
	logtail();
	memset(&info, 0, sizeof info);
	memset(&st, 0, sizeof st);
	ioctl(dev, TI_INFO, &info);
	t_info("counts", "ticks %lu interrupts %lu io %lu flash %lu", info.ticks, info.ints,
	    info.io, info.flash);
	t_check("no_hw_faults", info.io == 0, "%lu hardware-page faults", info.io);
	t_info("privileged_emulated", "%ld", getsym("guest_npriv") - npriv0);
	t_check("no_refused_insns", bad0 < 0 || getsym("guest_nprivbad") == bad0,
	    "%ld refused instructions", getsym("guest_nprivbad") - bad0);
	t_check("fault_free", alive && info.faults == 0 && info.ints > 0 &&
	    stat(LOG, &st) == 0 && st.st_size == 0,
	    "alive %d faults %lu log %ld bytes", alive, info.faults, (long)st.st_size);
	stop(pid);
	t_check("archive_file", stat(ARC, &st) == 0 && st.st_size > 0x58 && sig(ARC), "no %s", ARC);

	/* again from the dump: the archived variable comes back from its file */
	pid = start();
	settle(120);
	writes = info.flash;
	shot("v200_reboot");
	t_info("second_boot", "flash writes %lu", writes);
	type(sum);
	type(recall);
	kept = settle(60);
	shot("v200_recall2");
	t_check("archive_kept", kept != 0 && kept == recall1, "LCD crc %08lx, before %08lx",
	    kept, recall1);
	t_check("no_hw_faults_again", info.io == 0, "%lu hardware-page faults", info.io);
	stop(pid);
	t_check("archive_file_again", stat(ARC, &st) == 0, "no %s", ARC);
	system("/sbin/umount " MNT);
	unlink(ARC);
	rmdir(ROOT "/archive");
	unlink(ROOT "/ram");
	rmdir(ROOT);
	unlink(LOG);
}

static int
modid(name)
	char *name;
{
	struct modstatus st;
	int id = 1;

	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, name) == 0)
			return st.ms_id;
		id = st.ms_id + 1;
	}
	return -1;
}

int
main()
{
	struct mod_mreg reg;
	int mj = TI_MAJOR, id;

	t_init("v200", 900);
	if (access(MD "/tiplatform", 0) < 0 || access("/dev/tiplatform", 0) < 0) {
		t_skip("module", "no tiplatform module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "tiplatform");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "%s", T_ERR))
		return t_done();
	dev = open("/dev/tiplatform", O_RDWR);
	if (!t_check("open_autoload", dev >= 0, "%s", T_ERR))
		return t_done();
	calculator();
	close(dev);
	id = modid("tiplatform");
	t_check("loaded", id > 0, "not in modstat");
	if (id > 0)
		t_check("unload", moduload(id) == 0, "%s", T_ERR);
	return t_done();
}
