/* exec(2) arguments and the exec switch. */
#ifndef _SYS_EXEC_H
#define _SYS_EXEC_H

/* the new image's argument block, as exec builds it */
struct uarg {
	caddr_t	estkstart;
	int	estksize;
	char	_ua0[4];
	int	stringsize;
	char	_ua1[8];
	int	argc;
	int	envc;
	int	prefixc;
	char	_ua2[8];
	int	auxsize;
	char	_ua3[4];
	addr_t	stacklow;
	caddr_t	stackend;
	char	_ua4[16];
};

struct execsw {
	short	*exec_magic;
	int	(*exec_func)();
	int	(*exec_core)();
};
extern int nexectype;
extern struct execsw execsw[];

#ifdef __STDC__
extern int execmap(vnode_t *, caddr_t, size_t, size_t, off_t, int);
extern int setregs(struct uarg *);
#else
extern int execmap();
extern int setregs();
#endif

#endif
