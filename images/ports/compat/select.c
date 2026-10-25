/* libc.so.1 exports select(2) only under its ABI name. */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>

extern int _abi_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);

int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
{
	return _abi_select(n, r, w, e, t);
}
