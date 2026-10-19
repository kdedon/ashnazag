/* mount(2) flags. */
#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#define	MS_RDONLY	0x01
#define	MS_FSS		0x02	/* 4-argument mount */
#define	MS_DATA		0x04	/* 6-argument mount */
#define	MS_NOSUID	0x10
#define	MS_REMOUNT	0x20
#define	MS_NOTRUNC	0x40	/* ENAMETOOLONG for long names */

#endif
