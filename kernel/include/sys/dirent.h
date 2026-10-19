/* Directory entries as getdents returns them. */
#ifndef _SYS_DIRENT_H
#define _SYS_DIRENT_H

typedef struct dirent {
	ino_t		d_ino;
	off_t		d_off;
	unsigned short	d_reclen;
	char		d_name[1];	/* d_reclen - 10 long */
} dirent_t;

#endif
