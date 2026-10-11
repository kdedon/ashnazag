/*
 * xatick.c -- XATICK.PRG: a GEM program that keeps working while
 * others do.  It opens a window and, four times a second, counts in
 * its info line and in C:\XATICK.TXT, so a test reads from Unix that
 * it ran alongside the desktop; the last key it got goes to
 * C:\XAKEY.TXT, and keys t and m start C:\XAHELLO.TOS and C:\XAMINTP.TTP
 * as TOS programs.  A SIGUSR1 it sends itself at start, caught and
 * returned from, writes C:\XASIG.TXT.  Ends on AP_TERM or its close
 * box.
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

static volatile long caught;

static void
usr1(long sig)
{
	caught = sig;
}

int
main(void)
{
	short msg[8], wh, ev, d, key;
	char info[32];
	long n = 0;

	if (appl_init() < 0)
		return 1;
	graf_mouse(ARROW, 0);
	wh = wind_create(NAME | MOVER | CLOSER | INFO, 0, 0, 800, 600);
	if (wh <= 0) {
		appl_exit();
		return 1;
	}
	wind_set_str(wh, WF_NAME, " Ticker ");
	wind_set_str(wh, WF_INFO, " 0 ");
	wind_open(wh, 440, 380, 300, 120);
	put("C:\\XATICK.TXT", 0);
	Psignal(SIGUSR1, (long)usr1);
	Pkill(Pgetpid(), SIGUSR1);
	put("C:\\XASIG.TXT", caught);
	for (;;) {
		ev = evnt_multi(MU_MESAG | MU_TIMER | MU_KEYBD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		    0, 0, msg, 250, &d, &d, &d, &d, &key, &d);
		if (ev & MU_KEYBD) {
			put("C:\\XAKEY.TXT", key & 0xff);
			if ((key & 0xff) == 't')
				shel_write(1, 0, 0, "C:\\XAHELLO.TOS", "\0");
			if ((key & 0xff) == 'm')
				shel_write(1, 0, 0, "C:\\XAMINTP.TTP", "\0");
		}
		if (ev & MU_TIMER) {
			n++;
			sprintf(info, " %ld ", n);
			wind_set_str(wh, WF_INFO, info);
			put("C:\\XATICK.TXT", n);
		}
		if ((ev & MU_MESAG) && (msg[0] == AP_TERM || msg[0] == WM_CLOSED))
			break;
		if ((ev & MU_MESAG) && msg[0] == WM_MOVED)
			wind_set(wh, WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
		if ((ev & MU_MESAG) && msg[0] == WM_TOPPED)
			wind_set(wh, WF_TOP, wh, 0, 0, 0);
	}
	wind_close(wh);
	wind_delete(wh);
	appl_exit();
	return 0;
}
