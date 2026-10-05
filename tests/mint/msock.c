/*
 * msock -- sockets: "msock echo ADDR PORT" sends a line and reads it
 * back; "msock refused ADDR PORT" expects ECONNREFUSED.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int
main(int argc, char **argv)
{
	struct sockaddr_in a, b;
	socklen_t bl = sizeof b;
	char buf[64];
	int s, n = 0, k, t = 0;

	if (argc != 4)
		return 2;
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		return 1;
	}
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(atoi(argv[3]));
	a.sin_addr.s_addr = inet_addr(argv[2]);
	if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
		if (strcmp(argv[1], "refused") == 0 && errno == ECONNREFUSED) {
			printf("refused\n");
			return 0;
		}
		printf("connect: %s\n", strerror(errno));
		return 1;
	}
	if (strcmp(argv[1], "echo") != 0)
		return 1;
	if (getpeername(s, (struct sockaddr *)&b, &bl) < 0 || b.sin_port != a.sin_port)
		printf("peername bad\n");
	k = 1;
	setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &k, sizeof k);
	bl = sizeof t;
	if (getsockopt(s, SOL_SOCKET, SO_TYPE, &t, &bl) < 0 || t != SOCK_STREAM)
		printf("type bad\n");
	write(s, "mint echo\n", 10);
	while (n < 10 && (k = read(s, buf + n, 10 - n)) > 0)
		n += k;
	buf[n] = 0;
	printf("got %s", buf);
	shutdown(s, 2);
	close(s);
	return 0;
}
