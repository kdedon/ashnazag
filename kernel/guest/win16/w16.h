/*
 * w16.h -- the Win16 environment: what its parts share.
 *
 * One process holds the whole Win16 world: guest memory (one byte array
 * with selectors into it), the x86 core, the loaded modules, and our own
 * KERNEL, USER and GDI in native code.  Guest values are little-endian;
 * GW/PW and friends read and write them.  A far pointer travels as a
 * u32, selector in the top half.
 */
#ifndef W16_H
#define W16_H

#include "x86.h"

/* far pointers */
#define	FP(sel, off)	((u32)(sel) << 16 | ((off) & 0xffff))
#define	FPSEL(p)	((p) >> 16)
#define	FPOFF(p)	((p) & 0xffff)
#define	LO16(v)		((v) & 0xffff)
#define	HI16(v)		((v) >> 16 & 0xffff)

extern struct x86 *cpu;		/* the one CPU */
extern u8 *M;			/* guest memory */
extern u32 MSIZE;
extern struct desc *LDT;

/* guest memory at a linear address */
#define	GB(a)		(M[a])
#define	GW(a)		RD16(M, a)
#define	GL(a)		RD32(M, a)
#define	PB(a, v)	(M[a] = (v))
#define	PW(a, v)	WR16(M, a, v)
#define	PL(a, v)	WR32(M, a, v)

/* ---- mem16.c ---- */

/* GlobalAlloc flags */
#define	GMEM_FIXED	0x0000
#define	GMEM_MOVEABLE	0x0002
#define	GMEM_NOCOMPACT	0x0010
#define	GMEM_NODISCARD	0x0020
#define	GMEM_ZEROINIT	0x0040
#define	GMEM_MODIFY	0x0080
#define	GMEM_DISCARDABLE 0x0100
#define	GMEM_DDESHARE	0x2000

/* a global block, by the LDT index of its first selector */
struct gblock {
	u32	gb_base;	/* linear */
	u32	gb_size;	/* bytes asked for */
	u32	gb_room;	/* bytes reserved (>= size, 16-byte multiple) */
	u16	gb_nsel;	/* selectors: 1, or one per 64K of a huge block */
	u16	gb_flags;	/* GMEM_* it was made with */
	u16	gb_lock;
	u16	gb_owner;	/* hInstance or module handle; 0 system */
	u8	gb_code;	/* a code segment */
	u8	gb_used;
	u8	gb_discarded;	/* GlobalReAlloc(h, 0, GMEM_MOVEABLE): no memory, the handle kept */
};
#define	GMEM_DISCARDED	0x4000
extern struct gblock *gblk;	/* LDTSIZE of them */

extern void mem_init();		/* (bytes) */
extern u32 lin();		/* (sel, off): linear address, 0 if not mapped (0 is never a block) */
extern char *gptr();		/* (far pointer): host pointer, 0 if not mapped */
extern u32 gstrlen();		/* (far pointer) */
extern int sel_alloc();		/* (n): first of n consecutive free selectors, 0 none */
extern void sel_free();		/* (sel, n) */
extern void sel_set();		/* (sel, base, limit, acc) */
extern u32 sel_base();		/* (sel) */
extern u16 g_alloc();		/* (flags, size, owner): handle (= selector), 0 none */
extern u16 g_realloc();		/* (h, size, flags) */
extern int g_free();		/* (h): 0 ok */
extern u32 g_size();		/* (h) */
extern struct gblock *g_block(); /* (h or selector), 0 if none */
extern void g_freeowner();	/* (owner) */
extern u32 g_free_bytes();	/* () */
extern u16 sel_alias();		/* (sel, code): another selector on the same memory */

/* local heaps */
#define	LMEM_FIXED	0x0000
#define	LMEM_MOVEABLE	0x0002
#define	LMEM_ZEROINIT	0x0040
#define	LMEM_MODIFY	0x0080
#define	LMEM_DISCARDABLE 0x0f00
extern int l_init();		/* (sel, start, end): 0 failed */
extern u16 l_alloc();		/* (sel, flags, size): handle, 0 none */
extern u16 l_free();		/* (sel, h): 0 ok, else h */
extern u16 l_realloc();		/* (sel, h, size, flags) */
extern u16 l_lock();		/* (sel, h): offset */
extern int l_unlock();		/* (sel, h) */
extern u16 l_size();		/* (sel, h) */
extern u16 l_handle();		/* (sel, offset) */
extern u16 l_flags();		/* (sel, h) */
extern u16 l_compact();		/* (sel, want): largest free */

/* ---- ne.c: modules ---- */

#define	MAXSEG	255

struct nseg {
	u16	ns_sel;
	u16	ns_flags;	/* NE segment flags */
	u32	ns_size;	/* bytes in the file */
	u32	ns_alloc;	/* bytes in memory */
};
#define	NSF_DATA	0x0001
#define	NSF_MOVEABLE	0x0010
#define	NSF_RELOC	0x0100

struct nentry {
	u8	ne_seg;		/* 0 none, 0xfe constant, else segment number */
	u8	ne_flags;	/* 1 exported, 2 shared data */
	u16	ne_off;		/* or the constant */
};

