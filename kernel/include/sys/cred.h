/* User credentials. */
#ifndef _SYS_CRED_H
#define _SYS_CRED_H

typedef struct cred {
	ushort	cr_ref;
	ushort	cr_ngroups;
	uid_t	cr_uid, cr_gid;		/* effective */
	uid_t	cr_ruid, cr_rgid;	/* real */
	uid_t	cr_suid, cr_sgid;	/* saved */
	gid_t	cr_groups[1];		/* cr_ngroups long */
} cred_t;

#define crhold(cr)	(cr)->cr_ref++

#ifdef __STDC__
extern void crfree(cred_t *);
extern cred_t *crget(void);
extern cred_t *crdup(cred_t *);
extern int suser(cred_t *);
extern int groupmember(gid_t, cred_t *);
#else
extern void crfree();
extern cred_t *crget();
extern cred_t *crdup();
extern int suser();
extern int groupmember();
#endif

#endif
