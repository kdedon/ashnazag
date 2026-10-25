#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stropts.h>
#include <sys/dlpi.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arg.h"
#include "util.h"

#ifndef DL_PHYS_ADDR_REQ
#define DL_PHYS_ADDR_REQ	0x31
#define DL_PHYS_ADDR_ACK	0x32
#endif
#define DL_CURR_PHYS_ADDR	2

typedef struct {
	unsigned long dl_primitive;
	unsigned long dl_addr_length;
	unsigned long dl_addr_offset;
} physack_t;

typedef unsigned long uint32_t;

typedef struct bootp {
	unsigned char op      [1];
	unsigned char htype   [1];
	unsigned char hlen    [1];
	unsigned char hops    [1];
	unsigned char xid     [4];
	unsigned char secs    [2];
	unsigned char flags   [2];
	unsigned char ciaddr  [4];
	unsigned char yiaddr  [4];
	unsigned char siaddr  [4];
	unsigned char giaddr  [4];
	unsigned char chaddr  [16];
	unsigned char sname   [64];
	unsigned char file    [128];
	unsigned char magic   [4];
	unsigned char optdata [312-4];
} Bootp;

enum {
	DHCPdiscover =       1,
	DHCPoffer,
	DHCPrequest,
	DHCPdecline,
	DHCPack,
	DHCPnak,
	DHCPrelease,
	DHCPinform,
	Timeout0 =         200,
	Timeout1,
	Timeout2,

	Bootrequest =        1,
	Bootreply =          2,
	/* bootp flags */
	Fbroadcast =   1 << 15,

	OBpad =              0,
	OBmask =             1,
	OBrouter =           3,
	OBnameserver =       5,
	OBdnsserver =        6,
	OBhostname =        12,
	OBbaddr =           28,
	ODipaddr =          50, /* 0x32 */
	ODlease =           51,
	ODoverload =        52,
	ODtype =            53, /* 0x35 */
	ODserverid =        54, /* 0x36 */
	ODparams =          55, /* 0x37 */
	ODmessage =         56,
	ODmaxmsg =          57,
	ODrenewaltime =     58,
	ODrebindingtime =   59,
	ODvendorclass =     60,
	ODclientid =        61, /* 0x3d */
	ODtftpserver =      66,
	ODbootfile =        67,
	OBend =            255,
};

enum { Broadcast, Unicast };

Bootp bp;
unsigned char magic[] = { 99, 130, 83, 99 };

/* conf */
static unsigned char xid[sizeof(bp.xid)];
static unsigned char hwaddr[16];
static char hostname[SYS_NMLN];
static time_t starttime;
static char *ifname = "aen0";
static unsigned char cid[16];
static char *program = "";
static int sock;
static time_t timers[3];	/* deadlines; 0 is off */
static int tflag = 0;		/* seconds to wait for the first lease; 0 waits for ever */
/* sav */
static unsigned char server[4];
static unsigned char client[4];
static unsigned char mask[4];
static unsigned char router[4];
static unsigned char dns[12];
/* what ifconfig, route and resolv.conf hold */
static unsigned char setclient[4], setmask[4], setrouter[4], setdns[12];

static int dflag = 1; /* change DNS in /etc/resolv.conf ? */
static int iflag = 1; /* set IP ? */
static int fflag = 0; /* run in foreground */

static unsigned char ipbcast[4] = { 255, 255, 255, 255 };

static void
hnput(unsigned char *dst, uint32_t src, size_t n)
{
	unsigned int i;

	for (i = 0; n--; i++)
		dst[i] = (src >> (n * 8)) & 0xff;
}

static struct sockaddr *
iptoaddr(struct sockaddr *ifaddr, unsigned char ip[4], int port)
{
	struct sockaddr_in *in = (struct sockaddr_in *)ifaddr;

	memset(in, 0, sizeof(*in));
	in->sin_family = AF_INET;
	in->sin_port = htons(port);
	memcpy(&(in->sin_addr), ip, sizeof(in->sin_addr));

	return ifaddr;
}

