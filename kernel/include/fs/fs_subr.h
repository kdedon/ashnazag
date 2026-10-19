/* Routines filesystems share for operations they don't implement. */
#ifndef _FS_FS_SUBR_H
#define _FS_FS_SUBR_H

extern int	fs_nosys();
extern int	fs_sync();
extern void	fs_rwlock();
extern void	fs_rwunlock();
extern int	fs_cmp();
extern int	fs_frlock();
extern int	fs_setfl();
extern int	fs_poll();
extern int	fs_vcode();
extern int	fs_pathconf();

#endif
