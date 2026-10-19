/* fbmode: list the display's modes, or show a front session in one.
 *   fbmode               the modes
 *   fbmode W H D [s [k]] request WxHxD, draw a frame, hold s seconds
 *                        (default 20); k: keep it with FBIOMODEOK
 * Prints what was granted and FBIOGINFO before and after the hold, so a
 * trial that ran out shows as a changed mode. */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dsio.h"

static void info(fd, why)
int fd; char *why;
{
	struct fbinfo fi;

	if (ioctl(fd, FBIOGINFO, &fi) < 0) {
		perror("FBIOGINFO");
		return;
	}
	printf("%s: mode 0x%lx %lux%lu depth %lu rows %lu offset 0x%lx size %lu\n", why,
	    fi.fi_mode, fi.fi_width, fi.fi_height, fi.fi_depth, fi.fi_rowbytes, fi.fi_offset, fi.fi_size);
}

/* a border and a diagonal in the granted screen */
static void draw(fd)
int fd;
{
	struct fbinfo fi;
	unsigned char *p, *q;
	unsigned long x, y, b;

	if (ioctl(fd, FBIOGINFO, &fi) < 0)
		return;
	p = (unsigned char *)mmap(0, fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == (unsigned char *)-1) {
		perror("mmap");
		return;
	}
	b = fi.fi_depth / 8;
	for (y = 0; y < fi.fi_height; y++) {
		q = p + fi.fi_offset + y * fi.fi_rowbytes;
		for (x = 0; x < fi.fi_width; x++)
			memset(q + x * b, x < 4 || y < 4 || x >= fi.fi_width - 4 || y >= fi.fi_height - 4 ||
			    x * fi.fi_height / fi.fi_width == y ? 0xff : 0x00, b);
	}
}

int main(argc, argv)
int argc; char **argv;
{
	struct fbmodeinfo mi[32];
	struct fbmodes ms;
	struct fbacq a;
	struct fbreq rq;
	unsigned long i;
	int fd = open("/dev/fb0", O_RDWR);

	if (fd < 0) {
		perror("/dev/fb0");
		return 1;
	}
	if (argc < 4) {
		ms.ms_count = 32;
		ms.ms_modes = mi;
		if (ioctl(fd, FBIOGMODES, &ms) < 0) {
			perror("FBIOGMODES");
			return 1;
		}
		for (i = 0; i < ms.ms_count && i < 32; i++)
			printf("0x%04lx %4lux%-4lu %2lu bits %2lu Hz rows %5lu%s%s%s\n", mi[i].mi_id,
			    mi[i].mi_width, mi[i].mi_height, mi[i].mi_depth, mi[i].mi_hz, mi[i].mi_rowbytes,
			    mi[i].mi_flags & FBM_CURRENT ? " current" : "",
			    mi[i].mi_flags & FBM_TRIED ? " tried" : "",
			    mi[i].mi_flags & FBM_SCALE2 ? " scale2" : "");
		return 0;
	}
	memset(&a, 0, sizeof a);
	a.fa_kind = FBK_USER;
	a.fa_flags = FBA_FRONT;
	strcpy(a.fa_name, "fbmode");
	if (ioctl(fd, FBIOACQUIRE, &a) < 0) {
		perror("FBIOACQUIRE");
		return 1;
	}
	memset(&rq, 0, sizeof rq);
	rq.rq_width = atol(argv[1]);
	rq.rq_height = atol(argv[2]);
	rq.rq_depth = atol(argv[3]);
	if (ioctl(fd, FBIOREQMODE, &rq) < 0) {
		perror("FBIOREQMODE");
		return 1;
	}
	printf("granted 0x%lx how %lu at %lu,%lu\n", rq.rq_id, rq.rq_how, rq.rq_x, rq.rq_y);
	info(fd, "shown");
	draw(fd);
	if (argc > 5 && ioctl(fd, FBIOMODEOK, 0) < 0)
		perror("FBIOMODEOK");
	sleep(argc > 4 ? atoi(argv[4]) : 20);
	info(fd, "after");
	return 0;
}
