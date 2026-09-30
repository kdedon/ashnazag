/*
 * exit.c -- libcoh-linux fallback exit().  crt0 calls exit(status) so stdio gets
 * flushed.  When the real Coherent stdio is linked, ITS exit() (stdio/exit.c,
 * which runs atexit handlers + _finish() to flush streams) resolves the symbol
 * first and this one is never pulled.  For programs WITHOUT stdio, this minimal
 * exit (just the _exit syscall -- nothing to flush) resolves it.
 *
 * Kept in its OWN object so it is pulled only when nothing else defines exit
 * (avoids a multiple-definition clash with stdio/exit.o).
 *
 * Bootstrap form: host gcc -m32.
 */

extern int _exit();

void
exit(status)
int status;
{
	_exit(status);
}
