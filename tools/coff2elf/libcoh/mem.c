/*
 * mem.c -- libcoh-linux OS-interface glue the Coherent libc memory code needs:
 * sbrk (the Linux-backed break primitive), plus abort/ulimit stubs.  The actual
 * malloc/free/realloc are the REAL Coherent libc (stdlib/malloc/*.c), which the
 * since-deleted i386 harness compiled with cc2-i386 and archived alongside this.
 *
 * Bootstrap form: host gcc -m32 (standard cdecl ELF, ABI-compatible with MWC i386
 * output).  MWC-compiled at self-host.
 */

extern char *_lbrk();		/* Linux brk syscall: _lbrk(addr) -> new break */
extern int write();

#define	BADSBRK	((char *)-1)

char *
sbrk(incr)
int incr;
{
	char *cur, *want;

	cur = _lbrk(0);		/* query current break */
	want = cur + incr;
	if (_lbrk(want) != want)
		return (BADSBRK);
	return (cur);
}

/* ulimit(3) -> process memory cap; -1 means "unknown / no limit" (malloc skips
 * the cap).  Linux has no ulimit() syscall; -1 is the safe answer. */
long
ulimit(cmd)
int cmd;
{
	return (-1L);
}

abort()
{
	write(2, "abort\n", 6);
	_exit(134);		/* 128 + SIGABRT */
}

int	errno;			/* the libc error global */

/* isatty: stdio uses it to pick buffering.  0 => not a tty => fully buffered
 * (flushed at exit via crt0's exit() -> _finish()).  A real ioctl-based isatty
 * can replace this when interactive line-buffering matters. */
isatty(fd)
int fd;
{
	return (0);
}
