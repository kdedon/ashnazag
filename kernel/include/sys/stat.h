/* File status: the old stat, the extended xstat, mode bits. */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/time.h>

#define _ST_FSTYPSZ	16

struct stat {
	o_dev_t		st_dev;
	o_ino_t		st_ino;
	o_mode_t	st_mode;
	short		st_nlink;
	o_uid_t		st_uid;
	o_gid_t		st_gid;
	o_dev_t		st_rdev;
	off_t		st_size;
	time_t		st_atime;
	time_t		st_mtime;
	time_t		st_ctime;
};

struct xstat {
	dev_t		st_dev;
	long		_st0[3];
	ino_t		st_ino;
	mode_t		st_mode;
	nlink_t		st_nlink;
	uid_t		st_uid;
	gid_t		st_gid;
	dev_t		st_rdev;
	long		_st1[2];
	off_t		st_size;
	long		_st2;
	timestruc_t	st_atime;
	timestruc_t	st_mtime;
	timestruc_t	st_ctime;
	long		st_blksize;
	long		st_blocks;
	char		st_fstype[_ST_FSTYPSZ];
	long		_st3[8];
};

#define S_IFMT		0xF000
#define S_IAMB		0x1FF
#define S_IFIFO		0x1000
#define S_IFCHR		0x2000
#define S_IFDIR		0x4000
#define S_IFNAM		0x5000
#define S_IFBLK		0x6000
#define S_IFREG		0x8000
#define S_IFLNK		0xA000
#define S_IFSOCK	0xC000
#define S_ISUID		0x800
#define S_ISGID		0x400
#define S_ISVTX		0x200
#define S_IREAD		00400
#define S_IWRITE	00200
#define S_IEXEC		00100
#define S_IRWXU		00700
#define S_IRUSR		00400
#define S_IWUSR		00200
#define S_IXUSR		00100
#define S_IRWXG		00070
#define S_IRGRP		00040
#define S_IWGRP		00020
#define S_IXGRP		00010
#define S_IRWXO		00007
#define S_IROTH		00004
#define S_IWOTH		00002
#define S_IXOTH		00001

#define S_ISFIFO(mode)	((mode&0xF000) == 0x1000)
#define S_ISCHR(mode)	((mode&0xF000) == 0x2000)
#define S_ISDIR(mode)	((mode&0xF000) == 0x4000)
#define S_ISBLK(mode)	((mode&0xF000) == 0x6000)
#define S_ISREG(mode)	((mode&0xF000) == 0x8000)

#define _MKNOD_VER	2
#define _STAT_VER	2

#endif
