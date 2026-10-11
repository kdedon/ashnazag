/*
 * fltool: the floppy service from the shell.
 *   fltool [-d drive] info [seconds]	status, once or each second
 *   fltool [-d drive] read track len	raw bytes to stdout
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "flio.h"

static void
show(fd)
	int fd;
{
	struct flinfo fi;

	if (ioctl(fd, FLIOC_INFO, &fi) < 0) {
		perror("FLIOC_INFO");
		exit(1);
	}
	printf("%s%s%s changes %lu, %lu tracks, %lu bytes a revolution\n",
	    fi.fi_flags & FLI_DISK ? "disk" : "no disk", fi.fi_flags & FLI_WPROT ? ", protected" : "",
	    fi.fi_flags & FLI_HD ? ", HD" : "", fi.fi_changes, fi.fi_tracks, fi.fi_rawlen);
	fflush(stdout);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char dev[32], *buf;
	struct flraw fr;
	int fd, drive = 0, n;

	if (argc > 2 && !strcmp(argv[1], "-d")) {
		drive = atoi(argv[2]);
		argc -= 2;
		argv += 2;
	}
	if (argc < 2)
		goto usage;
	sprintf(dev, "/dev/floppy%d", drive);
	if ((fd = open(dev, O_RDONLY)) < 0) {
		perror(dev);
		return 1;
	}
	if (!strcmp(argv[1], "info")) {
		n = argc > 2 ? atoi(argv[2]) : 0;
		do {
			show(fd);
			if (n)
				sleep(1);
		} while (n-- > 0);
		return 0;
	}
	if (!strcmp(argv[1], "read") && argc == 4) {
		fr.fr_track = atoi(argv[2]);
		fr.fr_len = atoi(argv[3]);
		fr.fr_flags = 0;
		if ((buf = malloc(fr.fr_len)) == 0)
			return 1;
		fr.fr_buf = buf;
		if (ioctl(fd, FLIOC_READ, &fr) < 0) {
			perror("FLIOC_READ");
			return 1;
		}
		fwrite(buf, 1, fr.fr_len, stdout);
		return 0;
	}
usage:
	fprintf(stderr, "usage: fltool [-d drive] info [seconds] | read track len\n");
	return 2;
}
