/* fbbench: how fast can a user program push pixels into the AMIX display
 * (the Voodoo3 LFB through the Mediator on the A4000D)?  Acquires a front
 * session on /dev/fb, maps it, then times full-frame passes with byte, word
 * and long stores, long reads, libc memcpy from RAM and an aligned long copy.
 * Prints KB/s for each.  Build: m68k-cbm-sysv4-gcc -O -o fbbench fbbench.c */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dsio.h"

static struct fbinfo fi;
static int hz = 60;

static long ticks()
{
	struct tms t;
	return (long)times(&t);
}

static void report(s, bytes, t0, t1)
char *s; unsigned long bytes; long t0, t1;
{
	long dt = t1 - t0;
	if (dt < 1) dt = 1;
	printf("%-28s %8lu KB in %3ld.%02ld s = %6ld KB/s\n", s, bytes >> 10,
	    dt / hz, (dt % hz) * 100 / hz, (long)((bytes >> 10) * hz / dt));
}

int main(argc, argv)
int argc; char **argv;
{
	struct fbacq acq;
	int fd, passes = 4, pass, i;
	char *dev = "/dev/fb", *map;
	unsigned char *fb, *row;
	unsigned long n, rowbytes, h, bytes;
	long t0, t1, v;
	static unsigned long ram[2048];

	if (argc > 1) passes = atoi(argv[1]);
	if ((fd = open(dev, O_RDWR)) < 0 && (fd = open(dev = "/dev/fb0", O_RDWR)) < 0) {
		perror(dev); return 1;
	}
	memset((char *)&acq, 0, sizeof acq);
	acq.fa_kind = FBK_USER; acq.fa_flags = FBA_FRONT; strcpy(acq.fa_name, "fbbench");
	if (ioctl(fd, FBIOACQUIRE, &acq) < 0) { perror("FBIOACQUIRE"); return 1; }
	if (ioctl(fd, FBIOGINFO, &fi) < 0) { perror("FBIOGINFO"); return 1; }
	map = (char *)mmap((caddr_t)0, (size_t)fi.fi_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0);
	if (map == (char *)-1) { perror("mmap"); return 1; }
	v = sysconf(_SC_CLK_TCK); if (v > 0) hz = v;
	fb = (unsigned char *)map + fi.fi_offset;
	rowbytes = fi.fi_rowbytes; h = fi.fi_height;
	n = fi.fi_width * (fi.fi_depth / 8);		/* bytes per visible row */
	printf("%s: %lux%lu %lu-bit, rowbytes %lu, %lu bytes mapped, %d passes, %d Hz clock\n",
	    fi.fi_name, fi.fi_width, fi.fi_height, fi.fi_depth, rowbytes, fi.fi_size, passes, hz);
	bytes = n * h * passes;

	t0 = ticks();
	for (pass = 0; pass < passes; pass++)
		for (row = fb, i = 0; i < h; i++, row += rowbytes) {
			register unsigned char *p = row, *e = row + n;
			while (p < e) *p++ = (unsigned char)pass;
		}
	t1 = ticks(); report("byte stores", bytes, t0, t1);

	t0 = ticks();
	for (pass = 0; pass < passes; pass++)
		for (row = fb, i = 0; i < h; i++, row += rowbytes) {
			register unsigned short *p = (unsigned short *)row, *e = (unsigned short *)(row + n);
			while (p < e) *p++ = (unsigned short)pass * 0x101;
		}
	t1 = ticks(); report("word stores", bytes, t0, t1);

	t0 = ticks();
	for (pass = 0; pass < passes; pass++)
		for (row = fb, i = 0; i < h; i++, row += rowbytes) {
			register unsigned long *p = (unsigned long *)row, *e = (unsigned long *)(row + n), x = pass * 0x01010101UL;
			while (p + 4 <= e) { p[0] = x; p[1] = x; p[2] = x; p[3] = x; p += 4; }
			while (p < e) *p++ = x;
		}
	t1 = ticks(); report("long stores (x4 unrolled)", bytes, t0, t1);

	t0 = ticks(); v = 0;
	for (pass = 0; pass < passes; pass++)
		for (row = fb, i = 0; i < h; i++, row += rowbytes) {
			register unsigned long *p = (unsigned long *)row, *e = (unsigned long *)(row + n);
			while (p < e) v += *p++;
		}
	t1 = ticks(); report("long reads", bytes, t0, t1);

	t0 = ticks();
	for (pass = 0; pass < passes; pass++)
		for (row = fb, i = 0; i < h; i++, row += rowbytes)
			memcpy((char *)row, (char *)ram, n > sizeof ram ? sizeof ram : n);
	t1 = ticks(); report("libc memcpy RAM->fb", bytes, t0, t1);

	t0 = ticks();
	for (pass = 0; pass < passes; pass++)
		for (row = fb, i = 0; i < h; i++, row += rowbytes) {
			register unsigned long *p = (unsigned long *)row, *e = (unsigned long *)(row + n), *s = ram;
			while (p + 4 <= e) { p[0] = s[0]; p[1] = s[1]; p[2] = s[2]; p[3] = s[3]; p += 4; s += 4; }
		}
	t1 = ticks(); report("long copy RAM->fb (x4)", bytes, t0, t1);

	t0 = ticks();
	for (pass = 0; pass < passes; pass++)
		for (row = fb, i = 0; i < h; i++, row += rowbytes)
			memcpy((char *)row, (char *)(row + rowbytes * (i < h / 2 ? h / 2 : -(long)(h / 2))), n);
	t1 = ticks(); report("libc memcpy fb->fb", bytes, t0, t1);

	for (row = fb, i = 0; i < h; i++, row += rowbytes) memset((char *)row, 0, n);
	printf("sum %lx\n", v);
	munmap((caddr_t)map, (size_t)fi.fi_size);
	close(fd);
	return 0;
}
