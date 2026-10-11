/*
 * xabusy.c -- XABUSY.PRG: a GEM program that spins without system calls
 * once C:\XABUSY.GO exists, writing 1 to C:\XABUSY.TXT first.  An alarm
 * set before it spins writes its signal to C:\XAALRM.TXT from the
 * handler.
 */
#include <gem.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static void
put(char *name, long n)
{
	char s[24];
	long fd;

	sprintf(s, "%ld\n", n);
	fd = Fcreate(name, 0);
	if (fd >= 0) {
		Fwrite((short)fd, strlen(s), s);
		Fclose((short)fd);
	}
}

static void
alrm(long sig)
{
	put("C:\\XAALRM.TXT", sig);
}

int
main(void)
{
	volatile unsigned long n = 0;

	if (appl_init() < 0)
		return 1;
	put("C:\\XABUSY.TXT", 0);
	while (Fattrib("C:\\XABUSY.GO", 0, 0) < 0)
		evnt_timer(250);
	Psignal(SIGALRM, (long)alrm);
	Talarm(2);
	put("C:\\XABUSY.TXT", 1);
	for (;;)
		n++;
}
