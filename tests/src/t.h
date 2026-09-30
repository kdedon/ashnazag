/*
 * t.h -- test result lines and helpers.
 *
 * One line per result: "PASS area.name", "FAIL area.name: why", "SKIP ...",
 * "INFO ...".  PASS/FAIL/SKIP also append P/F/S to COUNTFILE for the runner.
 * Written unbuffered so fork() never duplicates pending output.
 */
#ifndef T_H
#define T_H

#include <sys/types.h>

#define COUNTFILE	"/tmp/.tcount"

extern char *t_area;		/* area prefix, e.g. "proc" */
extern int t_nfail;

void t_init(char *area, int watchdog_secs);
void t_pass(char *name);
void t_fail(char *name, char *fmt, ...);
void t_skip(char *name, char *fmt, ...);
void t_info(char *name, char *fmt, ...);
int t_check(char *name, int ok, char *fmt, ...);	/* ok ? PASS : FAIL */
int t_done(void);			/* exit status: 1 if any FAIL */
void t_rearm(int secs);			/* reset the watchdog */
int t_waitchild(pid_t pid, int *status, int secs);	/* waitpid, SIGKILL on timeout */
int t_fill(int fd, int n, int seed);	/* write pattern, returns bytes */
int t_pattern(long off, int seed);	/* pattern byte at offset */
long t_kmem(char *sym);			/* kernel long at "sym" or "sym+off", -1 if unknown */
long t_now_ms(void);			/* gettimeofday in milliseconds */

#define T_ERR	strerror(errno)

#ifdef FMNAMESZ
/* struct str_mlist as the kernel's compiler lays it out: padded to 10 bytes */
struct k_mlist {
	char	l_name[FMNAMESZ + 1];
	char	l_pad;
};
#endif

#endif
