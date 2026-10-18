/*
 * ub.h - what the USB stack (U-Boot's core and hub code as FreeMiNT
 * carries them, GPL-2) needs from the AMIX kernel: printing, delays,
 * byte order, the few libc calls.  The same files are atari-sysv-sp1's
 * driver-ub, hence the ub_ ("USB bus") prefix; this header is the AMIX
 * side of it.  Kernel only: the stack runs in process context (a user
 * program polls the hubs through /dev/usb), so its waits are sleeps.
 */
#ifndef UB_H
#define UB_H

#include "sys/types.h"
#include "sys/param.h"

extern int printf();
#define ub_printf	printf
#define memset(p, c, n)	ub_memset((char *)(p), (c), (n))
#define memcpy(d, s, n)	bcopy((caddr_t)(s), (caddr_t)(d), (n))
void ub_memset();
char *strcat();
int sprintf();
extern void bcopy();
extern int strncmp();
extern void delay();
extern void drv_usecwait();

#ifndef NULL
#define NULL	0
#endif
#define E_OK	0
#ifndef ENOSYS
#define ENOSYS	89
#endif
#ifndef MAX
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#endif
#define _cdecl
#define MAINUSB
#define UNUSED(x)	(void)(x)
#define INLINE	static
#undef inline
#define inline

/* printing: ALERT always, DEBUG with -DUB_DEBUG */
#define ALERT(x)	do { ub_printf x; ub_printf("\n"); } while (0)
#define FORCE(x)	ALERT(x)
#ifdef UB_DEBUG
#define DEBUG(x)	ALERT(x)
#else
#define DEBUG(x)
#endif
#define TRACE(x)
#define ASSERT(x)

/* byte order: USB is little-endian, the 68k is not */
#define SWAP16(x)	((unsigned short)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
#define SWAP32(x)	((unsigned long)(((x) >> 24) | (((x) >> 8) & 0xff00) | \
			 (((x) << 8) & 0xff0000) | ((x) << 24)))
#define cpu2le16(x)	SWAP16(x)
#define le2cpu16(x)	SWAP16(x)
#define cpu2le32(x)	SWAP32(x)
#define le2cpu32(x)	SWAP32(x)

/* time */
void ub_udelay();		/* (us) busy */
void ub_mdelay();		/* (ms) sleeps */
#define udelay(u)	ub_udelay((long)(u))
#define mdelay(m)	ub_mdelay((long)(m))

/* the MiNT driver's interrupt brackets: nothing here, the host
 * controller is polled from process context */
#define MINT_INT_OFF
#define MINT_INT_ON
#define TOS_INT_OFF
#define TOS_INT_ON
#ifndef TRUE
#define TRUE	1
#define FALSE	0
#endif
#define FS_INFO		('U' << 8 | 99)	/* the MiNT version query, unused */
#define ikbd_int_pending()	0
#define fake_ikbd_int()

#endif /* UB_H */