/* sendto UDP wrapper */
static int
udpsend(unsigned char ip[4], int fd, void *data, size_t n)
{
	struct sockaddr_in addr;
	int sent;

	iptoaddr((struct sockaddr *)&addr, ip, 67); /* bootp server */
	if ((sent = sendto(fd, data, n, 0, (struct sockaddr *)&addr, sizeof(addr))) == -1)
		weprintf("sendto:");

	return sent;
}

/* recvfrom UDP wrapper */
static int
udprecv(int fd, void *data, size_t n)
{
	struct sockaddr_in addr;
	int addrlen = sizeof(addr);
	int r;

	if ((r = recvfrom(fd, data, n, 0, (struct sockaddr *)&addr, &addrlen)) == -1)
		eprintf("recvfrom:");

	return r;
}

static char *
ipstr(unsigned char *ip)
{
	static char buf[4][16];
	static int i;

	i = (i + 1) % 4;
	sprintf(buf[i], "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
	return buf[i];
}

static int
sh(char *cmd)
{
	int r = system(cmd);

	if (r != 0)
		weprintf("failed: %s\n", cmd);
	return r;
}

/* plumb the interface if it is not yet, then up with no address */
static void
ifinit(void)
{
	char cmd[128];

	sprintf(cmd, "/usr/sbin/ifconfig %s > /dev/null 2>&1", ifname);
	if (system(cmd) != 0) {
		sprintf(cmd, "/usr/sbin/slink addaen /dev/%s %s", ifname, ifname);
		if (sh(cmd) != 0)
			exit(1);
	}
	sprintf(cmd, "/usr/sbin/ifconfig %s 0.0.0.0 up -trailers", ifname);
	if (sh(cmd) != 0)
		exit(1);
}

static void
setip(unsigned char ip[4], unsigned char mask[4], unsigned char gateway[4])
{
	char cmd[128];

	if (memcmp(ip, setclient, 4) || memcmp(mask, setmask, 4)) {
		sprintf(cmd, "/usr/sbin/ifconfig %s %s netmask %s up -trailers",
		        ifname, ipstr(ip), ipstr(mask));
		if (sh(cmd) == 0) {
			memcpy(setclient, ip, 4);
			memcpy(setmask, mask, 4);
		}
	}
	if (memcmp(gateway, setrouter, 4)) {
		if (setrouter[0]) {
			sprintf(cmd, "/usr/sbin/route delete default %s > /dev/null", ipstr(setrouter));
			sh(cmd);
		}
		memset(setrouter, 0, 4);
		sprintf(cmd, "/usr/sbin/route add default %s 1 > /dev/null", ipstr(gateway));
		if (gateway[0] && sh(cmd) == 0)
			memcpy(setrouter, gateway, 4);
	}
}

static void
cat(int dfd, char *src)
{
	char buf[BUFSIZ];
	int n, fd;

	if ((fd = open(src, O_RDONLY)) == -1)
		return; /* can't read, but don't error out */
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(dfd, buf, n);
	close(fd);
}

static void
setdnsconf(unsigned char dns[12])
{
	char buf[128];
	int fd, i;

	if (!memcmp(dns, setdns, sizeof(setdns)))
		return;
	if ((fd = creat("/etc/resolv.conf", 0644)) == -1) {
		weprintf("can't change /etc/resolv.conf:");
		return;
	}
	cat(fd, "/etc/resolv.conf.head");
	for (i = 0; i < 12 && dns[i]; i += 4) {
		sprintf(buf, "nameserver %s\n", ipstr(dns + i));
		write(fd, buf, strlen(buf));
	}
	cat(fd, "/etc/resolv.conf.tail");
	close(fd);
	memcpy(setdns, dns, sizeof(setdns));
}

/* the lease as KEY=value lines in /etc/dhcp/ifname, for dhcpinfo and scripts */
static void
savelease(uint32_t lease, long pid)
{
	char path[64], tmp[64], d[48];
	FILE *f;
	int i;

	mkdir("/etc/dhcp", 0755);
	sprintf(path, "/etc/dhcp/%s", ifname);
	sprintf(tmp, "%s.new", path);
	if ((f = fopen(tmp, "w")) == NULL) {
		weprintf("%s:", tmp);
		return;
	}
	d[0] = '\0';
	for (i = 0; i < 12 && dns[i]; i += 4)
		sprintf(d + strlen(d), "%s%s", i ? " " : "", ipstr(dns + i));
	fprintf(f, "IP=%s\nMASK=%s\nROUTER=%s\nDNS='%s'\nSERVER=%s\nLEASE=%lu\nPID=%ld\n",
	        ipstr(client), ipstr(mask), ipstr(router), d, ipstr(server), lease, pid);
	fclose(f);
	rename(tmp, path);
}

static void
optget(Bootp *bp, void *data, int opt, int n)
{
	unsigned char *p = bp->optdata;
	unsigned char *top = ((unsigned char *)bp) + sizeof(*bp);
	int code, len;

	while (p < top) {
		code = *p++;
		if (code == OBpad)
			continue;
		if (code == OBend || p == top)
			break;
		len = *p++;
		if (len > top - p)
			break;
		if (code == opt) {
			memcpy(data, p, MIN(len, n));
			break;
		}
		p += len;
	}
}

static unsigned char *
optput(unsigned char *p, int opt, unsigned char *data, size_t len)
{
	*p++ = opt;
	*p++ = (unsigned char)len;
	memcpy(p, data, len);

	return p + len;
}

static unsigned char *
hnoptput(unsigned char *p, int opt, uint32_t data, size_t len)
{
	*p++ = opt;
	*p++ = (unsigned char)len;
	hnput(p, data, len);

	return p + len;
}

/* send a control message on a DLPI stream; the reply's primitive */
static long
dlctl(int fd, long *buf, int len, int maxlen)
{
	struct strbuf ctl;
	int flags = 0;

	ctl.buf = (char *)buf;
	ctl.len = len;
	if (putmsg(fd, &ctl, NULL, 0) == -1)
		return -1;
	ctl.maxlen = maxlen;
	if (getmsg(fd, &ctl, NULL, &flags) == -1 || ctl.len < (int)sizeof(long))
		return -1;
	return buf[0];
}

/*
 * While a reply may come as a broadcast, a DLPI stream bound to IP's
 * ethertype sends and receives the DHCP datagrams whole: the IP stack
 * turns 255.255.255.255 into the interface's broadcast address (for an
 * unconfigured one 0.255.255.255) and drops limited broadcasts to it.
 */
static int dlfd = -1;

static void
dlup(void)
{
	char dev[64];
	long buf[16];

	if (dlfd >= 0)
		return;
	sprintf(dev, "/dev/%s", ifname);
	if ((dlfd = open(dev, O_RDWR)) == -1)
		eprintf("%s:", dev);
	memset(buf, 0, sizeof(buf));
	buf[0] = DL_BIND_REQ;
	buf[1] = 0x800;			/* IP's ethertype */
	if (dlctl(dlfd, buf, sizeof(dl_bind_req_t), sizeof(buf)) != DL_BIND_ACK)
		eprintf("%s: bind refused\n", dev);
}

static void
dldown(void)
{
	if (dlfd >= 0)
		close(dlfd);
	dlfd = -1;
}

static void
dlsend(void *data, size_t n)
{
	static unsigned char ether[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static unsigned char pkt[28 + sizeof(Bootp)];
	long buf[16];
	struct strbuf ctl, dat;
	unsigned long sum;
	int i;

	memset(pkt, 0, 28);
	pkt[0] = 0x45;			/* IPv4, 20-byte header */
	hnput(pkt + 2, 28 + n, 2);
	hnput(pkt + 4, time(NULL), 2);	/* id */
	pkt[8] = 64;			/* ttl */
	pkt[9] = 17;			/* UDP */
	memcpy(pkt + 12, setclient, 4);
	memcpy(pkt + 16, ipbcast, 4);
	for (sum = 0, i = 0; i < 20; i += 2)
		sum += pkt[i] << 8 | pkt[i + 1];
	sum = (sum & 0xffff) + (sum >> 16);
	hnput(pkt + 10, ~(sum + (sum >> 16)), 2);
	hnput(pkt + 20, 68, 2);
	hnput(pkt + 22, 67, 2);
	hnput(pkt + 24, 8 + n, 2);	/* UDP checksum 0: none */
	memcpy(pkt + 28, data, n);

	dlup();
	memset(buf, 0, sizeof(buf));
	buf[0] = DL_UNITDATA_REQ;
	buf[1] = 6;
	buf[2] = sizeof(dl_unitdata_req_t);
	memcpy((char *)buf + sizeof(dl_unitdata_req_t), ether, 6);
	ctl.buf = (char *)buf;
	ctl.len = sizeof(dl_unitdata_req_t) + 6;
	dat.buf = (char *)pkt;
	dat.len = 28 + n;
	if (putmsg(dlfd, &ctl, &dat, 0) == -1)
		weprintf("putmsg:");
}

/* a datagram from the DLPI stream into bp: 1 if it is UDP to port 68 */
static int
dlrecv(void)
{
	static unsigned char pkt[1600];
	long buf[16];
	struct strbuf ctl, dat;
	int flags = 0, hl;

	ctl.buf = (char *)buf;
	ctl.maxlen = sizeof(buf);
	dat.buf = (char *)pkt;
	dat.maxlen = sizeof(pkt);
	if (getmsg(dlfd, &ctl, &dat, &flags) == -1 || dat.len < 28)
		return 0;
	hl = (pkt[0] & 0xf) * 4;
	if ((pkt[0] >> 4) != 4 || pkt[9] != 17 || dat.len < hl + 8 ||
	    pkt[hl + 2] != 0 || pkt[hl + 3] != 68)
		return 0;
	memset(&bp, 0, sizeof(bp));
	memcpy(&bp, pkt + hl + 8, MIN(dat.len - hl - 8, (int)sizeof(bp)));
	return 1;
}

static void
dhcpsend(int type, int how)
{
	static unsigned char params[] = { OBmask, OBrouter, OBdnsserver };
	unsigned char *ip, *p;

	memset(&bp, 0, sizeof(bp));
	hnput(bp.op, Bootrequest, 1);
	hnput(bp.htype, 1, 1);
	hnput(bp.hlen, 6, 1);
	memcpy(bp.xid, xid, sizeof(xid));
	hnput(bp.flags, Fbroadcast, sizeof(bp.flags));
	hnput(bp.secs, time(NULL) - starttime, sizeof(bp.secs));
	memcpy(bp.magic, magic, sizeof(bp.magic));
	memcpy(bp.chaddr, hwaddr, sizeof(bp.chaddr));
	p = bp.optdata;
	p = hnoptput(p, ODtype, type, 1);
	p = optput(p, ODclientid, cid, sizeof(cid));
	p = optput(p, OBhostname, (unsigned char *)hostname, strlen(hostname));

	switch (type) {
	case DHCPdiscover:
		break;
	case DHCPrequest:
		/* memcpy(bp.ciaddr, client, sizeof bp.ciaddr); */
		p = optput(p, ODipaddr, client, sizeof(client));
		p = optput(p, ODserverid, server, sizeof(server));
		p = optput(p, ODparams, params, sizeof(params));
		break;
	case DHCPrelease:
		memcpy(bp.ciaddr, client, sizeof(client));
		p = optput(p, ODipaddr, client, sizeof(client));
		p = optput(p, ODserverid, server, sizeof(server));
		break;
	}
	*p++ = OBend;

	if (how == Broadcast)
		dlsend(&bp, p - (unsigned char *)&bp);
	else
		udpsend(server, sock, &bp, p - (unsigned char *)&bp);
}

static int
dhcprecv(void)
{
	unsigned char type;
	struct pollfd pfd[2];
	time_t now;
	int i, ms, t, got;

	for (;;) {
		now = time(NULL);
		t = -1;
		for (i = 0; i < LEN(timers); i++)
			if (timers[i] && (t < 0 || timers[i] < timers[t]))
				t = i;
		if (t >= 0 && timers[t] <= now) {
			timers[t] = 0;
			return Timeout0 + t;
		}
		ms = t < 0 ? -1 : (timers[t] - now) * 1000;
		pfd[0].fd = sock;
		pfd[1].fd = dlfd;
		pfd[0].events = pfd[1].events = POLLIN;
		pfd[0].revents = pfd[1].revents = 0;
		if (poll(pfd, dlfd >= 0 ? 2 : 1, ms) == -1) {
			if (errno == EINTR)
				continue;
			eprintf("poll:");
		}
		got = 0;
		if (pfd[0].revents) {
			memset(&bp, 0, sizeof(bp));
			udprecv(sock, &bp, sizeof(bp));
			got = 1;
		} else if (dlfd >= 0 && pfd[1].revents)
			got = dlrecv();
		if (got && bp.op[0] == Bootreply && !memcmp(bp.xid, xid, sizeof(xid))) {
			type = 0;
			optget(&bp, &type, ODtype, sizeof(type));
			return type;
		}
	}
}

static void
setenvip(char *name, unsigned char *ip)
{
	char *s = malloc(32);

	if (s) {
		sprintf(s, "%s=%s", name, ipstr(ip));
		putenv(s);
	}
}

static void
acceptlease(void)
{
	if (iflag)
		setip(client, mask, router);
	if (dflag)
		setdnsconf(dns);
	if (*program) {
		setenvip("SERVER", server);
		setenvip("CLIENT", client);
		setenvip("MASK", mask);
		setenvip("ROUTER", router);
		setenvip("DNS", dns);
		system(program);
	}
}

/* seconds from now; 0 turns timer n off */
static void
settimeout(int n, uint32_t secs)
{
	if (secs == 0 || secs > 0x10000000)
		timers[n] = 0;
	else
		timers[n] = time(NULL) + secs;
}

/* halfway to the expiration of timer n, minimum of 60 seconds */
static uint32_t
calctimeout(int n)
{
	long left = timers[n] - time(NULL);

	return left / 2 < 60 ? 60 : left / 2;
}

static void
giveup(int unused)
{
	(void)unused;
	fprintf(stderr, "%s: no lease on %s\n", argv0, ifname);
	exit(1);
}

static void
run(void)
{
	int forked = 0, t;
	long pid;
	uint32_t renewaltime, rebindingtime, lease;

	if (tflag) {
		signal(SIGALRM, giveup);
		alarm(tflag);
	}
Init:
	dlup();
	dhcpsend(DHCPdiscover, Broadcast);
	settimeout(0, 1);
	goto Selecting;
Selecting:
	for (;;) {
		switch (dhcprecv()) {
		case DHCPoffer:
			memcpy(client, bp.yiaddr, sizeof(client));
			optget(&bp, server, ODserverid, sizeof(server));
			goto Requesting;
		case Timeout0:
			goto Init;
		}
	}
Requesting:
	for (t = 4; t <= 64; t *= 2) {
		dhcpsend(DHCPrequest, Broadcast);
		settimeout(0, t);
		for (;;) {
			switch (dhcprecv()) {
			case DHCPack:
				goto Bound;
			case DHCPnak:
				goto Init;
			case Timeout0:
				break;
			default:
				continue;
			}
			break;
		}
	}
	/* no response from DHCPREQUEST after several attempts, go to INIT */
	goto Init;
Bound:
	memset(mask, 0, sizeof(mask));
	memset(router, 0, sizeof(router));
	memset(dns, 0, sizeof(dns));
	renewaltime = rebindingtime = lease = 0;
	optget(&bp, mask, OBmask, sizeof(mask));
	optget(&bp, router, OBrouter, sizeof(router));
	optget(&bp, dns, OBdnsserver, sizeof(dns));
	optget(&bp, &renewaltime, ODrenewaltime, sizeof(renewaltime));
	optget(&bp, &rebindingtime, ODrebindingtime, sizeof(rebindingtime));
	optget(&bp, &lease, ODlease, sizeof(lease));
	/* the options are big-endian, as this CPU is */
	if (!renewaltime)
		renewaltime = lease / 2;
	if (!rebindingtime)
		rebindingtime = lease / 8 * 7;
	alarm(0);
	dldown();
	acceptlease();
	if (!forked)
		printf("%s: %s netmask %s\n", ifname, ipstr(client), ipstr(mask));
	if (!fflag && !forked) {
		/* the parent returns once the lease file names the daemon */
		fflush(stdout);
		if ((pid = fork()) == -1)
			eprintf("fork:");
		if (pid) {
			savelease(lease, pid);
			exit(0);
		}
		setsid();
	} else
		savelease(lease, getpid());
	forked = 1;
	settimeout(0, renewaltime);
	settimeout(1, rebindingtime);
	settimeout(2, lease);
	for (;;) {
		switch (dhcprecv()) {
		case Timeout0: /* t1 elapsed */
			goto Renewing;
		case Timeout1: /* t2 elapsed */
			goto Rebinding;
		case Timeout2: /* lease expired */
			goto Init;
		}
	}
Renewing:
	dlup();
	dhcpsend(DHCPrequest, Unicast);
	settimeout(0, calctimeout(1));
	for (;;) {
		switch (dhcprecv()) {
		case DHCPack:
			goto Bound;
		case Timeout0: /* resend request */
			goto Renewing;
		case Timeout1: /* t2 elapsed */
			goto Rebinding;
		case Timeout2:
		case DHCPnak:
			goto Init;
		}
	}
Rebinding:
	settimeout(0, calctimeout(2));
	dhcpsend(DHCPrequest, Broadcast);
	for (;;) {
		switch (dhcprecv()) {
		case DHCPack:
			goto Bound;
		case Timeout0: /* resend request */
			goto Rebinding;
		case Timeout2: /* lease expired */
		case DHCPnak:
			goto Init;
		}
	}
}

static void
cleanexit(int unused)
{
	char path[64];

	(void)unused;
	dhcpsend(DHCPrelease, Unicast);
	sprintf(path, "/etc/dhcp/%s", ifname);
	unlink(path);
	_exit(0);
}

/* the station address, from the interface's DLPI driver */
static void
gethwaddr(void)
{
	char dev[64];
	long buf[64];
	physack_t *a = (physack_t *)buf;
	int fd;

	sprintf(dev, "/dev/%s", ifname);
	if ((fd = open(dev, O_RDWR)) == -1)
		eprintf("%s:", dev);
	buf[0] = DL_PHYS_ADDR_REQ;
	buf[1] = DL_CURR_PHYS_ADDR;
	if (dlctl(fd, buf, 2 * sizeof(long), sizeof(buf)) != DL_PHYS_ADDR_ACK ||
	    a->dl_addr_length != 6 || a->dl_addr_offset + 6 > sizeof(buf))
		eprintf("%s: no station address\n", dev);
	memcpy(hwaddr, (char *)buf + a->dl_addr_offset, 6);
	close(fd);
}

static void
usage(void)
{
	eprintf("usage: %s [-d] [-e program] [-f] [-i] [-t seconds] [ifname] [clientid]\n", argv0);
}

int
main(int argc, char *argv[])
{
	int bcast = 1;
	struct sockaddr_in addr;
	struct utsname u;
	unsigned long r;
	size_t i;

	ARGBEGIN {
	case 'd': /* don't update DNS in /etc/resolv.conf */
		dflag = 0;
		break;
	case 'e': /* run program */
		program = EARGF(usage());
		break;
	case 'f': /* run in foreground */
		fflag = 1;
		break;
	case 'i': /* don't set ip */
		iflag = 0;
		break;
	case 't': /* give up without a lease after this many seconds */
		tflag = atoi(EARGF(usage()));
		break;
	default:
		usage();
		break;
	} ARGEND;

	if (argc)
		ifname = argv[0]; /* interface name */
	if (argc >= 2)
		strncpy((char *)cid, argv[1], sizeof(cid)); /* client-id */

	signal(SIGTERM, cleanexit);

	if (uname(&u) == -1)
		eprintf("uname:");
	strncpy(hostname, u.nodename, sizeof(hostname) - 1);

	if (iflag)
		ifinit();
	gethwaddr();

	if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		eprintf("socket:");
	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&bcast, sizeof(bcast)) == -1)
		eprintf("setsockopt:");
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(68);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
		eprintf("bind:");
	if (!cid[0])
		memcpy(cid, hwaddr, sizeof(cid));

	r = time(NULL) ^ (getpid() << 16);
	for (i = 0; i < 6; i++)
		r = r * 31 + hwaddr[i];
	hnput(xid, r, sizeof(xid));

	starttime = time(NULL);
	run();

	return 0;
}
