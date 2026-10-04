/*
 * t_otbst.c -- virtual Ethernet stations (/dev/otbstation) and the Mac
 * .ENET driver that uses them.
 *
 * Kernel: who may open (group 25, one station per user), a raw station B
 * with an explicit address: attach rules, source address enforcement, poll, SIGPOLL, local switching with the driver's
 * station and the host, statistics.
 *
 * Driver: the DRVR image (default /tests/n0n2/enet.drvr) is called
 * the way the Device Manager calls it (a0 parameter block, a1 DCE); its
 * Toolbox glue is pointed at stubs here, its protocol handlers are ours.
 * ARP to the host (answered locally) and to the user-network gateway
 * (answered over the SONIC) come back through ReadPacket/ReadRest.
 * Runs after t_net (aen0 is 10.0.2.15).
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mkdev.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include "sys/mod.h"
#include "otwire.h"
#include "t.h"

#define	MOD	"/tests/otb/otbridge"
#define	DRVR	"/tests/n0n2/enet.drvr"
#define	STDEV	"/dev/otbstation"

/* filled by the stubs and the protocol handler below */
long	ph_count, ph_d0in, iodone_n, iodone_d0, ph_end;
short	ph_rem, ph_rem2, ph_rpd3, ph_left;
char	ph_rpz;
unsigned char ph_hdr[14], ph_buf[1700];
long	callent(), h_newptr();
void	callvbl(), ph(), s_newptr(), s_disp(), s_vinst(), s_vrem(), s_iodone();

/*
 * callent(entry, pb, dce): a0 = pb, a1 = DCE, result word in d0.
 * (SVR4 assembler syntax: & marks immediates.)
 * Glue stubs: register conventions of the traps they stand for.
 * ph: a protocol handler; copies the header, ReadPacket 4, ReadRest.
 */
