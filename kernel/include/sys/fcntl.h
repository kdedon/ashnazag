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

#endif
