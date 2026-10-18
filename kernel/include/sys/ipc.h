/* System V IPC permissions and control commands. */
#ifndef _SYS_IPC_H
#define _SYS_IPC_H

struct ipc_perm {
	uid_t	uid;
	gid_t	gid;
	uid_t	cuid;
	gid_t	cgid;
	mode_t	mode;
	ulong	seq;
	key_t	key;
	long	pad[4];
};

#define IPC_ALLOC	0100000
#define IPC_CREAT	0001000
#define IPC_EXCL	0002000
#define IPC_NOWAIT	0004000
#define IPC_PRIVATE	(key_t)0

#define IPC_RMID	10
#define IPC_SET		11
#define IPC_STAT	12

#endif
