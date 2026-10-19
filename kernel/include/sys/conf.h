/* Device and STREAMS module switch tables. */
#ifndef _SYS_CONF_H
#define _SYS_CONF_H

struct bdevsw {
	int	(*d_open)();
	int	(*d_close)();
	int	(*d_strategy)();
	int	(*d_print)();
	int	(*d_size)();
	int	(*d_xpoll)();
	int	(*d_xhalt)();
	int	*d_flag;
};

struct cdevsw {
	int	(*d_open)();
	int	(*d_close)();
	int	(*d_read)();
	int	(*d_write)();
	int	(*d_ioctl)();
	int	(*d_mmap)();
	int	(*d_segmap)();
	int	(*d_poll)();
	int	(*d_xpoll)();
	int	(*d_xhalt)();
	struct tty *d_ttys;
	struct streamtab *d_str;
	int	*d_flag;
};

#define FMNAMESZ	8
struct fmodsw {
	char	f_name[FMNAMESZ + 1];
	struct streamtab *f_str;
	int	*f_flag;
};

extern struct bdevsw bdevsw[];
extern struct cdevsw cdevsw[];
extern struct fmodsw fmodsw[];
extern int bdevcnt, cdevcnt, fmodcnt;

/* d_flag bits */
#define D_NEW		0x00
#define D_OLD		0x01
#define D_NOBRKUP	0x10	/* takes requests of any size */

#endif
