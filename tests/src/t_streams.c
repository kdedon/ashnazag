/*
 * t_streams.c -- memory devices, autopush, module push/pop, pseudo-terminals.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/stropts.h>
#include <sys/conf.h>
#include <sys/sad.h>
#include <sys/termio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

extern char *ptsname();

static void
test_mem_devices()
{
	char buf[8192];
	int fd, k, i, bad = 0;

	fd = open("/dev/null", O_RDWR);
	t_check("null_write", write(fd, buf, sizeof buf) == sizeof buf, "%s", T_ERR);
	t_check("null_read_eof", read(fd, buf, sizeof buf) == 0, "read not 0");
	close(fd);
	fd = open("/dev/zero", O_RDONLY);
	memset(buf, 1, sizeof buf);
	k = read(fd, buf, sizeof buf);
	for (i = 0; i < k; i++)
		if (buf[i])
			bad++;
	t_check("zero_read", k == sizeof buf && bad == 0, "read %d, %d nonzero", k, bad);
	close(fd);
}

static void
test_sad()
{
	struct strapush ap;
	struct str_list sl;
	struct k_mlist ml[2];
	int fd, i, found = 0;
	char list[80];

	fd = open("/dev/sad/user", O_RDWR);
	if (fd < 0) {
		t_fail("sad_open", "%s", T_ERR);
		return;
	}
	t_pass("sad_open");
	memset(&ap, 0, sizeof ap);
	ap.sap_major = 0;
	ap.sap_minor = 0;
	if (ioctl(fd, SAD_GAP, &ap) == -1)
		t_fail("sad_gap_console", "%s", T_ERR);
	else {
		list[0] = 0;
		for (i = 0; i < ap.sap_npush && i < MAXAPUSH; i++) {
			strcat(list, ap.sap_list[i]);
			strcat(list, " ");
			if (strcmp(ap.sap_list[i], "ldterm") == 0)
				found = 1;
		}
		t_check("sad_gap_console", found, "autopush list: %s", list);
	}
	strcpy(ml[0].l_name, "ldterm");
	strcpy(ml[1].l_name, "ttcompat");
	sl.sl_nmods = 2;
	sl.sl_modlist = (struct str_mlist *)ml;
	t_check("sad_vml_valid", ioctl(fd, SAD_VML, &sl) == 0, "ioctl: %s", T_ERR);
	strcpy(ml[1].l_name, "nosuchmod");
	t_check("sad_vml_invalid", ioctl(fd, SAD_VML, &sl) == 1, "bogus module accepted");
	close(fd);
}

static void
test_push_pop()
{
	char name[FMNAMESZ + 1];
	int fd, p[2];

	fd = open("/dev/ticlts", O_RDWR);
	if (fd < 0) {
		t_skip("clone_ticlts", "open: %s", T_ERR);
	} else {
		t_pass("clone_ticlts");
		t_check("I_PUSH_timod", ioctl(fd, I_PUSH, "timod") == 0, "%s", T_ERR);
		t_check("I_LOOK_timod", ioctl(fd, I_LOOK, name) == 0 && strcmp(name, "timod") == 0,
		    "I_LOOK '%s'", name);
		t_check("I_POP", ioctl(fd, I_POP, 0) == 0 && ioctl(fd, I_LOOK, name) == -1,
		    "module still present");
		close(fd);
	}
	pipe(p);
	t_check("I_PUSH_bogus", ioctl(p[0], I_PUSH, "nosuchmod") == -1 && errno == EINVAL, "errno %d", errno);
	close(p[0]);
	close(p[1]);
}

static void
test_pty()
{
	struct termio t;
	char *sname, buf[64];
	int m, s, k;
	pid_t pid;

	m = open("/dev/ptmx", O_RDWR);
	if (m < 0) {
		t_skip("pty", "open /dev/ptmx: %s", T_ERR);
		return;
	}
	t_pass("ptmx_open");
	/* without /usr/lib/pt_chmod, grantpt's child returns from grantpt too */
	pid = getpid();
	k = grantpt(m);
	if (getpid() != pid)
		_exit(1);
	t_info("grantpt", "%d%s%s", k, k ? " " : "", k ? strerror(errno) : "");
	t_check("unlockpt", unlockpt(m) == 0, "%s", T_ERR);
	sname = ptsname(m);
	t_check("ptsname", sname != 0 && strncmp(sname, "/dev/pts/", 9) == 0, "'%s'", sname ? sname : "(null)");
	if (sname == 0)
		goto out;
	s = open(sname, O_RDWR | O_NOCTTY);
	if (s < 0) {
		t_fail("pts_open", "%s: %s", sname, T_ERR);
		goto out;
	}
	t_pass("pts_open");
	t_check("pts_push", ioctl(s, I_PUSH, "ptem") == 0 && ioctl(s, I_PUSH, "ldterm") == 0 &&
	    ioctl(s, I_PUSH, "ttcompat") == 0, "%s", T_ERR);
	t_check("pts_isatty", isatty(s) && ioctl(s, TCGETA, &t) == 0, "%s", T_ERR);

	t.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
	ioctl(s, TCSETA, &t);
	write(m, "hello\n", 6);
	t_rearm(10);
	k = read(s, buf, sizeof buf);
	t_check("pty_master_to_slave", k == 6 && memcmp(buf, "hello\n", 6) == 0, "read %d", k);
	write(s, "abc\n", 4);
	{
		struct pollfd pf;
		pf.fd = m;
		pf.events = POLLIN;
		k = poll(&pf, 1, 3000);
	}
	k = k == 1 ? read(m, buf, sizeof buf) : -1;
	t_check("pty_slave_to_master", k >= 4 && memcmp(buf, "abc", 3) == 0, "read %d", k);
	t_rearm(40);
	close(s);
out:
	close(m);
}

int
main()
{
	t_init("streams", 40);
	test_mem_devices();
	test_sad();
	test_push_pop();
	test_pty();
	return t_done();
}
