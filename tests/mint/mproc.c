/*
 * mproc -- process calls.  "exit N" exits with N; "pipe" sends a line
 * from a forked child; "sig" catches SIGUSR1 and reaps a child killed by
 * SIGTERM; "exec PRG" runs PRG and prints its exit status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile int got;

static void
h(int s)
{
	got = s;
}

int
main(int argc, char **argv)
{
	char buf[64];
	int p[2], st, n;
	pid_t pid;

	if (argc == 3 && strcmp(argv[1], "exit") == 0)
		return atoi(argv[2]);
	if (argc == 2 && strcmp(argv[1], "pipe") == 0) {
		if (pipe(p) < 0 || (pid = fork()) < 0)
			return 1;
		if (pid == 0) {
			close(p[0]);
			write(p[1], "through the pipe\n", 17);
			_exit(7);
		}
		close(p[1]);
		n = read(p[0], buf, sizeof buf - 1);
		buf[n > 0 ? n : 0] = 0;
		waitpid(pid, &st, 0);
		printf("read %s", buf);
		printf("child %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "sig") == 0) {
		struct sigaction sa;

		memset(&sa, 0, sizeof sa);
		sa.sa_handler = h;
		sigaction(SIGUSR1, &sa, NULL);
		kill(getpid(), SIGUSR1);
		printf("caught %d\n", got == SIGUSR1);
		if ((pid = fork()) == 0) {
			for (;;)
				pause();
		}
		if (pid < 0)
			return 1;
		sleep(1);
		kill(pid, SIGTERM);
		waitpid(pid, &st, 0);
		printf("killed %d\n", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "exec") == 0) {
		if ((pid = fork()) == 0) {
			execv(argv[2], argv + 2);
			_exit(99);
		}
		waitpid(pid, &st, 0);
		printf("status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 0;
	}
	return 2;
}
