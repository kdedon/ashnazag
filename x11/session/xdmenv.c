/*
 * xdmenv -- start an environment full screen from an X session.
 *
 *	xdmenv mac|tos|amiga
 *
 * Under xdm the screen belongs to root's X server and the user has no
 * console terminal, so the display service would refuse the user's
 * session the front.  Installed setuid root: for the user xdm logged
 * in on the screen (named in CONSUSER by xdm's startup script), it runs
 * the launcher as that user with the console as controlling terminal,
 * then brings the X server back to the front.  Anyone else just runs
 * the launcher with their own rights.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <termios.h>
#include "dsio.h"

#define CONSUSER "/usr/x11r6/lib/X11/xdm/console-user"

static char *envs[][2] = {
	{ "mac", "/usr/bin/startmac" },
	{ "tos", "/usr/bin/starttos" },
	{ "amiga", "/usr/bin/startmig" },
	{ 0, 0 }
};

/* the user xdm logged in on the screen; the file must be root's */
static int
consuser(name)
	char *name;
{
	char buf[64];
	struct stat st;
	int fd, n;

	if ((fd = open(CONSUSER, O_RDONLY)) < 0)
		return 0;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
	    (st.st_mode & 022) || st.st_nlink != 1 ||
	    (n = read(fd, buf, sizeof buf - 1)) <= 0) {
		close(fd);
		return 0;
	}
	close(fd);
	buf[n] = 0;
	if (buf[n - 1] == '\n')
		buf[n - 1] = 0;
	return strcmp(buf, name) == 0;
}

static void
drop(pw)
	struct passwd *pw;
{
	if (setgid(pw->pw_gid) < 0 || initgroups(pw->pw_name, pw->pw_gid) < 0 ||
	    setuid(pw->pw_uid) < 0 || geteuid() != pw->pw_uid ||
	    getegid() != pw->pw_gid) {
		perror("xdmenv");
		exit(1);
	}
}

/* back to the caller's own rights and groups */
static void
unset()
{
	if (setgid(getgid()) < 0 || setuid(getuid()) < 0 ||
	    geteuid() != getuid() || getegid() != getgid()) {
		perror("xdmenv");
		exit(1);
	}
}

/* the launcher's whole environment */
static char **
newenv(pw)
	struct passwd *pw;
{
	static char home[1100], user[80], logname[80], shell[1100], tz[80];
	static char *env[] = { home, user, logname, shell,
		"PATH=/usr/bin:/usr/sbin:/usr/ucb:/usr/x11r6/bin", "TERM=vt100",
		tz, 0 };
	char *z;

	sprintf(home, "HOME=%.1000s", pw->pw_dir);
	sprintf(user, "USER=%.60s", pw->pw_name);
	sprintf(logname, "LOGNAME=%.60s", pw->pw_name);
	sprintf(shell, "SHELL=%.1000s", *pw->pw_shell ? pw->pw_shell : "/bin/sh");
	/* TZ only as a zone name */
	if ((z = getenv("TZ")) != 0 && strlen(z) < 60 && !strchr(z, '/') &&
	    !strchr(z, '.'))
		sprintf(tz, "TZ=%s", z);
	else
		env[6] = 0;
	return env;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct passwd *pw;
	struct fbstate st;
	struct termios tio;
	char *cmd = 0;
	long x;
	int i, fd, pid, status;

	for (i = 0; argc == 2 && envs[i][0]; i++)
		if (strcmp(argv[1], envs[i][0]) == 0)
			cmd = envs[i][1];
	if (cmd == 0) {
		fprintf(stderr, "usage: xdmenv mac|tos|amiga\n");
		return 2;
	}
	if ((pw = getpwuid(getuid())) == 0) {
		fprintf(stderr, "xdmenv: unknown user\n");
		return 1;
	}
	if (access(cmd, X_OK) < 0) {
		fprintf(stderr, "xdmenv: %s is not installed\n", cmd);
		return 1;
	}
	if (geteuid() != 0 || !consuser(pw->pw_name)) {
		unset();
		execl(cmd, cmd, (char *)0);
		perror(cmd);
		return 127;
	}

	/* the X server's session, brought back when the environment ends */
	x = -1;
	if ((fd = open("/dev/fb0", O_RDONLY)) >= 0) {
		if (ioctl(fd, FBIOGSTATE, &st) == 0)
			x = st.st_front;
		fcntl(fd, F_SETFD, 1);
	}
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	if ((pid = fork()) < 0) {
		perror("xdmenv: fork");
		return 1;
	}
	if (pid == 0) {
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		setsid();
		for (i = 0; i < 20; i++)
			close(i);
		/* the first terminal a session leader opens becomes its own */
		if (open("/dev/console", O_RDWR) != 0)
			_exit(1);
		dup(0);
		dup(0);
		/* a fresh open gets the line discipline's defaults (DEL
		   interrupts); restore the console's usual keys */
		if (tcgetattr(0, &tio) == 0) {
			tio.c_lflag |= ISIG;
			tio.c_cc[VINTR] = 3;
			tio.c_cc[VQUIT] = 034;
			tio.c_cc[VERASE] = 010;
			tio.c_cc[VKILL] = 025;
			tcsetattr(0, TCSANOW, &tio);
		}
		drop(pw);
		chdir(pw->pw_dir);
		execle(cmd, cmd, (char *)0, newenv(pw));
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return 1;
	if (fd >= 0 && x > 0)
		ioctl(fd, FBIOSWITCH, x);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
