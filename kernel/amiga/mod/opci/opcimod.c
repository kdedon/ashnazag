/*
 * The opci module: the PCI bus behind a Mediator, Prometheus, G-REX or
 * Firestorm bridge, for other modules (<sys/opci.h>, "$depend opci").
 *
 * Loading reads openpci.library (Thomas Richter's, from Aminet; not
 * shipped here) from opci_libpath and starts it on amilib.  A kernel
 * without a bridge, or without the Amiga platform hooks, refuses the
 * load with ENXIO.  Unloading expunges the library; while a dependent
 * module is loaded DLM keeps opci.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/errno.h"
#include "sys/immu.h"
#include "sys/signal.h"
#include "sys/fs/s5dir.h"
#include "sys/psw.h"
#include "sys/pcb.h"
#include "sys/user.h"
#include "sys/cred.h"
#include "sys/vnode.h"
#include "sys/vfs.h"
#include "sys/uio.h"
#include "sys/file.h"
#include "sys/kmem.h"
#include "sys/moddefs.h"

extern char opci_libpath[];
extern int opci_init(), opci_fini();

#define	OPCI_MAXLIB	(1024L * 1024)

static int
opci_read(vp, size, bufp)
	struct vnode *vp;
	long size;
	char **bufp;
{
	char *b;
	int e, resid = 0;

	b = (char *)kmem_alloc((size_t)size, KM_SLEEP);
	e = vn_rdwr(UIO_READ, vp, (caddr_t)b, (int)size, (off_t)0,
	    UIO_SYSSPACE, 0, 0x7fffffffL, u.u_cred, &resid);
	if (e == 0 && resid)
		e = EIO;
	if (e) {
		kmem_free((_VOID *)b, (size_t)size);
		return e;
	}
	*bufp = b;
	return 0;
}

static int
opci_load()
{
	struct vnode *vp;
	struct vattr va;
	char *buf;
	long size;
	int e;

	e = vn_open(opci_libpath, UIO_SYSSPACE, FREAD, 0, &vp, (enum create)0);
	if (e) {
		printf("opci: %s: error %d\n", opci_libpath, e);
		return e;
	}
	va.va_mask = AT_SIZE;
	e = VOP_GETATTR(vp, &va, 0, u.u_cred);
	size = va.va_size;
	if (e == 0 && (vp->v_type != VREG || size < 32 || size > OPCI_MAXLIB))
		e = ENOEXEC;
	if (e == 0)
		e = opci_read(vp, size, &buf);
	(void)VOP_CLOSE(vp, FREAD, 1, (off_t)0, u.u_cred);
	VN_RELE(vp);
	if (e)
		return e;
	/* amilib copies the hunks out; the file image goes at once */
	e = opci_init(buf, (unsigned long)size);
	kmem_free((_VOID *)buf, (size_t)size);
	return e;
}

static int
opci_unload()
{
	return opci_fini();
}

MOD_MISC_WRAPPER(opci, opci_load, opci_unload, "PCI bridges (openpci.library)");
