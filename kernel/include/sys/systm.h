/* Kernel services and globals. */
#ifndef _SYS_SYSTM_H
#define _SYS_SYSTM_H

extern struct vnode *rootdir;
extern clock_t lbolt;		/* ticks since boot */
extern short cputype;

#ifdef __STDC__
extern void wakeup(caddr_t);
extern int sleep(caddr_t, int);
extern int grow(int *);
extern int timeout(void (*)(), caddr_t, long);
extern int untimeout(int);
extern int nodev(void);
extern int nulldev(void);
extern int bcmp(char *, char *, size_t);
extern char *strncpy(char *, char *, size_t);
extern int strcmp(char *, char *);
extern int strncmp(char *, char *, size_t);
extern int copyin(caddr_t, caddr_t, size_t);
extern int copyout(caddr_t, caddr_t, size_t);
extern int copyinstr(char *, char *, size_t, size_t *);
extern void bcopy(caddr_t, caddr_t, size_t);
extern void bzero(caddr_t, size_t);
extern int fubyte(caddr_t);
extern int fuword(int *);
extern int subyte(caddr_t, char);
extern int suword(int *, int);
extern int setjmp(label_t *);
extern void longjmp(label_t *);
#else
extern void wakeup();
extern int sleep();
extern int grow();
extern int timeout();
extern int untimeout();
extern int nodev();
extern int nulldev();
extern int bcmp();
extern char *strncpy();
extern int strcmp();
extern int strncmp();
extern int copyin();
extern int copyout();
extern int copyinstr();
extern void bcopy();
extern void bzero();
extern int fubyte();
extern int fuword();
extern int subyte();
extern int suword();
extern int setjmp();
extern void longjmp();
#endif
extern void swtch();

/* system call table */
extern struct sysent {
	char	sy_narg;
	char	sy_flags;
	int	(*sy_call)();
} sysent[];

/* system call return values */
typedef union rval {
	struct {
		int	r_v1;
		int	r_v2;
	} r_v;
	off_t	r_off;
	time_t	r_time;
} rval_t;
#define r_val1		r_v.r_v1
#define r_val2		r_v.r_v2

#endif
