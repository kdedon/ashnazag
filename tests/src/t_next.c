/*
 * t_next: the nextguest module (registered, loaded, opened), then the
 * user's kernel from the OPENSTEP CD on SCSI disk 6 (tests/next/mkvol.sh),
 * loaded and entered by the native host without a ROM: it must print its
 * version line on the host console, stop at its missing root by returning to
 * the monitor (a host call), and reach the board's id, interrupt and
 * timer registers.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include "next/profile.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	DEV	"/dev/dsk/c6d0s0"
#define	MNT	"/next"
#define	ROOT	"/tmp/next"
#define	LOG	"/tmp/next.log"
#define	SCR1	0x0200c000UL
#define	KBASE	0x04000000UL

static int dev = -1;

/* the host's complaints, if any */
static void
logtail()
{
	static char buf[32768];
	char *p, *e;
	int fd, n, k = 0;

	if ((fd = open(LOG, O_RDONLY)) < 0)
		return;
	if ((n = read(fd, buf, sizeof buf - 1)) > 0) {
		buf[n] = 0;
		for (p = buf; *p && k < 80; p = e) {
			e = strchr(p, 10);
			if (e)
				*e++ = 0;
			else
				e = p + strlen(p);
			if (!strstr(p, "census")) {
				t_info("log", "%s", p);
				k++;
			}
		}
	}
	close(fd);
}

/* whether the host console log holds the text */
static int
logfind(text)
	char *text;
{
	static char buf[16384];
	int fd, n = 0;

	if ((fd = open(LOG, O_RDONLY)) >= 0) {
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
	}
	if (n <= 0)
		return 0;
	buf[n] = 0;
	return strstr(buf, text) != 0;
}

/* the kernel loaded and entered without the ROM: it runs and probes the board */
static void
kernel()
{
	static struct nx_info info, now;
	pid_t pid;
	int i, gone = 0, status, id = 0, ints = 0, banner;

	if (access(DEV, 0) != 0 || system("/sbin/mount -F ufs -r " DEV " " MNT) != 0) {
		t_skip("kernel", "no NeXT volume");
		return;
	}
	if (access(MNT "/native", X_OK) != 0 || access(MNT "/mach_kernel", R_OK) != 0) {
		t_skip("kernel", "no native or kernel on the volume");
		system("/sbin/umount " MNT);
		return;
	}
	mkdir(ROOT, 0700);
	system("cp -r " MNT "/fs/etc " ROOT "/etc; cp " MNT "/fs/.meta " ROOT ".meta");
	unlink(ROOT "/nvram");
	unlink(LOG);
	if ((pid = fork()) == 0) {
		int fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		setpgrp();
		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fd, 1);
		dup2(fd, 2);
		putenv("NATIVEHOST_ROOT=" ROOT);
		putenv("NEXT_CENSUS=1");
		execl(MNT "/native", "native", "-k", MNT "/mach_kernel", (char *)0);
		_exit(127);
	}
	/* the module forgets the machine when it ends: keep the last look */
	for (i = 0; i < 200 && !gone; i++) {
		poll((struct pollfd *)0, 0, 100);
		if (ioctl(dev, NX_INFO, &now) == 0 && now.ncensus >= info.ncensus)
			info = now;
		gone = waitpid(pid, &status, WNOHANG) != 0;
		if (gone) t_info("exit", "status %x", status);
	}
	logtail();
	t_info("kernel", "gone %d ticks %lu io %lu last %lx pc %lx", gone, info.ticks, info.io,
	    info.lasta, info.lastpc);
	for (i = 0; i < (int)info.ncensus && i < NX_NCENSUS; i++) {
		struct nx_census *c = &info.census[i];

		if (c->pc < KBASE || c->pc >= KBASE + 0x1000000UL)
			continue;
		id |= c->addr == SCR1 && !c->write;
		ints |= c->addr == 0x02007800UL || c->addr == 0x02007000UL;
	}
	banner = logfind("NeXT Mach ");
	t_check("kernel_banner", banner, "no version line from the kernel's console");
	t_check("root_installed", logfind("next: root filesystem installed"), "the host filesystem was not installed");
	t_check("root_not_network", !logfind("no suitable root") && !logfind("signal"), "the root probe or a signal stopped the kernel");
	if (logfind("root mounted from the host folder"))
		t_info("root", "mounted");
	t_check("kernel_loaded", info.ncensus > 0, "no board access");
	t_check("reads_id", id, "no read of the machine id from the kernel");
	t_check("interrupt_regs", ints, "no interrupt register access from the kernel");
	if (!gone)
		kill(-pid, SIGTERM);
	waitpid(pid, &status, 0);
	for (i = 0; i < 100 && kill(-pid, 0) == 0; i++)
		poll((struct pollfd *)0, 0, 100);
	system("/sbin/umount " MNT);
	unlink(ROOT "/vram");
	unlink(ROOT "/nvram");
	system("rm -rf " ROOT " " ROOT ".meta");
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
	int mj = NX_MAJOR, id;

	t_init("next", 600);
	if (access(MD "/nextguest", 0) < 0 || access("/dev/nextguest", 0) < 0) {
		t_skip("module", "no nextguest module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "nextguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "%s", T_ERR))
		return t_done();
	dev = open("/dev/nextguest", O_RDWR);
	if (!t_check("open_autoload", dev >= 0, "%s", T_ERR))
		return t_done();
	kernel();
	close(dev);
	id = modid("nextguest");
	t_check("loaded", id > 0, "not in modstat");
	if (id > 0)
		t_check("unload", moduload(id) == 0, "%s", T_ERR);
	return t_done();
}
