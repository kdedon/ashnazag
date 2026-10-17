/* Basic kernel types: the SVR4 m68k ABI. */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

typedef unsigned char	uchar_t, unchar, u_char;
typedef unsigned short	ushort_t, ushort, u_short;
typedef unsigned int	uint_t, uint, u_int;
typedef unsigned long	ulong_t, ulong, u_long;

typedef char		*addr_t, *caddr_t;
typedef long		daddr_t, off_t, id_t, pid_t, uid_t, gid_t, time_t, clock_t;
typedef short		cnt_t, o_dev_t, o_pid_t;
typedef ushort_t	o_mode_t, o_uid_t, o_gid_t, o_ino_t;
typedef ulong_t		paddr_t, k_sigset_t, k_fltset_t, major_t, minor_t;
typedef ulong_t		mode_t, nlink_t, dev_t, ino_t;
typedef int		key_t;
typedef enum boolean { B_FALSE, B_TRUE } boolean_t;
typedef struct _label { int val[13]; } label_t;
typedef struct _quad { long val[2]; } quad;

#ifndef _SIZE_T
#define _SIZE_T
typedef uint_t		size_t;
#endif

#define NODEV		((dev_t)-1)
#define NBBY		8

/* Traditional C has no const or volatile. */
#ifdef __STDC__
#ifndef _VOID
#define _VOID		void
#endif
#else
#ifndef _VOID
#define _VOID		char
#endif
#ifndef const
#define const
#endif
#ifndef volatile
#define volatile
#endif
#endif

#endif
