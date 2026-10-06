/*
 * bsdsrv -- t_amiga's host side of SYS:bsdtest: a TCP server on
 * 127.0.0.1:7397 and a name server on 127.0.0.1:53 that knows
 * bsdtest.example (10.1.2.3), with resolv.conf pointing at it while it
 * runs.  Creates the go file named by its argument, then prints one
 * "name ok detail" line per check.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

#define	RESOLV	"/etc/resolv.conf"

static int dfd = -1, queries;
static char saved[1024];
static int nsaved = -1;

static long
ms()
{
	struct tms t;

	return times(&t) * (1000L / CLK_TCK);
}

static void
check(name, ok, detail)
	char *name, *detail;
	int ok;
{
	printf("%s %d %s\n", name, ok != 0, detail);
	fflush(stdout);
}

/* one query: bsdtest.example A gets 10.1.2.3, anything else NXDOMAIN */
static void
answer()
{
	static unsigned char want[] = "\7bsdtest\7example";
	unsigned char b[512];
	struct sockaddr_in from;
	int n, fl = sizeof from, k;

	if ((n = recvfrom(dfd, (char *)b, sizeof b - 16, 0, (struct sockaddr *)&from, &fl)) < 12)
		return;
	k = 12 + sizeof want;
	if (n >= k + 4 && memcmp(b + 12, want, sizeof want) == 0 && b[k + 1] == 1) {
		queries++;
		b[2] = 0x81; b[3] = 0x80;
		b[6] = 0; b[7] = 1;
		memcpy(b + k + 4, "\300\14\0\1\0\1\0\0\0\74\0\4\12\1\2\3", 16);
		n = k + 20;
	} else {
		b[2] = 0x81; b[3] = 0x83;
	}
	sendto(dfd, (char *)b, n, 0, (struct sockaddr *)&from, fl);
}

/* wait up to ms for fd while answering queries: 1 ready */
static int
await(fd, t)
	int fd;
	long t;
{
	struct pollfd p[2];
	long end = ms() + t;

	while (ms() < end) {
		p[0].fd = fd; p[0].events = POLLIN;
		p[1].fd = dfd; p[1].events = POLLIN;
		if (poll(p, dfd >= 0 ? 2 : 1, 200) <= 0)
			continue;
		if (p[1].revents & POLLIN)
			answer();
		if (p[0].revents)
			return 1;
	}
	return 0;
}

static int
server(type, port)
	int type, port;
{
	struct sockaddr_in a;
	int s = socket(AF_INET, type, 0), on = 1;

	memset((char *)&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(0x7f000001);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof on);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 ||
	    (type == SOCK_STREAM && listen(s, 1) < 0)) {
		close(s);
		return -1;
	}
	return s;
}

/* a line from fd into b, waiting up to t ms in all */
static int
line(fd, b, n, t)
	int fd, n;
	char *b;
	long t;
{
	int k = 0, r;

	while (k < n - 1 && await(fd, t)) {
		if ((r = read(fd, b + k, 1)) <= 0)
			break;
		if (b[k++] == '\n')
			break;
	}
	b[k] = 0;
	return k;
}

main(argc, argv)
	int argc;
	char **argv;
{
	char b[256], d[64], v[6][32];
	int l, c, fd, sel0, sel1, refused;
	long t0;

	if (argc != 2) {
		fprintf(stderr, "usage: bsdsrv go-file\n");
		return 2;
	}
	if ((fd = open(RESOLV, O_RDONLY)) >= 0) {
		nsaved = read(fd, saved, sizeof saved);
		close(fd);
	}
	if ((dfd = server(SOCK_DGRAM, 53)) >= 0 &&
	    (fd = open(RESOLV, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		write(fd, "nameserver 127.0.0.1\n", 21);
		close(fd);
	}
	check("bsd_nameserver", dfd >= 0, "127.0.0.1:53");
	if ((l = server(SOCK_STREAM, 7397)) < 0) {
		check("bsd_server", 0, "cannot listen on 127.0.0.1:7397");
		goto out;
	}
	close(creat(argv[1], 0644));
	if (!await(l, 180000L) || (c = accept(l, (struct sockaddr *)0, (int *)0)) < 0) {
		check("bsd_connect", 0, "no connection in 180 s");
		goto out;
	}
	/* gone before bsdtest returns, so the guest's poll runs it once */
	unlink(argv[1]);
	t0 = ms();
	check("bsd_connect", 1, "accepted");
	line(c, b, sizeof b, 30000L);
	sprintf(d, "hello after %ld ms", ms() - t0);
	check("bsd_hello", strcmp(b, "hello\n") == 0, d);
	check("bsd_timeout", ms() - t0 >= 900 && ms() - t0 < 5000, d);
	write(c, "world\n", 6);
	line(c, b, sizeof b, 60000L);
	b[strcspn(b, "\n")] = 0;
	printf("report: %s\n", b);
	if (sscanf(b, "R localhost=%31s select0=%d select1=%d recv=%31s dns=%31s refused=%d",
	    v[0], &sel0, &sel1, v[1], v[2], &refused) != 6) {
		check("bsd_report", 0, b);
		goto out;
	}
	check("bsd_hosts", strcmp(v[0], "127.0.0.1") == 0, v[0]);
	sprintf(d, "%d", sel0);
	check("bsd_select_timeout", sel0 == 0, d);
	sprintf(d, "%d", sel1);
	check("bsd_select_ready", sel1 == 1, d);
	check("bsd_recv", strcmp(v[1], "world") == 0, v[1]);
	sprintf(d, "%s after %d queries", v[2], queries);
	check("bsd_dns", strcmp(v[2], "10.1.2.3") == 0 && queries > 0, d);
	sprintf(d, "errno %d", refused);
	check("bsd_refused", refused == 61, d);
out:
	unlink(argv[1]);
	if (nsaved >= 0 && (fd = open(RESOLV, O_WRONLY | O_TRUNC)) >= 0) {
		write(fd, saved, nsaved);
		close(fd);
	} else if (nsaved < 0)
		unlink(RESOLV);
	return 0;
}
