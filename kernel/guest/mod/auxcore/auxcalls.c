/*
 * auxcalls.c -- the A/UX call table: number, trap #0 argument count,
 * flags, AMIX call for class N, else the handler.  Numbers not listed
 * are A/UX nosys (SIGSYS).  AE_TODO rows are A/UX calls without an
 * implementation yet; they fail with EINVAL.
 *
 * K&R C.
 */

#include "auxcore.h"

extern int aux_open(), aux_fcntl(), aux_ioctl(), aux_wait();
extern int aux_stat(), aux_lstat(), aux_fstat();
extern int aux_xstat(), aux_xlstat(), aux_xfstat();
extern int aux_getdirentries(), aux_utssys(), aux_gethostname();
extern int aux_getdomainname(), aux_gettimeofday(), aux_getdtablesize();
extern int aux_getcompat(), aux_setcompat(), aux_setpgrp(), aux_setsid();
extern int aux_setpgid(), aux_swapmmumode(), aux_netdown(), aux_slotmanager();
extern int aux_sigvec(), aux_sigblock(), aux_sigsetmask(), aux_sigpause();
extern int aux_sigstack();
extern int aux_read(), aux_select(), aux_waitpid(), aux_flock();
extern int aux_statfs(), aux_fstatfs(), aux_truncate(), aux_ftruncate();
extern int aux_utimes(), aux_getitimer(), aux_setitimer(), aux_alarm();
extern int aux_setreuid(), aux_setregid(), aux_shmsys(), aux_sigpending();
extern int aux_fidop(), aux_csop(), aux_setxinfo(), aux_fsetxinfo();

#define	S	AE_SETJMP
#define	T	AE_TODO

