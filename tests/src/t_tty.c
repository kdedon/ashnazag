/*
 * t_tty.c -- console terminal: isatty, termio/termios round trips, modules.
 * Only reads modes, or writes back exactly what was read.
 */
#include <sys/types.h>
#include <sys/termio.h>
#include <sys/termios.h>
#include <sys/stropts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

int
main()
{
	struct termio a, b;
	struct termios s, s2;
	struct str_list sl;
	struct k_mlist ml[8];
	char *n;
	int fd, k, i, found;

	t_init("tty", 25);
	fd = open("/dev/console", O_RDWR | O_NOCTTY);
	if (fd < 0) {
		t_fail("open_console", "%s", T_ERR);
		return t_done();
	}
	t_pass("open_console");
	t_check("isatty_console", isatty(fd) == 1, "isatty %d", isatty(fd));
	k = open("/dev/null", O_RDONLY);
	t_check("isatty_null", isatty(k) == 0, "isatty(/dev/null) true");
	close(k);
	t_info("isatty_stdout", "%d", isatty(1));
	n = ttyname(fd);
	t_info("ttyname", "%s", n ? n : "(null)");

	k = ioctl(fd, TCGETA, &a);
	t_check("TCGETA", k == 0, "%s", T_ERR);
	t_info("termio", "iflag 0%o oflag 0%o cflag 0%o lflag 0%o erase %d intr %d", a.c_iflag,
	    a.c_oflag, a.c_cflag, a.c_lflag, a.c_cc[VERASE], a.c_cc[VINTR]);
	k = ioctl(fd, TCSETA, &a);
	memset(&b, 0x55, sizeof b);
	ioctl(fd, TCGETA, &b);
	t_check("TCSETA_roundtrip", k == 0 && a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag &&
	    a.c_cflag == b.c_cflag && a.c_lflag == b.c_lflag && memcmp(a.c_cc, b.c_cc, NCC) == 0,
	    "ioctl %d, modes changed", k);

	k = ioctl(fd, TCGETS, &s);
	t_check("TCGETS", k == 0 && (s.c_lflag & 0xffff) == a.c_lflag && (s.c_cflag & 0xffff) == a.c_cflag,
	    "ioctl %d, termios/termio differ", k);
	k = tcgetattr(fd, &s2);
	t_check("tcgetattr", k == 0 && memcmp(&s, &s2, sizeof s) == 0, "differs from TCGETS");
	k = tcsetattr(fd, TCSANOW, &s);
	tcgetattr(fd, &s2);
	t_check("tcsetattr_roundtrip", k == 0 && s.c_lflag == s2.c_lflag && s.c_iflag == s2.c_iflag,
	    "%s", T_ERR);
	t_info("speed", "out %ld in %ld", (long)cfgetospeed(&s), (long)cfgetispeed(&s));

	/* ldterm must have been autopushed on the console */
	k = ioctl(fd, I_FIND, "ldterm");
	t_check("I_FIND_ldterm", k == 1, "I_FIND %d (%s)", k, k < 0 ? T_ERR : "absent");
	k = ioctl(fd, I_LIST, (char *)0);
	sl.sl_nmods = 8;
	sl.sl_modlist = (struct str_mlist *)ml;
	if (ioctl(fd, I_LIST, &sl) >= 0) {
		char buf[128];
		buf[0] = 0;
		found = 0;
		for (i = 0; i < sl.sl_nmods; i++) {
			strcat(buf, ml[i].l_name);
			strcat(buf, " ");
			if (strcmp(ml[i].l_name, "ldterm") == 0)
				found = 1;
		}
		t_info("I_LIST", "%d entries: %s", k, buf);
		t_check("I_LIST_ldterm", found, "stack: %s", buf);
	} else
		t_fail("I_LIST", "%s", T_ERR);

	k = open("/dev/tty", O_RDWR);
	t_info("dev_tty", "%s", k >= 0 ? "controlling tty present" : strerror(errno));
	if (k >= 0)
		close(k);
	close(fd);
	return t_done();
}
