/* open(2) flags. */
#ifndef _SYS_FCNTL_H
#define _SYS_FCNTL_H

#define O_RDONLY	0
#define O_WRONLY	1
#define O_RDWR		2
#define O_NDELAY	0x04
#define O_APPEND	0x08
#define O_SYNC		0x10
#define O_NONBLOCK	0x80
#define O_CREAT		0x100
#define O_TRUNC		0x200
#define O_EXCL		0x400
#define O_NOCTTY	0x800

#define F_DUPFD		0
#define F_GETFD		1
#define F_SETFD		2
#define F_GETFL		3
#define F_SETFL		4
#define F_SETLK		6
#define F_SETLKW	7
#define F_CHKFL		8
#define F_ALLOCSP	10
#define F_FREESP	11
#define F_GETLK		14
#define F_RSETLK	20
#define F_RGETLK	21
#define F_RSETLKW	22
#define F_GETOWN	23
#define F_SETOWN	24

typedef struct flock {
	short	l_type;
	short	l_whence;
	off_t	l_start;
	off_t	l_len;		/* 0: to end of file */
	long	l_sysid;
	pid_t	l_pid;
	long	_l0[4];
} flock_t;

/* l_type */
#define F_RDLCK		01
#define F_WRLCK		02
#define F_UNLCK		03

#endif
