/*
 * setclk -- set the system time from the RTC; -s sets the RTC from the
 * system time.  The RTC keeps local time in the zone of /etc/TIMEZONE,
 * as TOS does.  date(1) runs "setclk -s".
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

extern int putenv(), stime(), open(), read(), write();
extern void tzset();
extern long timezone, altzone;

static char tz[128] = "TZ=GMT0";

static void
zone()
{
	FILE *f;
	char l[sizeof tz], *p;

	if ((f = fopen("/etc/TIMEZONE", "r")) != NULL) {
		while (fgets(l, sizeof l, f) != NULL)
			if (strncmp(l, "TZ=", 3) == 0) {
				if ((p = strchr(l, '\n')) != NULL)
					*p = '\0';
				strcpy(tz, l);
			}
		fclose(f);
	}
	putenv(tz);
	tzset();
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct tm *tm;
	long t;
	int fd, set;

	set = argc == 2 && strcmp(argv[1], "-s") == 0;
	if (argc > 1 && !set) {
		fprintf(stderr, "usage: setclk [-s]\n");
		return 2;
	}
	zone();
	if ((fd = open("/dev/clock", set ? O_WRONLY : O_RDONLY)) < 0) {
		perror("setclk: /dev/clock");
		return 1;
	}
	if (set) {
		t = time((long *)0);
		tm = localtime(&t);
		t -= tm->tm_isdst ? altzone : timezone;
		if (write(fd, (char *)&t, 4) != 4) {
			perror("setclk: /dev/clock");
			return 1;
		}
		return 0;
	}
	if (read(fd, (char *)&t, 4) != 4) {
		perror("setclk: /dev/clock");
		return 1;
	}
	tm = gmtime(&t);
	tm->tm_isdst = -1;
	if ((t = mktime(tm)) == -1 || stime(&t) < 0) {
		perror("setclk");
		return 1;
	}
	return 0;
}
