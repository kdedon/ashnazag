/*
 * tcpcat -- stdin to a TCP peer, then the peer's reply to stdout.
 *
 *	tcpcat a.b.c.d port < in > out
 */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

extern unsigned long inet_addr();
extern int read(), write(), close(), atoi(), socket(), connect(), shutdown();

int
main(argc, argv)
int argc;
char **argv;
{
	struct sockaddr_in sin;
	static char buf[8192];
	int s, n;

	if (argc != 3) {
		fprintf(stderr, "usage: tcpcat a.b.c.d port\n");
		return 2;
	}
	memset((char *)&sin, 0, sizeof sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(argv[1]);
	sin.sin_port = htons(atoi(argv[2]));
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0 ||
	    connect(s, (struct sockaddr *)&sin, sizeof sin) < 0) {
		perror("tcpcat");
		return 1;
	}
	while ((n = read(0, buf, sizeof buf)) > 0)
		if (write(s, buf, n) != n) {
			perror("tcpcat: send");
			return 1;
		}
	shutdown(s, 1);
	while ((n = read(s, buf, sizeof buf)) > 0)
		(void)write(1, buf, n);
	close(s);
	return n < 0;
}
