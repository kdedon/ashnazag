/*
 * t_net.c -- on-board Ethernet under the stock inet stack, on QEMU's
 * user-mode network: gateway 10.0.2.2, a TCP echo at 10.0.2.100:7
 * (guestfwd to cat), host 10.0.2.2 port 1 closed.
 *
 * DLPI on a raw /dev/aen0 stream, then the tape's configuration (slink,
 * ifconfig, route), ping, TCP echo of 64 KB, a refused connect,
 * ifconfig down/up, and the driver's interrupt counters.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stropts.h>
#include <sys/dlpi.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include "t.h"

#define DL_ENABMULTI_REQ	0x1d
#define DL_PHYS_ADDR_REQ	0x31
#define DL_PHYS_ADDR_ACK	0x32
#define DL_TOOMANY		0x13

#define GW	0x0a000202	/* 10.0.2.2 */
#define ECHO	0x0a000264	/* 10.0.2.100 */

static long ctl[64];

/* one control message back within secs; its primitive, or -1 */
static long
dl_reply(int fd, int secs, int *len)
{
	struct strbuf c;
	struct pollfd p;
	int fl = 0;

	p.fd = fd;
	p.events = POLLIN | POLLPRI;
	if (poll(&p, 1, secs * 1000) != 1)
		return -1;
	c.buf = (char *)ctl;
	c.maxlen = sizeof ctl;
	c.len = 0;
	if (getmsg(fd, &c, (struct strbuf *)0, &fl) < 0 || c.len < 4)
		return -1;
	*len = c.len;
	return ctl[0];
}

static int
dl_send(int fd, void *m, int n)
{
	struct strbuf c;

	c.buf = (char *)m;
	c.len = n;
	c.maxlen = 0;
	return putmsg(fd, &c, (struct strbuf *)0, 0);
}

static void
t_dlpi(void)
{
	long m[16];
	unsigned char *a;
	int fd, n, i, ok;
	long r;

	fd = open("/dev/aen0", O_RDWR);
	if (!t_check("dlpi_open", fd >= 0, "/dev/aen0: %s", T_ERR))
		return;

	m[0] = DL_INFO_REQ;
	dl_send(fd, m, 4);
	r = dl_reply(fd, 5, &n);
	t_check("dlpi_info", r == DL_INFO_ACK && n >= (int)sizeof(dl_info_ack_t) &&
	    ((dl_info_ack_t *)ctl)->dl_current_state == DL_UNBOUND &&
	    ((dl_info_ack_t *)ctl)->dl_mac_type == DL_ETHER &&
	    ((dl_info_ack_t *)ctl)->dl_max_sdu == 1500,
	    "reply %ld len %d", r, n);

	m[0] = DL_PHYS_ADDR_REQ;
	m[1] = 1;
	dl_send(fd, m, 8);
	r = dl_reply(fd, 5, &n);
	a = (unsigned char *)ctl + ctl[2];
	ok = r == DL_PHYS_ADDR_ACK && ctl[1] == 6 && ctl[2] + 6 <= n;
	if (ok)
		t_info("mac", "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2],
		    a[3], a[4], a[5]);
	t_check("dlpi_physaddr_apple", ok && a[0] == 8 && a[1] == 0 && a[2] == 7,
	    "reply %ld len %d", r, n);

	/* AppleTalk's SNAP binding */
	m[0] = DL_BIND_REQ;
	m[1] = 0xaa;
	for (i = 2; i < 6; i++)
		m[i] = 0;
	dl_send(fd, m, sizeof(dl_bind_req_t));
	r = dl_reply(fd, 5, &n);
	t_check("dlpi_bind_snap", r == DL_BIND_ACK && ctl[1] == 0xaa, "reply %ld", r);
	m[0] = DL_SUBS_BIND_REQ;
	m[1] = 12;
	m[2] = 5;
	memcpy((char *)&m[3], "\010\000\007\200\233", 5);
	dl_send(fd, m, 17);
	r = dl_reply(fd, 5, &n);
	t_check("dlpi_subs_bind", r == DL_SUBS_BIND_ACK, "reply %ld", r);

	/* four multicast addresses per stream, the fifth refused */
	for (i = 0; i < 5; i++) {
		m[0] = DL_ENABMULTI_REQ;
		m[1] = 6;
		m[2] = 12;
		memcpy((char *)&m[3], "\011\000\007\000\000\000", 6);
		((unsigned char *)&m[3])[5] = i;
		dl_send(fd, m, 18);
		r = dl_reply(fd, 5, &n);
		if (i < 4 && r != DL_OK_ACK)
			break;
	}
	t_check("dlpi_multicast_limit", i == 5 && r == DL_ERROR_ACK && ctl[2] == DL_TOOMANY,
	    "request %d: reply %ld errno %ld", i, r, ctl[2]);

	/* destination offset far outside the message: dropped */
	m[0] = DL_UNITDATA_REQ;
	m[1] = 6;
	m[2] = 0x7ffffff0;
	m[3] = m[4] = 0;
	dl_send(fd, m, 20);
	m[0] = DL_INFO_REQ;
	dl_send(fd, m, 4);
	r = dl_reply(fd, 5, &n);
	t_check("dlpi_bad_offset", r == DL_INFO_ACK &&
	    ((dl_info_ack_t *)ctl)->dl_current_state == DL_IDLE, "reply %ld", r);
	close(fd);
}

