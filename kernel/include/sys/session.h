/* Sessions and controlling terminals. */
#ifndef _SYS_SESSION_H
#define _SYS_SESSION_H

typedef struct sess {
	short		s_ref;
	short		s_mode;
	uid_t		s_uid;
	gid_t		s_gid;
	ulong		s_ctime;
	dev_t		s_dev;		/* controlling tty */
	struct vnode	*s_vp;
	struct pid	*s_sidp;
	struct cred	*s_cred;
} sess_t;

#define s_sid		s_sidp->pid_id

extern sess_t session0;

#define SESS_HOLD(sp)	(++(sp)->s_ref)
#define SESS_RELE(sp)	(--(sp)->s_ref > 0 ? 0 : sess_rele(sp))

extern int sess_rele();
extern void sess_create();
extern void freectty();
extern void alloctty();
extern int realloctty();
extern dev_t cttydev();

#endif
