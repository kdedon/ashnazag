/*
 * usbtool - the usb module's control device, /dev/usb (ubioc.h):
 *
 *   usbtool start            set the stack going, poll once
 *   usbtool poll [N]         poll the hubs N times, half a second apart
 *   usbtool devs             the devices
 *   usbtool stor             the mass storage devices
 *   usbtool read UNIT LBA N  N blocks from LBA of storage UNIT, hex dump
 *   usbtool dd UNIT LBA N    the same, raw, to stdout
 *   usbtool daemon           start, then poll every half second (ubd)
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include "ubioc.h"

/* Only printf, perror and system calls from the C library, so that the
 * same source serves Atari System V, whose runtime libc lays FILE out
 * differently from the cross headers; AMIX would not mind. */
#include <poll.h>
static unsigned char rwbuf[UB_RWMAX];

static unsigned long
number(s)
char *s;
{
	unsigned long v = 0;
	int base = 10;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		base = 16, s += 2;
	for (; *s; s++) {
		int c = *s >= 'a' ? *s - 'a' + 10 : *s >= 'A' ? *s - 'A' + 10 : *s - '0';
		if (c < 0 || c >= base)
			break;
		v = v * base + c;
	}
	return v;
}

static int fd;

static void
fail(what)
char *what;
{
	perror(what);
	exit(1);
}

static void
devs()
{
	static struct ub_devinfo d[UB_NDEV];
	int i;

	if (ioctl(fd, UBIOC_DEVS, d) < 0)
		fail("UBIOC_DEVS");
	for (i = 0; i < UB_NDEV; i++)
		if (d[i].present)
			printf("dev %d: addr %d  %04x:%04x  class %d  \"%s\" \"%s\" \"%s\"%s\n", i,
			    d[i].addr, d[i].vid, d[i].pid, d[i].class, d[i].mf, d[i].prod, d[i].serial,
			    d[i].hub ? " (hub)" : "");
}

static void
stor()
{
	static struct ub_storinfo s[UB_NSTOR];
	int i;

	if (ioctl(fd, UBIOC_STOR, s) < 0)
		fail("UBIOC_STOR");
	for (i = 0; i < UB_NSTOR; i++)
		if (s[i].present)
			printf("stor %d: dev %d  %s %s %s  %lu blocks of %lu (%lu MB)%s%s\n", i, s[i].devaddr,
			    s[i].vendor, s[i].product, s[i].rev, s[i].blocks, s[i].blocksize,
			    s[i].blocks / 2048 * s[i].blocksize / 512,
			    s[i].removable ? ", removable" : "", s[i].ready ? "" : ", not ready");
}

static void
read_blocks(unit, lba, n, raw)
int unit, raw;
unsigned long lba, n;
{
	struct ub_rw rw;
	unsigned char *buf;
	unsigned long k;

	if (n * 512 > UB_RWMAX) {
		printf("at most %d blocks\n", UB_RWMAX / 512);
		return;
	}
	buf = rwbuf;
	rw.unit = unit;
	rw.lba = lba;
	rw.n = n;
	rw.buf = (char *)buf;
	if (ioctl(fd, UBIOC_RD, &rw) < 0)
		fail("UBIOC_RD");
	if (raw) {
		write(1, buf, n * 512);
		return;
	}
	for (k = 0; k < n * 512; k++) {
		if (k % 16 == 0)
			printf("%06lx:", lba * 512 + k);
		printf(" %02x", buf[k]);
		if (k % 16 == 15)
			printf("\n");
	}
}

int
main(argc, argv)
int argc;
char **argv;
{
	int n, i, r;

	if (argc < 2) {
		printf("usage: usbtool start|poll [N]|devs|stor|read UNIT LBA N|dd UNIT LBA N|daemon\n");
		return 2;
	}
	if ((fd = open("/dev/usb", O_RDWR)) < 0)
		fail("/dev/usb");
	if (strcmp(argv[1], "start") == 0) {
		if (ioctl(fd, UBIOC_START, 0) < 0)
			fail("UBIOC_START");
		devs();
	} else if (strcmp(argv[1], "poll") == 0) {
		n = argc > 2 ? (int)number(argv[2]) : 1;
		for (i = 0; i < n; i++) {
			if (ioctl(fd, UBIOC_POLL, 0) < 0)
				fail("UBIOC_POLL");
			if (i + 1 < n)
				poll((struct pollfd *)0, 0, 1000);
		}
		devs();
	} else if (strcmp(argv[1], "devs") == 0)
		devs();
	else if (strcmp(argv[1], "stor") == 0)
		stor();
	else if ((strcmp(argv[1], "read") == 0 || strcmp(argv[1], "dd") == 0) && argc == 5)
		read_blocks((int)number(argv[2]), number(argv[3]), number(argv[4]),
		    argv[1][0] == 'd');
	else if (strcmp(argv[1], "daemon") == 0) {
		if (ioctl(fd, UBIOC_START, 0) < 0)
			fail("UBIOC_START");
		for (;;) {
			poll((struct pollfd *)0, 0, 500);
			if (ioctl(fd, UBIOC_POLL, 0) < 0)
				fail("UBIOC_POLL");
		}
	} else {
		r = ioctl(fd, UBIOC_STATUS, 0);
		printf("state %d\n", r);
	}
	return 0;
}
