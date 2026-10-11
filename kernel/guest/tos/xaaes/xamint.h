/*
 * xamint.h -- the MiNT layer: a small kernel serving MiNT's system
 * calls and its module interface to XaAES inside the TOS environment.
 * Built with 16-bit int against MiNT's own headers, so its structures
 * and argument layouts are the ones modules and programs expect.
 */
#define __LOOSE_ERRROR_CODES
#include "mint/mint.h"
#include "libkern/libkern.h"
#include "mint/proc.h"
#include "mint/file.h"
#include "mint/filedesc.h"
#include "mint/credentials.h"
#include "mint/signal.h"
#include "mint/proc_extensions.h"
#include "mint/mem.h"
#include "mint/basepage.h"
#include "mint/kentry.h"
#include "mint/module.h"
#include "mint/emu_tos.h"
#include "mint/time.h"
#include "mint/stat.h"
#include "mint/fcntl.h"
#include "mint/ioctl.h"
#include "mint/dcntl.h"
#include "mint/ssystem.h"
#include "mint/fsops.h"
#include "mint/pathconf.h"
#include "k_exec.h"
#include "cnf.h"

/* libkern's bzero miscounts sizes under 4 at unaligned addresses */
#undef bzero
#define	bzero(p, n)	memset((p), 0, (n))

#ifndef EFILNF
#define	EFILNF	ENOENT
#endif
#ifndef EPTHNF
#define	EPTHNF	ENOTDIR
#endif
#ifndef EDRIVE
#define	EDRIVE	ENXIO
#endif
#ifndef EIMBA
#define	EIMBA	EFAULT
#endif

#define	NOFILE	40			/* handles per process */
#define	NHOSTFD	256
#define	PLEN	256			/* host path length */

/* a process: MiNT's structure first, ours after it */
struct kp {
	struct proc	p;
	long		ksp;		/* kernel stack while switched out */
	short		kthread;	/* runs in the kernel only */
	short		started;
	void		(*kfunc)(void *);
	void		*karg;
	long		wake_at;	/* ms; 0: no timeout */
	long		*sel_r, *sel_w;	/* Fselect's sets, while waiting */
	long		sel_rv, sel_wv;
	short		exitcode;
	short		zombie;
	struct kp	*all;		/* every process */
	struct memspace	mem;
	struct filedesc	fd;
	struct cwd	cwd;
	struct sigacts	sa;
	struct pcred	cred;
	struct ucred	ucr;
	struct p_ext	ext;
	struct file	*ofiles[NOFILE - MIN_HANDLE];
	unsigned char	oflags[NOFILE - MIN_HANDLE];
	char		*dirs[26];	/* current directory by drive, '\' separated */
	char		*dta;
	BASEPAGE	*base;
	struct sigsave	*sigsave;	/* contexts of handlers running */
};

#define	KP(p)	((struct kp *)(p))

extern struct proc *curproc, *rootproc;
extern struct kp *allprocs;
extern volatile short xa_resched;
extern long _cdecl (*aes_disp)(void *);
extern struct kentry kentry;
extern dos_vecs dos_tab;
extern long (*dos_trap[0x190])();
extern char xa_thunks[], xa_tthunks[];
extern int xa_tracing;
extern long hc_d1;
extern struct global global;

/* entry.S */
long hc(long n, long nargs, ...);
void xa_swtch(long *save, long sp);
void xa_kstart(void);
void xa_idle(void);
short xa_spl7(void);
void xa_splx(short sr);
short xa_getsr(void);
long xa_usp(void);
void xa_setusp(long usp);
void xa_sigtramp(void);
long xa_super(void);
extern long ks_usp, ks_pc, ks_regs[15];
extern short ks_sr;
void xa_trap1(void), xa_trap2(void), xa_tick(void), xa_osentry(void);
extern long old_trap1, old_trap2, old_etv;

/* host system calls */
#define	H_EXIT		1
#define	H_READ		3
#define	H_WRITE		4
#define	H_OPEN		5
#define	H_CLOSE		6
#define	H_LINK		9
#define	H_UNLINK	10
#define	H_CHDIR		12
#define	H_TIME		13
#define	H_CHMOD		15
#define	H_LSEEK		19
#define	H_GETPID	20
#define	H_UTIME		30
#define	H_ACCESS	33
#define	H_KILL		37
#define	H_DUP		41
#define	H_PIPE		42
#define	H_IOCTL		54
#define	H_UMASK		60
#define	H_FCNTL		62
#define	H_RMDIR		79
#define	H_MKDIR		80
#define	H_GETDENTS	81
#define	H_POLL		87
#define	H_SYMLINK	89
#define	H_READLINK	90
#define	H_FCHMOD	93
#define	H_STATVFS	103
#define	H_XSTAT		123
#define	H_LXSTAT	124
#define	H_FXSTAT	125
#define	H_RENAME	134

