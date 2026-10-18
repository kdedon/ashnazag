/* File system statistics. */
#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H

#define FSTYPSZ		16

typedef struct statvfs {
	u_long	f_bsize;
	u_long	f_frsize;
	u_long	f_blocks;
	u_long	f_bfree;
	u_long	f_bavail;
	u_long	f_files;
	u_long	f_ffree;
	u_long	f_favail;
	u_long	f_fsid;
	char	f_basetype[FSTYPSZ];
	u_long	f_flag;
	u_long	f_namemax;
	char	f_fstr[32];
	u_long	_f0[16];
} statvfs_t;

/* f_flag */
#define ST_RDONLY	0x01
#define ST_NOSUID	0x02
#define ST_NOTRUNC	0x04

#endif
