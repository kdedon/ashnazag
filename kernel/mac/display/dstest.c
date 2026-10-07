/*
 * dstest -- exercise the display service.
 *
 *   dstest info                  display, modes, VBL count, keyboard
 *   dstest draw [-s seed] [-c steps] [-t secs] [-b] [-w|-u] [-n name] [-m mode]
 *                                session (in mode): pattern, CLUT, then steps CLUT
 *                                rotations one VBL apart; hold secs (0:
 *                                until killed), printing notes; -b stays
 *                                in the background; -u maps cache-inhibited
 *   dstest events [-t secs] [-n count]
 *                                session in front, print key and mouse
 *                                events
 *   dstest switch id             bring session id (0: console) to front
 *   dstest state
 *
 * The pattern and palette are dspat.h's, so screen dumps can be checked.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "dspat.h"

static char *fbdev = "/dev/fb0";

static void
die(what)
char *what;
{
	fprintf(stderr, "dstest: %s: %s\n", what, strerror(errno));
	exit(1);
}

static int
fbopen()
{
	int fd = open(fbdev, O_RDWR);

	if (fd < 0)
		die(fbdev);
	return fd;
}

static int
acquire(fd, front, name)
int fd, front;
char *name;
{
	struct fbacq a;

	memset(&a, 0, sizeof a);
	a.fa_kind = FBK_USER;
	a.fa_flags = front ? FBA_FRONT : 0;
	strncpy(a.fa_name, name, sizeof a.fa_name - 1);
	if (ioctl(fd, FBIOACQUIRE, &a) < 0)
		die("FBIOACQUIRE");
	return (int)a.fa_id;
}

static void
setcmap(fd, fi, seed, k)
int fd;
struct fbinfo *fi;
unsigned long seed, k;
{
	static unsigned short r[256], g[256], b[256];
	struct fbcmap cm;

	if (fi->fi_cmapsize == 0)
		return;
	dspat_cmap(seed, k, fi->fi_cmapsize, r, g, b);
	cm.cm_start = 0;
	cm.cm_count = fi->fi_cmapsize;
	cm.cm_red = r;
	cm.cm_green = g;
	cm.cm_blue = b;
	if (ioctl(fd, FBIOPUTCMAP, &cm) < 0)
		die("FBIOPUTCMAP");
}

static int
info()
{
	struct fbinfo fi;
	struct fbmodeinfo mi[16];
	struct fbmodes ms;
	struct fbstate st;
	struct evinfo ei;
	unsigned long vbl, i;
	int fd = fbopen(), kfd;

	if (ioctl(fd, FBIOGINFO, &fi) < 0)
		die("FBIOGINFO");
	printf("%.16s type %lu %lux%lu depth %lu row %lu visual %lu cmap %lu/%lu\n",
	    fi.fi_name, fi.fi_type, fi.fi_width, fi.fi_height, fi.fi_depth,
	    fi.fi_rowbytes, fi.fi_visual, fi.fi_cmapsize, fi.fi_cmapbits);
	printf("map offset 0x%lx size 0x%lx mode 0x%lx flags 0x%lx\n",
	    fi.fi_offset, fi.fi_size, fi.fi_mode, fi.fi_flags);
	ms.ms_count = 16;
	ms.ms_modes = mi;
	if (ioctl(fd, FBIOGMODES, &ms) < 0)
		die("FBIOGMODES");
	for (i = 0; i < ms.ms_count && i < 16; i++)
		printf("mode 0x%lx %lux%lu depth %lu row %lu offset 0x%lx%s\n",
		    mi[i].mi_id, mi[i].mi_width, mi[i].mi_height, mi[i].mi_depth,
		    mi[i].mi_rowbytes, mi[i].mi_offset,
		    (mi[i].mi_flags & FBM_CURRENT) ? " current" : "");
	if (ioctl(fd, FBIOGSTATE, &st) < 0 || ioctl(fd, FBIOGVBL, &vbl) < 0)
		die("FBIOGSTATE");
	printf("front %ld serial %lu vbl %lu\n", st.st_front, st.st_serial, vbl);
	if ((kfd = open("/dev/kbd", O_RDONLY)) >= 0 && ioctl(kfd, EVIOCGINFO, &ei) == 0)
		printf("keyboard: key set %lu handler %lu flags 0x%lx\n",
		    ei.ei_kset, ei.ei_id, ei.ei_flags);
	return 0;
}

static int
draw(argc, argv)
int argc;
char **argv;
{
	struct fbinfo fi;
	struct fbnote n;
	struct pollfd p;
	unsigned long seed = 0, steps = 0, mode = 0, k;
	long secs = 0, t0;
	int c, fd, id, front = 1, cache = 0;
	char *name = "dstest";
	unsigned char *fb;

	while ((c = getopt(argc, argv, "s:c:t:bwun:m:")) != -1)
		switch (c) {
		case 's': seed = strtoul(optarg, 0, 0); break;
		case 'c': steps = strtoul(optarg, 0, 0); break;
		case 't': secs = strtol(optarg, 0, 0); break;
		case 'b': front = 0; break;
		case 'w': cache = FBC_WT; break;
		case 'u': cache = FBC_CI; break;
		case 'n': name = optarg; break;
		case 'm': mode = strtoul(optarg, 0, 0); break;
		default: return 2;
		}
	fd = fbopen();
	id = acquire(fd, front, name);
	if (mode && ioctl(fd, FBIOSMODE, mode) < 0)
		die("FBIOSMODE");
	if (ioctl(fd, FBIOGINFO, &fi) < 0)
		die("FBIOGINFO");
	if (cache && ioctl(fd, FBIOCACHE, cache) < 0)
		die("FBIOCACHE");
	fb = (unsigned char *)mmap(0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == (unsigned char *)-1)
		die("mmap");
	setcmap(fd, &fi, seed, 0UL);
	dspat_draw(fb, &fi, seed);
	printf("dstest: session %d drawn, seed %lu, %lux%lu depth %lu mode 0x%lx\n",
	    id, seed, fi.fi_width, fi.fi_height, fi.fi_depth, fi.fi_mode);
	fflush(stdout);
	for (k = 1; k <= steps; k++) {
		if (ioctl(fd, FBIOVBLWAIT, 1) < 0)
			die("FBIOVBLWAIT");
		setcmap(fd, &fi, seed, k);
	}
	if (steps) {
		printf("dstest: CLUT rotated %lu\n", steps);
		fflush(stdout);
	}
	t0 = time(0);
	p.fd = fd;
	p.events = POLLIN;
	while (secs == 0 || time(0) - t0 < secs) {
		if (poll(&p, 1, 500) <= 0)
			continue;
		if (read(fd, &n, sizeof n) != sizeof n)
			break;
		printf("dstest: session %d %s, serial %lu, pattern %s\n", id,
		    n.fn_type == FBN_HIDDEN ? "hidden" : n.fn_type == FBN_SHOWN ? "shown" : "note",
		    n.fn_serial, dspat_check(fb, &fi, seed) ? "damaged" : "intact");
		fflush(stdout);
	}
	return 0;
}

static int
events(argc, argv)
int argc;
char **argv;
{
	struct pollfd p[2];
	struct inev v;
	long secs = 10, t0, count = 0, got = 0;
	int c, fd, i;

	while ((c = getopt(argc, argv, "t:n:")) != -1)
		switch (c) {
		case 't': secs = strtol(optarg, 0, 0); break;
		case 'n': count = strtol(optarg, 0, 0); break;
		default: return 2;
		}
	fd = fbopen();
	(void)acquire(fd, 1, "events");
	p[0].fd = open("/dev/kbd", O_RDONLY);
	p[1].fd = open("/dev/mouse", O_RDONLY);
	if (p[0].fd < 0 || p[1].fd < 0)
		die("/dev/kbd, /dev/mouse");
	for (i = 0; i < 2; i++) {
		p[i].events = POLLIN;
		if (ioctl(p[i].fd, EVIOCBIND, fd) < 0)
			die("EVIOCBIND");
	}
	t0 = time(0);
	while (time(0) - t0 < secs && (count == 0 || got < count)) {
		if (poll(p, 2, 500) <= 0)
			continue;
		for (i = 0; i < 2; i++) {
			if (!(p[i].revents & POLLIN) || read(p[i].fd, &v, sizeof v) != sizeof v)
				continue;
			got++;
			printf("%ld.%06ld %s code 0x%02x value %ld\n", v.ie_sec, v.ie_usec,
			    v.ie_type == IE_KEY ? "key" : v.ie_type == IE_REL ? "rel" :
			    v.ie_type == IE_BTN ? "btn" : v.ie_type == IE_SYN ? "syn" :
			    v.ie_type == IE_DROP ? "drop" : "?", v.ie_code, v.ie_value);
			fflush(stdout);
		}
	}
	return 0;
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct fbstate st;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: dstest info | draw [-s seed] [-c steps] [-t secs] [-b] [-w|-u] [-n name] [-m mode] | events [-t secs] [-n count] | switch id | state\n");
		return 2;
	}
	if (strcmp(argv[1], "info") == 0)
		return info();
	if (strcmp(argv[1], "draw") == 0)
		return draw(argc - 1, argv + 1);
	if (strcmp(argv[1], "events") == 0)
		return events(argc - 1, argv + 1);
	fd = fbopen();
	if (strcmp(argv[1], "switch") == 0 && argc == 3) {
		if (ioctl(fd, FBIOSWITCH, atoi(argv[2])) < 0)
			die("FBIOSWITCH");
		return 0;
	}
	if (strcmp(argv[1], "state") == 0) {
		if (ioctl(fd, FBIOGSTATE, &st) < 0)
			die("FBIOGSTATE");
		printf("front %ld serial %lu\n", st.st_front, st.st_serial);
		return 0;
	}
	fprintf(stderr, "dstest: unknown command %s\n", argv[1]);
	return 2;
}