struct auxent auxcalls[] = {
	{ 1, 1, S, 1, 0 },		/* rexit */
	{ 2, 0, S, 2, 0 },		/* fork */
	{ 3, 3, S, 0, aux_read },
	{ 4, 3, S, 4, 0 },		/* write */
	{ 5, 3, S, 0, aux_open },
	{ 6, 1, S, 6, 0 },		/* close */
	{ 7, 1, S, 0, aux_wait },
	{ 8, 2, S, 8, 0 },		/* creat */
	{ 9, 2, S, 9, 0 },		/* link */
	{ 10, 1, S, 10, 0 },		/* unlink */
	{ 11, 2, S, 11, 0 },		/* exec */
	{ 12, 1, S, 12, 0 },		/* chdir */
	{ 13, 1, S, 13, 0 },		/* gtime */
	{ 14, 3, S, 14, 0 },		/* mknod */
	{ 15, 2, S, 15, 0 },		/* chmod */
	{ 16, 3, S, 16, 0 },		/* chown */
	{ 17, 1, S, 17, 0 },		/* sbreak */
	{ 19, 3, S, 19, 0 },		/* seek */
	{ 20, 0, S, 20, 0 },		/* getpid */
	{ 22, 1, S, 22, 0 },		/* unmount */
	{ 23, 1, S, 23, 0 },		/* setuid */
	{ 24, 0, S, 24, 0 },		/* getuid */
	{ 25, 1, S, 25, 0 },		/* stime */
	{ 26, 4, T, 0, 0 },		/* ptrace */
	{ 27, 1, S, 0, aux_alarm },
	{ 29, 0, S, 29, 0 },		/* pause */
	{ 30, 2, S, 30, 0 },		/* utime */
	{ 33, 2, S, 33, 0 },		/* access */
	{ 34, 1, S, 34, 0 },		/* nice */
	{ 36, 0, S, 36, 0 },		/* sync */
	{ 37, 2, S, 0, aux_kill },
	{ 38, 5, S, 0, aux_sysm68k },
	{ 39, 3, S, 0, aux_setpgrp },
	{ 41, 1, S, 41, 0 },		/* dup */
	{ 42, 0, S, 42, 0 },		/* pipe */
	{ 43, 1, S, 43, 0 },		/* times */
	{ 44, 4, S, 44, 0 },		/* profil */
	{ 45, 1, S, 45, 0 },		/* lock */
	{ 46, 1, S, 46, 0 },		/* setgid */
	{ 47, 0, S, 47, 0 },		/* getgid */
	{ 48, 2, S, 0, aux_ssig },
	{ 49, 6, T, 0, 0 },		/* msgsys */
	{ 50, 5, S, 0, aux_sysm68k },
	{ 51, 1, S, 51, 0 },		/* sysacct */
	{ 52, 4, S, 0, aux_shmsys },
	{ 53, 5, T, 0, 0 },		/* semsys */
	{ 54, 3, S, 0, aux_ioctl },
	{ 55, 4, T, 0, 0 },		/* phys */
	{ 56, 3, T, 0, 0 },		/* locking */
	{ 57, 3, S, 0, aux_utssys },
	{ 59, 3, S, 59, 0 },		/* exece */
	{ 60, 1, S, 60, 0 },		/* umask */
	{ 61, 1, S, 61, 0 },		/* chroot */
	{ 62, 3, S, 0, aux_fcntl },
	{ 63, 2, S, 63, 0 },		/* ulimit */
	{ 64, 1, T, 0, 0 },		/* reboot */
	{ 65, 1, T, 0, 0 },		/* powerdown */
	{ 66, 2, 0, 0, aux_slotmanager },
	{ 67, 1, S, 0, aux_swapmmumode },
	{ 70, 3, S, 0, aux_netdown },	/* accept */
	{ 71, 3, S, 0, aux_netdown },	/* bind */
	{ 72, 3, S, 0, aux_netdown },	/* connect */
	{ 73, 0, T, 0, 0 },		/* gethostid */
	{ 74, 2, S, 0, aux_gethostname },
	{ 75, 3, S, 0, aux_netdown },	/* getpeername */
	{ 76, 3, S, 0, aux_netdown },	/* getsockname */
	{ 77, 5, S, 0, aux_netdown },	/* getsockopt */
	{ 78, 2, S, 0, aux_netdown },	/* listen */
	{ 79, 4, S, 0, aux_netdown },	/* recv */
	{ 80, 6, S, 0, aux_netdown },	/* recvfrom */
	{ 81, 3, S, 0, aux_netdown },	/* recvmsg */
	{ 82, 5, S, 0, aux_select },
	{ 83, 4, S, 0, aux_netdown },	/* send */
	{ 84, 3, S, 0, aux_netdown },	/* sendmsg */
	{ 85, 6, S, 0, aux_netdown },	/* sendto */
	{ 86, 1, T, 0, 0 },		/* sethostid */
	{ 87, 2, T, 0, 0 },		/* sethostname */
	{ 88, 2, S, 0, aux_setregid },
	{ 89, 2, S, 0, aux_setreuid },
	{ 90, 5, S, 0, aux_netdown },	/* setsockopt */
	{ 91, 2, S, 0, aux_netdown },	/* shutdown */
	{ 92, 3, S, 0, aux_netdown },	/* socket */
	{ 93, 4, S, 0, aux_netdown },	/* socketpair */
	{ 100, 2, S, 0, aux_getdomainname },
	{ 101, 2, T, 0, 0 },		/* setdomainname */
	{ 102, 2, S, 92, 0 },		/* getgroups */
	{ 103, 2, S, 91, 0 },		/* setgroups */
	{ 104, 0, S, 0, aux_getdtablesize },
	{ 105, 2, S, 0, aux_flock },
	{ 106, 3, S, 121, 0 },		/* readv */
	{ 107, 3, S, 122, 0 },		/* writev */
	{ 108, 2, S, 80, 0 },		/* mkdir */
	{ 109, 1, S, 79, 0 },		/* rmdir */
	{ 110, 4, S, 0, aux_getdirentries },
	{ 111, 2, S, 0, aux_lstat },
	{ 112, 2, S, 89, 0 },		/* symlink */
	{ 113, 3, S, 90, 0 },		/* readlink */
	{ 114, 2, S, 0, aux_truncate },
	{ 115, 2, S, 0, aux_ftruncate },
	{ 116, 1, S, 58, 0 },		/* fsync */
	{ 117, 2, S, 0, aux_statfs },
	{ 118, 2, S, 0, aux_fstatfs },
	{ 123, 2, S, 134, 0 },		/* rename */
	{ 124, 2, S, 0, aux_fstat },
	{ 125, 2, S, 0, aux_stat },
	{ 127, 0, S, 0, aux_getcompat },
	{ 128, 1, S, 0, aux_setcompat },
	{ 129, 3, S, 0, aux_sigvec },
	{ 130, 1, S, 0, aux_sigblock },
	{ 131, 1, S, 0, aux_sigsetmask },
	{ 132, 1, S, 0, aux_sigpause },
	{ 133, 2, S, 0, aux_sigstack },
	{ 134, 2, S, 0, aux_getitimer },
	{ 135, 3, S, 0, aux_setitimer },
	{ 136, 1, S, 0, aux_gettimeofday },
	{ 137, 1, T, 0, 0 },		/* _settimeofday */
	{ 138, 2, S, 138, 0 },		/* adjtime */
	{ 139, 4, T, 0, 0 },		/* errsys */
	{ 141, 4, T, 0, 0 },		/* mount */
	{ 142, 1, S, 22, 0 },		/* umount */
	{ 143, 3, S, 93, 0 },		/* fchmod */
	{ 144, 3, S, 94, 0 },		/* fchown */
	{ 145, 2, S, 0, aux_utimes },
	{ 146, 0, S, 0, aux_setsid },
	{ 147, 2, S, 0, aux_setpgid },
	{ 148, 0, T, 0, 0 },		/* getcterm */
	{ 149, 1, S, 0, aux_sigpending },
	{ 151, 3, S, 0, aux_waitpid },
	{ 152, 2, S, 0, aux_fidop },
	{ 153, 5, T, 0, 0 },		/* asioread */
	{ 154, 5, T, 0, 0 },		/* asiowrite */
	{ 155, 0, T, 0, 0 },		/* asiowait */
	{ 156, 2, T, 0, 0 },		/* sema_acq */
	{ 157, 1, T, 0, 0 },		/* sema_rel */
	{ 158, 3, T, 0, 0 },		/* _memlock */
	{ 159, 3, T, 0, 0 },		/* _memunlock */
	{ 160, 3, T, 0, 0 },		/* chnod */
	{ 161, 3, S, 0, aux_csop },
	{ 162, 2, S, 0, aux_xfstat },
	{ 163, 2, S, 0, aux_xstat },
	{ 164, 2, S, 0, aux_fsetxinfo },
	{ 165, 2, S, 0, aux_setxinfo },
	{ 166, 2, S, 0, aux_xlstat },
	{ 167, 4, T, 0, 0 },		/* atp_control */
	{ 168, 2, S, 0, aux_gettimeofday },
	{ 169, 2, T, 0, 0 },		/* settimeofday */
	{ -1, 0, 0, 0, 0 }
};