#define	HO_WRONLY	1
#define	HO_RDWR		2
#define	HO_APPEND	8
#define	HO_NONBLOCK	0x80
#define	HO_CREAT	0x100
#define	HO_TRUNC	0x200
#define	HO_EXCL		0x400
#define	HO_NOCTTY	0x800

/* the host's struct stat: offsets */
#define	HST_SIZE	136
#define	HST_DEV		0
#define	HST_INO		16
#define	HST_MODE	20
#define	HST_NLINK	24
#define	HST_UID		28
#define	HST_GID		32
#define	HST_RDEV	36
#define	HST_FSIZE	48
#define	HST_ATIME	56
#define	HST_MTIME	64
#define	HST_CTIME	72
#define	HST_BLKSIZE	80
#define	HST_BLOCKS	84
#define	HL(b, o)	(*(long *)((char *)(b) + (o)))

/* log.c */
void xlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xlog_open(void);
long merr(long e);
long now_ms(void);
long xa_unserved(short n);

/* mem.c */
void mem_init(long lo, long hi);
void *kmalloc_o(unsigned long n, struct kp *owner);
void kfree_any(void *p);
long mem_largest(void);
long mem_shrink(void *p, long n);
void mem_freeproc(struct kp *k);
MEMREGION *mem_region(struct kp *k, long addr);
void mem_give(void *p, struct kp *k);
void *_cdecl _kmalloc(unsigned long size, const char *func);
void _cdecl _kfree(void *place, const char *func);
void *_cdecl _umalloc(unsigned long size, const char *func);
void _cdecl _ufree(void *place, const char *func);

/* proc.c */
void proc_init(void);
struct kp *proc_new(struct kp *parent, const char *name, int kthread, int share);
void proc_exit(struct kp *k, int code);
void proc_reap(struct kp *k);
void schedule(void);
void ready(struct kp *k);
void run_timeouts(void);
void idle_loop(void);
int sel_wait(long ms);
long xa_yield(void);
void xa_tick_c(void);
void xa_kstart_c(void);
void xa_trapret(short *frame);
int _cdecl sleep(int que, long cond);
void _cdecl wake(int que, long cond);
void _cdecl wakeselect(struct proc *p);
void _cdecl nap(unsigned n);
long _cdecl ikill(int pid, unsigned short sig);
struct proc *_cdecl pid2proc(int pid);
struct proc *_cdecl get_curproc(void);
TIMEOUT *_cdecl addroottimeout(long delta, void (*func)(PROC *, long), unsigned short flags);
void _cdecl cancelroottimeout(TIMEOUT *t);
long _cdecl kthread_create(struct proc *p, void _cdecl (*func)(void *), void *arg, struct proc **np, const char *fmt, ...);
void _cdecl kthread_exit(short code);

/* exec.c */
long _cdecl create_process(const void *filename, const void *cmdline, const void *env, struct proc **pret, long stack, struct create_process_opts *opts);
long run_xaaes(const char *path);

/* file.c */
void file_init(void);
void fd_inherit(struct kp *child, struct kp *parent);
void fd_closeall(struct kp *k);
long fd_alloc(struct kp *k, struct file *f, int from);
struct file *fd_get(struct kp *k, int fd);
int host_path(const char *mpath, char *out, int follow);
long file_open(const char *path, int mode, int perm, struct file **res);
long file_close(struct file *f);
long file_read(struct file *f, char *buf, long n);
long file_write(struct file *f, const char *buf, long n);
long file_select(struct file *f, int wr);
long file_getchar(struct file *f, int mode);
long hstat(const char *hp, char *st, int link);
void st_xattr(char *st, XATTR *x);
void st_stat64(char *st, STAT *s);
void dir_set(struct kp *k, int drv, const char *path);
long _cdecl kernel_opendir(struct dirstruct *dirh, const char *name);
long _cdecl kernel_readdir(struct dirstruct *dirh, char *buf, int len);
void _cdecl kernel_closedir(struct dirstruct *dirh);
struct file *_cdecl kernel_open(const char *path, int rwmode, long *err, XATTR *x);
long _cdecl kernel_read(struct file *f, void *buf, long n);
long _cdecl kernel_write(struct file *f, const void *buf, long n);
long _cdecl kernel_lseek(FILEPTR *f, long where, int whence);
void _cdecl kernel_close(struct file *f);
extern short contty_state;
extern int logfd;

/* dos.c */
void dos_init(void);
long _cdecl sys_p_term(short code);
long _cdecl sys_s_system(int mode, ulong arg1, ulong arg2);
long _cdecl sys_f_select(unsigned short timeout, long *rfdp, long *wfdp, long *xfdp);

/* trace.c */
void trace_init(void);
void trace_procs(void);
void fault_init(void);

/* kentry.c */
void kentry_init(void);
long cookie_get(long tag, long *val);
long cookie_set(long tag, long val);
