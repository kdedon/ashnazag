/* ioctl command encoding: direction, an 8-bit argument size, group, number. */
#ifndef _SYS_IOCCOM_H
#define _SYS_IOCCOM_H

#define IOCPARM_MASK	0xff
#define IOC_VOID	0x20000000
#define IOC_OUT		0x40000000
#define IOC_IN		0x80000000
#define IOC_INOUT	(IOC_IN|IOC_OUT)

#define _IOSZ(t)	((((int)sizeof(t)) & IOCPARM_MASK) << 16)
#define _IO(x, y)	(IOC_VOID | ((x) << 8) | (y))
#define _IOR(x, y, t)	(IOC_OUT | _IOSZ(t) | ((x) << 8) | (y))
#define _IOW(x, y, t)	(IOC_IN | _IOSZ(t) | ((x) << 8) | (y))
#define _IOWR(x, y, t)	(IOC_INOUT | _IOSZ(t) | ((x) << 8) | (y))

#endif