/* run argv with output to the console; exit status, or -1 */
static int
run(char **av, int secs)
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid == 0) {
		execv(av[0], av);
		_exit(127);
	}
	if (pid < 0 || t_waitchild(pid, &st, secs) < 0)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int
cmd(char *name, int secs, char *a0, char *a1, char *a2, char *a3, char *a4,
    char *a5, char *a6)
{
	char *av[8];
	int r;

	av[0] = a0; av[1] = a1; av[2] = a2; av[3] = a3;
	av[4] = a4; av[5] = a5; av[6] = a6; av[7] = 0;
	r = run(av, secs);
	return t_check(name, r == 0, "%s: exit %d", a0, r);
}

static int
tcp_open(unsigned long host, int port, int *err)
{
	struct sockaddr_in s;
	int fd;

	*err = 0;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		*err = errno;
		return -1;
	}
	memset(&s, 0, sizeof s);
	s.sin_family = AF_INET;
	s.sin_port = port;
	s.sin_addr.s_addr = host;
	if (connect(fd, (struct sockaddr *)&s, sizeof s) < 0) {
		*err = errno;
		close(fd);
		return -1;
	}
	return fd;
}

static void
t_tcp(void)
{
	char out[1024], in[1024];
	int fd, err, i, n, k, bad;
	long t0, ms;

	fd = tcp_open((unsigned long)ECHO, 7, &err);
	if (!t_check("tcp_connect", fd >= 0, "10.0.2.100:7: %s", strerror(err)))
		return;
	t0 = t_now_ms();
	bad = -1;
	for (i = 0; i < 64 && bad < 0; i++) {
		for (k = 0; k < (int)sizeof out; k++)
			out[k] = t_pattern((long)i * sizeof out + k, 7);
		if (write(fd, out, sizeof out) != sizeof out) {
			bad = i;
			break;
		}
		for (n = 0; n < (int)sizeof in; n += k)
			if ((k = read(fd, in + n, sizeof in - n)) <= 0)
				break;
		if (n != sizeof in || memcmp(in, out, sizeof in) != 0)
			bad = i;
	}
	ms = t_now_ms() - t0;
	t_check("tcp_echo_64k", bad < 0, "block %d wrong or short", bad);
	if (bad < 0)
		t_info("tcp_echo_ms", "%ld ms for 64 KB each way", ms);
	close(fd);

	fd = tcp_open((unsigned long)GW, 1, &err);
	t_check("tcp_refused", fd < 0 && err == ECONNREFUSED, "10.0.2.2:1: fd %d, %s",
	    fd, strerror(err));
	if (fd >= 0)
		close(fd);
}

int
main(void)
{
	long v;
	int ok;

	t_init("net", 280);
	t_dlpi();

	cmd("aen_status", 20, "/usr/amiga/bin/aen", "-S", 0, 0, 0, 0, 0);
	cmd("slink_boot", 30, "/usr/sbin/slink", 0, 0, 0, 0, 0, 0);
	if (!cmd("slink_addaen", 30, "/usr/sbin/slink", "addaen", "/dev/aen0",
	    "aen0", 0, 0, 0))
		return t_done();
	if (!cmd("ifconfig", 20, "/usr/sbin/ifconfig", "aen0", "10.0.2.15",
	    "netmask", "0xffffff00", "up", "-trailers"))
		return t_done();
	cmd("route_default", 20, "/usr/sbin/route", "add", "default", "10.0.2.2", "1",
	    0, 0);
	ok = cmd("ping_gateway", 20, "/usr/sbin/ping", "10.0.2.2", "10", 0, 0, 0, 0);
	if (ok) {
		cmd("ping_s", 30, "/usr/sbin/ping", "-s", "10.0.2.2", "56", "5", 0, 0);
		t_tcp();
	}
	if (cmd("ifconfig_down", 20, "/usr/sbin/ifconfig", "aen0", "down", 0, 0, 0, 0) &&
	    cmd("ifconfig_up", 20, "/usr/sbin/ifconfig", "aen0", "up", 0, 0, 0, 0))
		cmd("ping_after_up", 20, "/usr/sbin/ping", "10.0.2.2", "10", 0, 0, 0, 0);
	cmd("aen_status_end", 20, "/usr/amiga/bin/aen", "-S", 0, 0, 0, 0, 0);

	v = t_kmem("sn_nintr");
	if (v < 0)
		t_skip("ca1_interrupts", "no sn_nintr in /tests/ksyms");
	else
		t_check("ca1_interrupts", v > 0, "%ld through VIA2 CA1", v);
	t_info("counters", "sn_nintr %ld sn_nslot %ld", v, t_kmem("sn_nslot"));
	return t_done();
}
