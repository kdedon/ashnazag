/*
 * kinc.h -- the kernel headers the guest modules use.
 */

#ifndef _KINC_H
#define _KINC_H

#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/immu.h"
#include "sys/signal.h"
#include "sys/fs/s5dir.h"
#include "sys/psw.h"
#include "sys/pcb.h"
#include "sys/user.h"
#include "sys/proc.h"
#include "sys/cred.h"
#include "sys/systm.h"
#include "sys/errno.h"
#include "sys/cmn_err.h"
#include "sys/kmem.h"
#include "sys/moddefs.h"
#include "guest.h"

extern struct proc *curproc;

/* big-endian stores into byte buffers */
#define	P8(p, v)	(((unsigned char *)(p))[0] = (v) & 0xff)
#define	P16(p, v)	(P8(p, (v) >> 8), P8((char *)(p) + 1, v))
#define	P32(p, v)	(P16(p, (v) >> 16), P16((char *)(p) + 2, v))
#define	G8(p)		((unsigned long)((unsigned char *)(p))[0])
#define	G16(p)		(G8(p) << 8 | G8((char *)(p) + 1))
#define	G32(p)		(G16(p) << 16 | G16((char *)(p) + 2))

#endif	/* _KINC_H */
