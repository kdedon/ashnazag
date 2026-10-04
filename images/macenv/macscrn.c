/*
 * macscrn -- fit a System file's 'scrn' resource to the screen.  The Mac
 * applies 'scrn' (depth, colour or greys) only when its base address and
 * its rectangle's size match the screen's, so the entry for the A/UX
 * screen (slot $E) gets the display's.
 *
 *	macscrn %System
 *
 * Exit 0: fitted or already right; 1: no display, no such entry, or an
 * unreadable file.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "dsio.h"

#define	G16(p)	((unsigned)((p)[0] << 8 | (p)[1]))
#define	G32(p)	((unsigned long)G16(p) << 16 | G16((p) + 2))

static int fd;

static int
at(off, b, n)
	long off;
	unsigned char *b;
	int n;
{
	return lseek(fd, (off_t)off, 0) != -1 && read(fd, (char *)b, n) == n;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct fbinfo fi;
	unsigned char b[512], *e;
	long fork = -1, map, data, o;
	unsigned long base;
	unsigned n, i, k, len, w, h;
	int f;

	if (argc != 2) {
		fprintf(stderr, "usage: macscrn %%System\n");
		return 2;
	}
	if ((f = open("/dev/fb0", O_RDONLY)) < 0 || ioctl(f, FBIOGINFO, &fi) < 0)
		return 1;
	close(f);
	if ((fd = open(argv[1], O_RDWR)) < 0 || !at(0L, b, 26))
		return 1;
	/* AppleDouble entry 2: the resource fork */
	for (i = 0, n = G16(b + 24); i < n && i < 16; i++) {
		if (!at(26L + 12 * i, b, 12))
			return 1;
		if (G32(b) == 2)
			fork = G32(b + 4);
	}
	if (fork < 0 || !at(fork, b, 16))
		return 1;
	data = fork + G32(b);
	map = fork + G32(b + 4);
	if (!at(map + 24, b, 2))
		return 1;
	o = map + G16(b);
	if (!at(o, b, 2))
		return 1;
	for (i = 0, n = G16(b) + 1; i < n; i++) {
		if (!at(o + 2 + 8 * i, b, 8))
			return 1;
		if (memcmp(b, "scrn", 4) == 0)
			break;
	}
	if (i == n)
		return 1;
	/* id 0 of the type's references */
	for (k = 0, n = G16(b + 4) + 1, o += G16(b + 6); k < n; k++) {
		if (!at(o + 12 * k, b, 12))
			return 1;
		if (G16(b) == 0)
			break;
	}
	if (k == n)
		return 1;
	o = data + (G32(b + 4) & 0xffffff);
	if (!at(o, b, 4) || (len = G32(b)) > sizeof b || !at(o + 4, b, (int)len))
		return 1;
	/* count, then per screen 28 bytes and its control calls */
	for (i = 0, k = 2, n = G16(b); i < n && k + 28 <= len; i++) {
		e = b + k;
		if (G16(e) == 0x7a && G16(e + 2) == 0xe) {
			/* base at 4; rectangle at 18: top, left, bottom, right */
			base = 0xe0000000 + fi.fi_offset;
			h = G16(e + 18) + fi.fi_height;
			w = G16(e + 20) + fi.fi_width;
			if (G32(e + 4) == base && G16(e + 22) == h && G16(e + 24) == w)
				return 0;
			e[4] = base >> 24;
			e[5] = base >> 16;
			e[6] = base >> 8;
			e[7] = base;
			e[22] = h >> 8;
			e[23] = h;
			e[24] = w >> 8;
			e[25] = w;
			return lseek(fd, o + 4 + k + 4, 0) == -1 || write(fd, (char *)e + 4, 22) != 22;
		}
		for (f = G16(e + 26), k += 28; f > 0 && k + 4 <= len; f--)
			k += 4 + G16(b + k + 2);
	}
	return 1;
}
