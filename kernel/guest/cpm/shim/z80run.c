/* The run loop.  z80runa.s is the target's copy, with the frequent
 * classes executed in assembly; ZRUNT compares the two on the machine. */

#include "z80.h"

int z80run(m, n, k)
struct z80 *m;
long n;
long *k;
{
	struct z80in in;
	register int rc;
	long i;

	rc = X_OK;
	for (i = 0; i < n; i++)
		if ((rc = z80step(m, &in)) != X_OK)
			break;
	*k += i;
	return (rc);
}
