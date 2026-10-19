/* Path name lookup. */
#ifndef _SYS_PATHNAME_H
#define _SYS_PATHNAME_H

typedef struct pathname {
	char	*pn_buf;	/* allocation */
	char	*pn_path;	/* next component */
	u_int	pn_pathlen;	/* bytes left */
} pathname_t;

#define PN_STRIP	0
#define PN_PEEK		1

extern void	pn_alloc();
extern int	pn_get();
extern int	pn_set();
extern int	pn_getcomponent();
extern void	pn_free();
extern int	lookupname();
extern int	lookuppn();
extern int	traverse();

#endif
