/* System V shared memory. */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#define SHMLBA		ctob(1)
#define SHM_R		0400
#define SHM_W		0200

/* shm_perm.mode */
#define SHM_LOCKED	001000
#define SHM_LOCKWAIT	010000

/* shmat flags */
#define SHM_RDONLY	010000
#define SHM_RND		020000

/* shmctl commands */
#define SHM_LOCK	3
#define SHM_UNLOCK	4

struct shmid_ds {
	struct ipc_perm	shm_perm;
	int		shm_segsz;
	struct anon_map	*shm_amp;
	ushort		shm_lkcnt;
	pid_t		shm_lpid;
	pid_t		shm_cpid;
	ulong		shm_nattch;
	ulong		shm_cnattch;
	time_t		shm_atime;
	long		_shm0;
	time_t		shm_dtime;
	long		_shm1;
	time_t		shm_ctime;
	long		_shm2[5];
};

struct shminfo {
	int	shmmax, shmmin, shmmni, shmseg;
};

#endif
