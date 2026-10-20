/*
 * /dev/mem and /dev/kmem.  A platform that knows its holes overrides
 * plat_kmemlen: some machines never end a bus cycle to a hole,
 * so only the mapped part of a transfer is done, and none at all fails
 * with EFAULT.  A fault caught on the way is EFAULT too.
 */
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/errno.h>

extern int __vm_mmrw(), getminor();
extern long plat_kmemlen();		/* bytes mapped from a, 0 in a hole, -1 unknown */

int
mmrw(dev, uio, rw, cr)
dev_t dev;
struct uio *uio;
int rw;
char *cr;
{
	unsigned long a;
	long n, r = 0;
	int err;

	if (getminor(dev) > 1)
		return __vm_mmrw(dev, uio, rw, cr);
	if (uio->uio_resid > 0) {
		a = uio->uio_offset;
		r = uio->uio_resid;
		while (r > 0 && (n = plat_kmemlen(a)) != 0) {
			if (n < 0 || n >= r)
				r = 0;
			else
				a += n, r -= n;
		}
		if (r == uio->uio_resid)
			return EFAULT;
		uio->uio_resid -= r;
	}
	err = __vm_mmrw(dev, uio, rw, cr);
	uio->uio_resid += r;
	return err == ENXIO ? EFAULT : err;
}