asm("
	.text
	.globl	callent
callent:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a2
	move.l	52(%sp),%a0
	move.l	56(%sp),%a1
	jsr	(%a2)
	ext.l	%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl	callvbl
callvbl:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	move.l	6(%a0),%a1
	jsr	(%a1)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl	s_newptr
s_newptr:
	movem.l	%d1/%a1,-(%sp)
	move.l	%d0,-(%sp)
	jsr	h_newptr
	addq.l	&4,%sp
	movem.l	(%sp)+,%d1/%a1
	move.l	%d0,%a0
	tst.l	%d0
	beq.s	1f
	moveq	&0,%d0
	rts
1:	moveq	&-108,%d0
	rts

	.globl	s_disp
s_disp:
	movem.l	%d1/%a0-%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	h_disp
	addq.l	&4,%sp
	movem.l	(%sp)+,%d1/%a0-%a1
	moveq	&0,%d0
	rts

	.globl	s_vinst
s_vinst:
	movem.l	%d1/%a0-%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	h_vinst
	addq.l	&4,%sp
	movem.l	(%sp)+,%d1/%a0-%a1
	moveq	&0,%d0
	rts

	.globl	s_vrem
s_vrem:
	movem.l	%d1/%a0-%a1,-(%sp)
	move.l	%a0,-(%sp)
	jsr	h_vrem
	addq.l	&4,%sp
	movem.l	(%sp)+,%d1/%a0-%a1
	moveq	&0,%d0
	rts

	.globl	s_iodone
s_iodone:
	move.l	%d0,iodone_d0
	addq.l	&1,iodone_n
	rts

	.globl	ph
ph:
	lea	-14(%a3),%a2
	lea	ph_hdr,%a5
	moveq	&13,%d2
1:	move.b	(%a2)+,(%a5)+
	dbra	%d2,1b
	move.w	%d1,ph_rem
	move.l	%d0,ph_d0in
	lea	ph_buf,%a3
	moveq	&4,%d3
	jsr	(%a4)
	seq	ph_rpz
	move.w	%d3,ph_rpd3
	move.w	%d1,ph_rem2
	move.w	&1600,%d3
	jsr	2(%a4)
	move.w	%d3,ph_left
	move.l	%a3,ph_end
	addq.l	&1,ph_count
	rts
");

static unsigned char *drv, pb[80];
static long dce[16], vbltask;
static int nvinst, nvrem, ndisp, nnew;
static int ph_want;

long
h_newptr(n)
	long n;
{
	nnew++;
	return (long)calloc(1, (size_t)n);
}

void h_disp(p) char *p; { ndisp++; free(p); }
void h_vinst(t) long t; { nvinst++; vbltask = t; }
void h_vrem(t) long t; { nvrem++; if (t == vbltask) vbltask = 0; }

static int
g16(p)
	unsigned char *p;
{
	return p[0] << 8 | p[1];
}

static void
p16(p, v)
	unsigned char *p;
	int v;
{
	p[0] = v >> 8;
	p[1] = v;
}

static void
p32(p, v)
	unsigned char *p;
	long v;
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* point the glue slots after 'AUXG' at the stubs */
static int
patch()
{
	static void (*stub[5])() = { s_newptr, s_disp, s_vinst, s_vrem, s_iodone };
	int i, k;

	for (i = 0; i < 256; i += 2)
		if (memcmp(drv + i, "AUXG", 4) == 0 && g16(drv + i + 4) == 5)
			break;
	if (i >= 256)
		return 0;
	for (i += 6, k = 0; k < 5; k++) {
		p16(drv + i + 8 * k, 0x4ef9);
		p32(drv + i + 8 * k + 2, (long)stub[k]);
	}
	return 1;
}

static long
entry(k)
	int k;
{
	return (long)drv + g16(drv + 8 + 2 * k);
}

/* Control call; queued (noq 0) completes through IODone */
static long
ctl(cs, noq)
	int cs, noq;
{
	p16(pb + 6, noq ? 0x0200 : 0);
	p16(pb + 26, cs);
	return callent(entry(2), pb, dce);
}

static long
attachph(type, h)
	int type;
	void (*h)();
{
	memset(pb, 0, sizeof pb);
	p16(pb + 28, type);
	p32(pb + 30, (long)h);
	return ctl(248, 1);
}

static unsigned char mymac[6], bmac[6] = { 0x02, 0xa5, 0x0b, 0x57, 0x00, 0x01 };
static unsigned char bcast[6] = { 255, 255, 255, 255, 255, 255 };

/* EWrite of an ARP request for ip, header and body in two pieces */
static long
arpreq(ip)
	unsigned long ip;
{
	static unsigned char h[14], a[28], wds[18];

	memcpy(h, bcast, 6);
	memset(h + 6, 0xee, 6);			/* the driver fills it */
	p16(h + 12, 0x0806);
	p16(a, 1); p16(a + 2, 0x0800); a[4] = 6; a[5] = 4; p16(a + 6, 1);
	memcpy(a + 8, mymac, 6);
	p32(a + 14, 0x0a000210L);		/* 10.0.2.16 */
	memset(a + 18, 0, 6);
	p32(a + 24, (long)ip);
	p16(wds, 14); p32(wds + 2, (long)h);
	p16(wds + 6, 28); p32(wds + 8, (long)a);
	p16(wds + 12, 0);
	memset(pb, 0, sizeof pb);
	p32(pb + 30, (long)wds);
	return ctl(246, 1);
}

/* tick the VBL task until the handler ran more than n times */
static int
waitph(n, ms)
	long n;
	int ms;
{
	long end = t_now_ms() + ms;

	while (ph_count <= n && t_now_ms() < end) {
		if (vbltask)
			callvbl(vbltask);
		poll((struct pollfd *)0, 0, 10);
	}
	return ph_count > n;
}

/* one frame from station fd within ms, or -1 */
static int
stread(fd, b, n, ms)
	int fd, n, ms;
	unsigned char *b;
{
	struct pollfd p;

	p.fd = fd;
	p.events = POLLIN;
	if (poll(&p, 1, ms) <= 0 || !(p.revents & POLLIN))
		return -1;
	return read(fd, b, n);
}

static unsigned char fb[1600];

static unsigned char *
frame(dst, src, type, len)
	unsigned char *dst, *src;
	int type, len;
{
	int i;

	memcpy(fb, dst, 6);
	memcpy(fb + 6, src, 6);
	p16(fb + 12, type);
	for (i = 14; i < len; i++)
		fb[i] = i;
	return fb;
}

/*
 * Open the station device twice as uid/gid in a child: 20 if the first
 * open works and the second is refused with EBUSY; 10 EACCES, 12 EPERM.
 */
static int
tryuser(uid, gid)
	int uid, gid;
{
	int pid, st, fd;

	if ((pid = fork()) == 0) {
		setgid(gid);
		setuid(uid);
		if ((fd = open(STDEV, O_RDWR)) < 0)
			_exit(errno == EACCES ? 10 : errno == EPERM ? 12 : 11);
		_exit(open(STDEV, O_RDWR) < 0 && errno == EBUSY ? 20 : 21);
	}
	if (pid < 0 || t_waitchild(pid, &st, 20) < 0 || !WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

static int npoll;
static void onpoll() { npoll++; signal(SIGPOLL, onpoll); }

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct mod_mreg reg;
	struct otb_station st;
	struct otb_stmulti sm;
	struct otb_ststats ss;
	int mj = 55, id, fd, bfd, n, dfd;
	long r, c0;
	unsigned char b[1600], info[18];
	char *path = argc > 1 ? argv[1] : DRVR;
	struct stat sb;

	t_init("otbst", 200);
	if (access(MOD, 0) < 0) {
		t_skip("all", "no otbridge module for this kernel");
		return t_done();
	}
	strcpy(reg.md_modname, "otbridge");
	reg.md_typedata = (caddr_t)&mj;
	(void)modadm(MOD_TY_CDEV, MOD_C_MREG, &reg);	/* t_otb may have done it */
	if ((id = modload(MOD)) < 0) {
		t_fail("load", "%s", T_ERR);
		return t_done();
	}
	if (access(STDEV, 0) < 0)
		mknod(STDEV, S_IFCHR | 0660, makedev(55, 1));
	chown(STDEV, 0, 25);
	chmod(STDEV, 0660);
	r = tryuser(100, 25);
	t_check("user_in_group", r == 20, "child %ld", r);
	r = tryuser(100, 26);
	t_check("user_mode_refused", r == 10, "child %ld", r);
	chmod(STDEV, 0666);
	r = tryuser(100, 26);
	t_check("user_policy_refused", r == 12, "child %ld", r);
	chmod(STDEV, 0660);

	/* ---- kernel: raw station B ---- */
	bfd = open(STDEV, O_RDWR);
	if (!t_check("open", bfd >= 0, "%s", T_ERR))
		goto unload;
	t_check("read_unattached", read(bfd, b, sizeof b) == 0, "%s", T_ERR);
	t_check("write_unattached", write(bfd, frame(bcast, bmac, 0x88b5, 60), 60) < 0 &&
	    errno == EINVAL, "%s", T_ERR);
	memset(&st, 0, sizeof st);
	st.st_mode = 2;
	t_check("mode_anchor_refused", ioctl(bfd, OTB_STATION, &st) < 0 && errno == EINVAL,
	    "%s", T_ERR);
	memcpy(st.st_mac, bmac, 6);
	st.st_mode = OTB_ST_BRIDGE;
	t_check("attach_explicit", ioctl(bfd, OTB_STATION, &st) == 0, "%s", T_ERR);
	t_check("attach_twice", ioctl(bfd, OTB_STATION, &st) < 0 && errno == EBUSY,
	    "%s", T_ERR);
	dfd = open(STDEV, O_RDWR);
	t_check("same_address_refused", dfd >= 0 && ioctl(dfd, OTB_STATION, &st) < 0 &&
	    errno == EBUSY, "%s", T_ERR);
	close(dfd);
	t_check("foreign_source_refused",
	    write(bfd, frame(bcast, mymac, 0x88b5, 60), 60) < 0 && errno == EINVAL, "%s", T_ERR);
	t_check("runt_refused", write(bfd, frame(bcast, bmac, 0x88b5, 60), 13) < 0 &&
	    errno == EINVAL, "%s", T_ERR);
	signal(SIGPOLL, onpoll);
	t_check("sigpoll_set", ioctl(bfd, OTB_STSIG, SIGPOLL) == 0, "%s", T_ERR);

	/* ---- driver ---- */
	if (stat(path, &sb) < 0 || (fd = open(path, O_RDONLY)) < 0) {
		t_fail("drvr", "%s: %s", path, T_ERR);
		goto unload;
	}
	drv = (unsigned char *)malloc((size_t)sb.st_size + 4);
	n = read(fd, drv, (unsigned)sb.st_size);
	close(fd);
	if (!t_check("drvr_image", n == sb.st_size && drv[18] == 5 &&
	    memcmp(drv + 19, ".ENET", 5) == 0 && g16(drv) == 0x4400, "%d bytes", n))
		goto unload;
	if (!t_check("glue", patch(), "no AUXG table"))
		goto unload;
	memset(pb, 0, sizeof pb);
	r = callent(entry(0), pb, dce);
	if (!t_check("open", r == 0 && dce[5] != 0 && nvinst == 1 && vbltask != 0,
	    "result %ld storage %lx vbl %d", r, dce[5], nvinst))
		goto unload;
	t_check("open_again", callent(entry(0), pb, dce) == 0 && nnew == 1, "%d allocations", nnew);
	memset(pb, 0, sizeof pb);
	memset(info, 0x55, sizeof info);
	p32(pb + 30, (long)info);
	p16(pb + 34, 18);
	c0 = iodone_n;
	r = ctl(252, 0);
	memcpy(mymac, info, 6);
	t_check("getinfo_queued", iodone_n == c0 + 1 && iodone_d0 == 0 && g16(pb + 16) == 0,
	    "iodone %ld d0 %ld", iodone_n - c0, iodone_d0);
	t_check("getinfo_address", info[0] == 2 && info[4] == 0 && info[5] == 0 &&
	    info[6] == 0 && info[17] == 0, "%02x:%02x:%02x:%02x:%02x:%02x",
	    info[0], info[1], info[2], info[3], info[4], info[5]);
	t_check("setgeneral", ctl(253, 1) == 0, "");
	t_check("read_unsupported", ctl(250, 1) == -17, "");
	t_check("attach_arp", attachph(0x0806, ph) == 0, "");
	t_check("attach_twice", attachph(0x0806, ph) == -94, "");

	/* ARP to the host: the reply is switched locally to the station */
	c0 = ph_count;
	r = arpreq(0x0a00020fL);
	t_check("write_arp_host", r == 0, "%ld", r);
	n = stread(bfd, b, sizeof b, 2000);
	t_check("station_to_station_broadcast", n == 42 && memcmp(b + 6, mymac, 6) == 0 &&
	    g16(b + 12) == 0x0806, "%d bytes", n);
	t_check("sigpoll", npoll > 0, "%d signals", npoll);
	if (t_check("arp_host_reply", waitph(c0, 3000) && g16(ph_hdr + 12) == 0x0806 &&
	    memcmp(ph_hdr, mymac, 6) == 0 && ph_buf[7] == 2 &&
	    memcmp(ph_buf + 14, "\012\000\002\017", 4) == 0, "%ld frames, op %d",
	    ph_count - c0, ph_buf[7])) {
		t_check("readpacket", ph_rpz && ph_rpd3 == 0 && ph_rem2 == ph_rem - 4,
		    "z %d d3 %d left %d of %d", ph_rpz, ph_rpd3, ph_rem2, ph_rem);
		t_check("readrest", ph_left == 1600 - ph_rem2 &&
		    ph_end == (long)ph_buf + 4 + ph_rem2 && ph_d0in == 0x0806,
		    "d3 %d, %d left", ph_left, ph_rem2);
		t_info("host_mac", "%02x:%02x:%02x:%02x:%02x:%02x",
		    ph_hdr[6], ph_hdr[7], ph_hdr[8], ph_hdr[9], ph_hdr[10], ph_hdr[11]);
	}
	/* ARP to the gateway: out through the SONIC, back by its CAM */
	c0 = ph_count;
	t_check("write_arp_gateway", arpreq(0x0a000202L) == 0, "");
	t_check("arp_gateway_reply", waitph(c0, 3000) && ph_buf[7] == 2 &&
	    memcmp(ph_buf + 14, "\012\000\002\002", 4) == 0 &&
	    memcmp(ph_hdr, mymac, 6) == 0, "%ld frames", ph_count - c0);
	while (stread(bfd, b, sizeof b, 50) > 0)
		;

	/* station B to the driver: unicast, 802.3, multicast */
	t_check("attach_local", attachph(0x88b5, ph) == 0, "");
	c0 = ph_count;
	t_check("b_to_driver", write(bfd, frame(mymac, bmac, 0x88b5, 100), 100) == 100 &&
	    waitph(c0, 2000) && ph_d0in == 0x88b5 && ph_rem == 86 &&
	    ph_buf[0] == 14 && ph_buf[85] == 99 && ph_left == 1600 - 82, "rem %d left %d",
	    ph_rem, ph_left);
	c0 = ph_count;
	(void)write(bfd, frame(mymac, bmac, 60, 74), 74);
	waitph(c0, 300);
	t_check("8023_without_handler", ph_count == c0, "");
	t_check("attach_8023", attachph(0, ph) == 0, "");
	c0 = ph_count;
	t_check("8023_frame", write(bfd, frame(mymac, bmac, 60, 74), 74) == 74 &&
	    waitph(c0, 2000) && g16(ph_hdr + 12) == 60, "");
	memset(pb, 0, sizeof pb);
	memcpy(pb + 28, "\011\000\007\377\377\377", 6);
	t_check("addmulti", ctl(245, 1) == 0, "");
	c0 = ph_count;
	t_check("multicast_in", write(bfd, frame(pb + 28, bmac, 0x88b5, 60), 60) == 60 &&
	    waitph(c0, 2000), "");
	t_check("delmulti", ctl(247, 1) == 0, "");
	c0 = ph_count;
	(void)write(bfd, frame((unsigned char *)"\011\000\007\377\377\377", bmac, 0x88b5, 60), 60);
	waitph(c0, 300);
	t_check("multicast_off", ph_count == c0, "");

	/* driver to B, errors, detach */
	{
		static unsigned char h[14], wds[12];

		memcpy(h, bmac, 6);
		p16(h + 12, 0x88b5);
		p16(wds, 14); p32(wds + 2, (long)h); p16(wds + 6, 0);
		memset(pb, 0, sizeof pb);
		p32(pb + 30, (long)wds);
		t_check("driver_to_b", ctl(246, 1) == 0 && stread(bfd, b, sizeof b, 2000) == 14 &&
		    memcmp(b + 6, mymac, 6) == 0, "");
		p16(wds, 10);
		t_check("write_runt", ctl(246, 1) == -92, "");
	}
	memset(pb, 0, sizeof pb);
	p16(pb + 28, 0x0806);
	t_check("detach_arp", ctl(249, 1) == 0, "");
	t_check("detach_again", ctl(249, 1) == -94, "");
	t_check("bad_cscode", ctl(1, 1) == -17, "");
	if (ioctl(bfd, OTB_STSTATS, &ss) == 0)
		t_info("b_stats", "rx %lu tx %lu drop %lu queued %lu",
		    ss.ss_rx, ss.ss_tx, ss.ss_drop, ss.ss_queued);
	memset(pb, 0, sizeof pb);
	r = callent(entry(4), pb, dce);
	t_check("close", r == 0 && dce[5] == 0 && nvrem == 1 && ndisp == 1 && vbltask == 0,
	    "result %ld", r);
	/* the driver's address is free again */
	dfd = open(STDEV, O_RDWR);
	memset(&st, 0, sizeof st);
	st.st_mode = OTB_ST_BRIDGE;
	t_check("address_released", dfd >= 0 && ioctl(dfd, OTB_STATION, &st) == 0 &&
	    memcmp(st.st_mac, mymac, 6) == 0, "%s", T_ERR);
	memcpy(sm.sm_addr, "\011\000\007\377\377\377", 6);
	sm.sm_on = 1;
	t_check("multi_ioctl", ioctl(dfd, OTB_STMULTI, &sm) == 0, "%s", T_ERR);
	close(dfd);
	close(bfd);
unload:
	t_check("unload", moduload(id) == 0, "%s", T_ERR);
	return t_done();
}
