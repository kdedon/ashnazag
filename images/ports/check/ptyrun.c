/*
 * ptyrun -- run a program on a pty and type keys at it.
 *
 *   ptyrun keys prog [arg...]
 *
 * keys: \r \e \\ and \ooo escapes; ~ waits a second.  The program's
 * output is copied to stdout.  Exits with the program's status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stropts.h>
#include <sys/types.h>
#include <sys/wait.h>

static int m;

static void
drain(void)
{
	char b[512];
	int n;

	while ((n = read(m, b, sizeof b)) > 0)
		fwrite(b, 1, n, stdout);
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	char *k, *sn, c;
	int s, st, i;
	pid_t pid;

	if (argc < 3) {
		fprintf(stderr, "usage: ptyrun keys prog [arg...]\n");
		return 2;
	}
	if ((m = open("/dev/ptmx", O_RDWR)) < 0) {
		perror("ptmx");
		return 2;
	}
	pid = getpid();
	grantpt(m);
	if (getpid() != pid)	/* grantpt's child, without pt_chmod */
		_exit(0);
	unlockpt(m);
	sn = ptsname(m);
	if ((pid = fork()) == 0) {
		setsid();
		if ((s = open(sn, O_RDWR)) < 0) {
			perror(sn);
			_exit(127);
		}
		ioctl(s, I_PUSH, "ptem");
		ioctl(s, I_PUSH, "ldterm");
		ioctl(s, I_PUSH, "ttcompat");
		dup2(s, 0); dup2(s, 1); dup2(s, 2);
		close(s); close(m);
		putenv("TERM=vt100");
		putenv("LINES=24");
		putenv("COLUMNS=80");
		execvp(argv[2], argv + 2);
		_exit(127);
	}
	fcntl(m, F_SETFL, O_NDELAY);
	for (k = argv[1]; *k; k++) {
		drain();
		if (*k == '~') {
			sleep(1);
			continue;
		}
		c = *k;
		if (c == '\\' && k[1]) {
			c = *++k;
			if (c == 'r')
				c = '\r';
			else if (c == 'e')
				c = 033;
			else if (c >= '0' && c <= '7') {
				c -= '0';
				for (i = 0; i < 2 && k[1] >= '0' && k[1] <= '7'; i++)
					c = c * 8 + *++k - '0';
			}
		}
		write(m, &c, 1);
	}
	for (i = 0; i < 20; i++) {
		drain();
		if (waitpid(pid, &st, WNOHANG) == pid) {
			drain();
			return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
		}
		sleep(1);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	return 124;
}
