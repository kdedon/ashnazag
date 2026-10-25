/*
 * t_dhcp.c -- sdhcp on QEMU's user-mode network, which serves DHCP:
 * 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.
 *
 * Starts from an unconfigured aen0 (address 0.0.0.0, no default route,
 * a stale resolv.conf), runs sdhcp and checks the address, the default
 * route, resolv.conf and dhcpinfo, then pings the gateway and asks the
 * leased DNS server for a name.  SIGTERM ends the daemon.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include "t.h"

#define OUT	"/tmp/t_dhcp.out"

static char out[4096];

/* run argv, stdout and stderr to OUT, then into out; exit status, or -1 */
static int
run(char *a0, char *a1, char *a2, char *a3, char *a4, int secs)
{
	char *av[6];
	pid_t pid;
	int st, fd, n;

	av[0] = a0; av[1] = a1; av[2] = a2; av[3] = a3; av[4] = a4; av[5] = 0;
	out[0] = '\0';
	pid = fork();
	if (pid == 0) {
		fd = open(OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		execv(av[0], av);
		_exit(127);
	}
	if (pid < 0 || t_waitchild(pid, &st, secs) < 0)
		return -1;
	if ((fd = open(OUT, O_RDONLY)) >= 0) {
		n = read(fd, out, sizeof(out) - 1);
		out[n > 0 ? n : 0] = '\0';
		close(fd);
	}
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* first line of out, for messages */
static char *
line1(void)
{
	static char l[80];
	int i;

	for (i = 0; out[i] && out[i] != '\n' && i < sizeof(l) - 1; i++)
		l[i] = out[i];
	l[i] = '\0';
	return l;
}

static int
contains(char *file, char *s)
{
	char buf[1024];
	int fd, n;

	if ((fd = open(file, O_RDONLY)) < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = '\0';
	return strstr(buf, s) != 0;
}

/* an A query for name to the server at ip; answers, or -1 */
static int
dnsquery(unsigned long ip, char *name)
{
	unsigned char q[512], *p;
	struct sockaddr_in sin;
	struct pollfd pfd;
	char *s, *dot;
	int fd, n, try;

	memset(q, 0, sizeof(q));
	q[0] = 0x12; q[1] = 0x34;	/* id */
	q[2] = 0x01;			/* recursion desired */
	q[5] = 1;			/* one question */
	p = q + 12;
	for (s = name; *s; s = dot + 1) {
		dot = strchr(s, '.');
		if (!dot)
			dot = s + strlen(s);
		*p++ = dot - s;
		memcpy(p, s, dot - s);
		p += dot - s;
		if (!*dot)
			break;
	}
	*p++ = 0;
	*p++ = 0; *p++ = 1;		/* A */
	*p++ = 0; *p++ = 1;		/* IN */
	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(53);
	sin.sin_addr.s_addr = ip;
	for (try = 0; try < 3; try++) {
		sendto(fd, (char *)q, p - q, 0, (struct sockaddr *)&sin, sizeof(sin));
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 3000) <= 0)
			continue;
		n = recv(fd, (char *)q, sizeof(q), 0);
		close(fd);
		if (n < 12 || q[0] != 0x12 || q[1] != 0x34 || (q[3] & 0xf))
			return -1;
		return q[6] << 8 | q[7];
	}
	close(fd);
	return -1;
}

int
main(void)
{
	int r, fd;
	long pid;

	t_init("dhcp", 170);
	if (access("/tests/sdhcp", X_OK) != 0) {
		t_skip("run", "no /tests/sdhcp");
		return t_done();
	}
	/* the stack as boot sets it up */
	if (run("/usr/sbin/ifconfig", "lo0", 0, 0, 0, 20) != 0)
		run("/usr/sbin/slink", 0, 0, 0, 0, 30);
	/* unconfigured: no address, no route, a stale resolv.conf */
	run("/usr/sbin/route", "delete", "default", "10.0.2.2", 0, 20);
	run("/usr/sbin/ifconfig", "aen0", "0.0.0.0", 0, 0, 20);
	unlink("/etc/resolv.conf");
	if ((fd = creat("/etc/resolv.conf", 0644)) >= 0) {
		write(fd, "nameserver 192.0.2.1\n", 21);
		close(fd);
	}
	unlink("/etc/dhcp/aen0");

	r = run("/tests/sdhcp", "-t", "60", "aen0", 0, 90);
	if (!t_check("lease", r == 0, "exit %d: %s", r, line1()))
		return t_done();
	t_info("sdhcp", "%s", line1());

	r = run("/usr/sbin/ifconfig", "aen0", 0, 0, 0, 20);
	t_check("ifconfig", r == 0 && strstr(out, "inet 10.0.2.15") &&
	    strstr(out, "netmask ffffff00"), "%s", out);
	/* a second default route to the same gateway is refused */
	run("/usr/sbin/route", "add", "default", "10.0.2.2", "1", 20);
	t_check("route_default", strstr(out, "File exists") != 0, "%s", line1());
	t_check("resolv_conf", contains("/etc/resolv.conf", "nameserver 10.0.2.3\n") &&
	    !contains("/etc/resolv.conf", "192.0.2.1"), "no nameserver 10.0.2.3");

	r = run("/tests/dhcpinfo", "ip", 0, 0, 0, 20);
	t_check("dhcpinfo_ip", r == 0 && !strcmp(out, "10.0.2.15\n"), "exit %d: %s", r, line1());
	r = run("/tests/dhcpinfo", "-i", "aen0", "router", 0, 20);
	t_check("dhcpinfo_router", r == 0 && !strcmp(out, "10.0.2.2\n"), "exit %d: %s", r, line1());
	r = run("/tests/dhcpinfo", "dns", 0, 0, 0, 20);
	t_check("dhcpinfo_dns", r == 0 && !strcmp(out, "10.0.2.3\n"), "exit %d: %s", r, line1());
	r = run("/tests/dhcpinfo", "nosuch", 0, 0, 0, 20);
	t_check("dhcpinfo_unknown", r == 1, "exit %d", r);

	r = run("/usr/sbin/ping", "10.0.2.2", "10", 0, 0, 20);
	t_check("ping_gateway", r == 0, "exit %d: %s", r, line1());
	r = dnsquery(htonl(0x0a000203), "localhost");
	t_check("dns_lookup", r > 0, "%d answers", r);

	pid = 0;
	if (run("/tests/dhcpinfo", "pid", 0, 0, 0, 20) == 0)
		pid = atol(out);
	if (t_check("daemon", pid > 1 && kill(pid, 0) == 0, "pid %ld", pid)) {
		kill(pid, SIGTERM);
		sleep(2);
		t_check("release", kill(pid, 0) != 0 && access("/etc/dhcp/aen0", 0) != 0,
		    "daemon still there or lease file left");
	}
	unlink(OUT);
	return t_done();
}
