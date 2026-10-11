/*
 * xamisc.c -- XAMISC.PRG: MiNT calls of the layer, results as lines of
 * C:\XAMISC.TXT: "fork N" (the child's exit code, with the parent's
 * memory unchanged), "vfork N", "poll N" (descriptors ready), "sock N"
 * (bytes echoed over a loopback connection), each -1 on failure.
 */
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int shared = 1;

static int
tfork(void)
{
	int st;
	pid_t p = fork();

	if (p == 0) {
		shared = 42;
		_exit(shared + 1);
	}
	if (p < 0 || waitpid(p, &st, 0) != p || shared != 1)
		return -1;
	return WEXITSTATUS(st);
}

static int
tvfork(void)
{
	int st;
	pid_t p = vfork();

	if (p == 0)
		_exit(7);
	if (p < 0 || waitpid(p, &st, 0) != p)
		return -1;
	return WEXITSTATUS(st);
}

static int
tpoll(void)
{
	int fd[2];
	struct pollfd p[2];

	if (pipe(fd) < 0)
		return -1;
	p[0].fd = fd[0];
	p[0].events = POLLIN;
	p[1].fd = fd[1];
	p[1].events = POLLOUT;
	if (poll(p, 2, 0) != 1 || !(p[1].revents & POLLOUT))
		return -1;
	write(fd[1], "x", 1);
	return poll(p, 2, 100);
}

static int
tsock(void)
{
	struct sockaddr_in a;
	socklen_t len = sizeof a;
	char b[8];
	int l, c, s, n;

	if ((l = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(0x7f000001);
	if (bind(l, (struct sockaddr *)&a, sizeof a) < 0 || listen(l, 1) < 0 ||
	    getsockname(l, (struct sockaddr *)&a, &len) < 0 || a.sin_port == 0)
		return -2;
	a.sin_addr.s_addr = htonl(0x7f000001);
	if ((c = socket(AF_INET, SOCK_STREAM, 0)) < 0 ||
	    connect(c, (struct sockaddr *)&a, sizeof a) < 0)
		return -3;
	if ((s = accept(l, 0, 0)) < 0)
		return -4;
	if (write(c, "ping", 4) != 4 || (n = read(s, b, sizeof b)) != 4 ||
	    write(s, b, n) != n || read(c, b, sizeof b) != 4 || memcmp(b, "ping", 4))
		return -5;
	close(s);
	close(c);
	close(l);
	return n;
}

int
main(void)
{
	FILE *f = fopen("C:\\XAMISC.TX_", "w");

	if (!f)
		return 1;
	fprintf(f, "fork %d\n", tfork());
	fprintf(f, "vfork %d\n", tvfork());
	fprintf(f, "poll %d\n", tpoll());
	fprintf(f, "sock %d\n", tsock());
	fclose(f);
	(void)Frename(0, "C:\\XAMISC.TX_", "C:\\XAMISC.TXT");
	return 0;
}
