/*
 * xamintp.c -- XAMINTP.TTP: a MiNTLib command-line program, run as a
 * Unix process from the console window.  Writes its pid to XAMINTP.PID,
 * reads a line and writes it to XAMINTP.TXT, both in its directory, and
 * ends with 3.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(void)
{
	char line[80];
	FILE *f;

	if ((f = fopen("XAMINTP.PID", "w")) != 0) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
	printf("MiNT program, pid %d\nLine? ", (int)getpid());
	fflush(stdout);
	if (!fgets(line, sizeof line, stdin))
		return 1;
	line[strcspn(line, "\r\n")] = 0;
	if ((f = fopen("XAMINTP.TXT", "w")) != 0) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
	printf("Got '%s'\n", line);
	return 3;
}