struct module {
	char	m_name[16];	/* upper case */
	char	m_path[260];	/* DOS path */
	char	m_host[1024];	/* host path */
	int	m_native;	/* our own (apitab) */
	int	m_dll;
	u16	m_hmod;		/* selector of its module database (the NE header) */
	u16	m_hinst;	/* its DGROUP selector, or hmod without one */
	u16	m_ref;
	u16	m_flags;	/* NE flags */
	u16	m_dgroup;	/* segment number, 0 none */
	u16	m_heap, m_stack;
	u16	m_cs, m_ip, m_ss, m_sp;	/* CS and SS as segment numbers */
	int	m_nseg;
	struct nseg m_seg[MAXSEG + 1];	/* from 1 */
	int	m_nent;
	struct nentry *m_ent;	/* by ordinal (from 1) */
	u8	*m_ne;		/* the header in host memory, as read */
	u32	m_nelen;
	u32	m_neoff;	/* in the file */
	u8	*m_nonres;	/* nonresident names, as read */
	u32	m_nonreslen;
	int	m_nimp;
	struct module *m_imp[64];
	struct apimod *m_api;	/* native: its table */
	u16	m_thunk;	/* native: first thunk of ordinal 0 */
	struct module *m_next;
};

extern struct module *modules;
extern struct module *mod_find();	/* (name) */
extern struct module *mod_loadcopy(), *mod_find_path();
extern struct module *mod_byhandle();	/* (hmod or hinst or a selector of it) */
extern struct module *mod_load();	/* (dos path or name, &error): loaded with refs, or 0 */
extern void mod_free();			/* (module) */
extern u32 mod_proc();			/* (module, ordinal or 0, name or 0): far pointer, 0 none */
extern int mod_ordinal();		/* (module, name): ordinal, 0 none */
extern char *ne_resname();		/* (module, ordinal) for messages */

/* resources: in the module database's copy of the table */
extern u32 res_find();		/* (module, type, name): handle (resource table offset), 0 none */
extern u16 res_load();		/* (module, hres): global handle, 0 none */
extern u32 res_size();		/* (module, hres) */
extern u32 res_data();		/* (module, type, name, &size): linear address of a loaded copy, 0 none */

/* ---- thunk.c ---- */

extern u16 thunksel;		/* the native entry points' segment */
extern void thunk_init();
extern u32 thunk_native();	/* (module, ordinal): far pointer to its thunk */
extern void thunk_dispatch();	/* (cpu, n) */
extern u32 thunk_procinst();	/* (far proc, ds): an instance thunk */
extern int thunk_isnative();	/* (far pointer): one of ours (then *fn gets its handler) */
extern u32 thunk_callback();	/* (far pointer, ds): a native entry calling x86 code */

/* calling guest code from native code */
extern void cb_push16();	/* (v) */
extern void cb_push32();	/* (v) */
extern u32 cb_call();		/* (far proc, ds or 0): DX:AX */
extern u16 cb_ds();		/* (far proc): the DGROUP its module wants */

/* our handlers: (args, first argument first) -> DX:AX */
typedef u32 (*apifn)();
struct impl {
	char	*im_mod;
	char	*im_name;
	apifn	im_fn;
};
extern struct impl k_impl[], u_impl[], g_impl[], o_impl[], mn_impl[], dl_impl[], ct_impl[], sb_impl[], md_impl[], cu_impl[];

/* the caller's arguments for a varargs entry start here (a far pointer) */
extern u32 api_varargs;
/* the instance of the module that called the entry in hand */
extern u16 api_callerds;

/* ---- kernel.c ---- */

struct task {
	u16	t_htask;	/* selector of its task block */
	u16	t_hinst;
	u16	t_hmod;
	u16	t_psp;
	u16	t_queue;
	struct module *t_mod;
	char	t_cmdline[128];
	int	t_show;
	int	t_exit;		/* exit code once ended */
	int	t_done;
	int	t_new;		/* not run yet */
	int	t_idle;		/* waiting in GetMessage with nothing to do */
	int	t_quit, t_quitcode;	/* WM_QUIT posted */
	void	*t_ctx;		/* task.c's */
};
extern struct task *curtask, *tasks[];
extern int ntasks;
extern void kernel_init();
extern struct task *task_new();	/* (module, cmdline, show): a task, run when its turn comes */
extern int task_run();		/* (first task): the session until it ends; its exit code */
extern void task_yield();	/* () another task's turn */
extern int task_othersready();	/* () another has something to do */
extern void w16_exit();		/* (code): the task ends (longjmp) */
extern void w16_fatal(char *, ...);	/* the environment ends with a message */
extern void w16_log(char *, ...);
extern int w16_debug;
extern u32 w16_ticks();		/* milliseconds since start */
extern char windir[], sysdir[];	/* DOS paths of the Windows and system directories */
extern int profile_get();	/* (file or 0 = WIN.INI, section, key, default, out, size) */
extern int profile_put();	/* (file or 0, section, key, value) */

/* ---- dos.c ---- */

extern void dos_init();
extern int dos_int21();		/* (cpu): handled */
extern int dos_hostpath();	/* (dos path, host out, size, create): 0 ok, else DOS error */
extern int dos_drive;		/* current drive, 0 = A */
extern char *drive_root[26];	/* host directory of each drive, 0 none */
extern int dos_open();		/* (dos path, mode): handle or -error */
extern int dos_creat();		/* (dos path, attr): handle or -error */
extern int dos_close();		/* (handle) */
extern s32 dos_read();		/* (handle, linear, count): bytes or -error */
extern s32 dos_write();		/* (handle, linear, count) */
extern s32 dos_seek();		/* (handle, offset, whence) */
extern int dos_fullpath();	/* (dos path, out 128): 0 ok */
extern int dos_delete();	/* (dos path) */
extern int dos_hostfd();	/* (handle): host fd or -1 */

/* ---- screen and input (scr.h) ---- */

/* ---- strings ---- */
extern int w16_stricmp();
extern int w16_strnicmp();
extern void w16_upper();	/* (s) in place */

#endif
