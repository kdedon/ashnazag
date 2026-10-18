/* Process sets for waitid, sigsend and priocntl. */
#ifndef _SYS_PROCSET_H
#define _SYS_PROCSET_H

#define P_INITPID	1
#define P_INITUID	0
#define P_INITPGID	0

typedef enum idtype {
	P_PID, P_PPID, P_PGID, P_SID, P_CID, P_UID, P_GID, P_ALL
} idtype_t;

typedef enum idop { POP_DIFF, POP_AND, POP_OR, POP_XOR } idop_t;

/* p_op applied to the left and right sets */
typedef struct procset {
	idop_t		p_op;
	idtype_t	p_lidtype;
	id_t		p_lid;
	idtype_t	p_ridtype;
	id_t		p_rid;
} procset_t;

#define setprocset(psp, op, ltype, lid, rtype, rid) \
	(psp)->p_op = (op); \
	(psp)->p_lidtype = (ltype); \
	(psp)->p_lid = (lid); \
	(psp)->p_ridtype = (rtype); \
	(psp)->p_rid = (rid);

#ifdef _KERNEL
extern int dotoprocs();
extern int procinset();
extern boolean_t cur_inset_only();
#endif

#endif
