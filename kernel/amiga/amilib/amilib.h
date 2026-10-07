/*
 * amilib -- run an AmigaOS shared library (a hunk load file) inside the
 * kernel, against a small exec, expansion and utility and a timer.device.
 *
 * The library sees AmigaOS structures at their AmigaOS offsets.  We
 * reach them by byte offset (AB/AW/AL), not by C structs, so the layout
 * does not depend on the compiler's alignment rules.
 *
 * Calls cross the AmigaOS register convention in both directions:
 *   library -> us   every LVO of our libraries is a jmp to a 6-byte stub
 *                   "jsr am_entry"; am_entry saves d0-a6, am_dispatch
 *                   finds the library and LVO from the stub address and
 *                   runs the handler on the saved registers.
 *   us -> library   am_call(fn, regs) loads d0-a6 from regs, calls fn and
 *                   stores d0/d1/a0/a1 back.
 *
 * The platform part (amx_*) is the only code that knows the host kernel:
 * kernel/amiga/opci/amxplat.c for AMIX, kernel/amiga/test/hplat.c for the
 * emulator harness.
 */

#ifndef _AMILIB_H
#define _AMILIB_H

#define	AB(p, o)	(*(unsigned char *)((char *)(p) + (o)))
#define	AW(p, o)	(*(unsigned short *)((char *)(p) + (o)))
#define	AL(p, o)	(*(unsigned long *)((char *)(p) + (o)))
#define	AP(p, o)	(*(char **)((char *)(p) + (o)))

/* saved registers: d0-d7, a0-a6, then am_entry's return (the stub + 6) */
#define	D0	0
#define	D1	1
#define	D2	2
#define	D3	3
#define	D4	4
#define	D5	5
#define	D6	6
#define	D7	7
#define	A0	8
#define	A1	9
#define	A2	10
#define	A3	11
#define	A4	12
#define	A5	13
#define	A6	14
#define	R_STUB	15

/* ---- exec/nodes.h, exec/lists.h ---- */
#define	LN_SUCC		0
#define	LN_PRED		4
#define	LN_TYPE		8
#define	LN_PRI		9
#define	LN_NAME		10
#define	LN_SIZE		14

#define	LH_HEAD		0
#define	LH_TAIL		4
#define	LH_TAILPRED	8
#define	LH_TYPE		12
#define	LH_SIZE		14

#define	NT_TASK		1
#define	NT_INTERRUPT	2
#define	NT_DEVICE	3
#define	NT_MSGPORT	4
#define	NT_MESSAGE	5
#define	NT_REPLYMSG	7
#define	NT_RESOURCE	8
#define	NT_LIBRARY	9
#define	NT_MEMORY	10
#define	NT_SIGNALSEM	15

/* ---- exec/libraries.h ---- */
#define	LIB_FLAGS	14
#define	LIB_NEGSIZE	16
#define	LIB_POSSIZE	18
#define	LIB_VERSION	20
#define	LIB_REVISION	22
#define	LIB_IDSTRING	24
#define	LIB_SUM		28
#define	LIB_OPENCNT	32
#define	LIB_SIZE	34

#define	LVO_OPEN	(-6)
#define	LVO_CLOSE	(-12)
#define	LVO_EXPUNGE	(-18)

/* ---- exec/execbase.h (V40) ---- */
#define	EB_THISTASK	276
#define	EB_IDNESTCNT	294
#define	EB_TDNESTCNT	295
#define	EB_ATTNFLAGS	296
#define	EB_MEMLIST	322
#define	EB_RESOURCELIST	336
#define	EB_DEVICELIST	350
#define	EB_INTRLIST	364
#define	EB_LIBLIST	378
#define	EB_PORTLIST	392
#define	EB_TASKREADY	406
#define	EB_TASKWAIT	420
#define	EB_VBLANKFREQ	530
#define	EB_POWERFREQ	531
#define	EB_SEMLIST	532
#define	EB_ECLOCKFREQ	568
#define	EB_MEMHANDLERS	616
#define	EB_SIZE		632

#define	AFF_68010	0x01
#define	AFF_68020	0x02
#define	AFF_68030	0x04
#define	AFF_68040	0x08
#define	AFF_68881	0x10
#define	AFF_68882	0x20
#define	AFF_FPU40	0x40
#define	AFF_68060	0x80

/* ---- exec/tasks.h ---- */
#define	TC_SIGALLOC	18
#define	TC_SIGWAIT	22
#define	TC_SIGRECVD	26
#define	TC_SIZE		92

/* ---- exec/semaphores.h ---- */
#define	SS_NESTCOUNT	14
#define	SS_WAITQUEUE	16	/* MinList */
#define	SS_OWNER	40
#define	SS_QUEUECOUNT	44
#define	SS_SIZE		46

/* ---- exec/interrupts.h ---- */
#define	IS_DATA		14
#define	IS_CODE		18
#define	IS_SIZE		22

#define	INTB_PORTS	3	/* level 2 */
#define	INTB_VERTB	5
#define	INTB_EXTER	13	/* level 6 */
#define	AM_NINT		16

/* ---- exec/ports.h, exec/io.h, devices/timer.h ---- */
#define	MP_FLAGS	14
#define	MP_SIGBIT	15
#define	MP_SIGTASK	16
#define	MP_MSGLIST	20
#define	MP_SIZE		34
#define	MN_REPLYPORT	14
#define	MN_LENGTH	18
#define	IO_DEVICE	20
#define	IO_UNIT		24
#define	IO_COMMAND	28
#define	IO_FLAGS	30
#define	IO_ERROR	31
#define	IO_SIZE		32
#define	TR_SECS		32
#define	TR_MICRO	36
#define	TR_ADDREQUEST	9
#define	TR_GETSYSTIME	10
#define	TR_SETSYSTIME	11
#define	IOF_QUICK	1

/* ---- exec/memory.h ---- */
#define	MEMF_PUBLIC	0x00000001
#define	MEMF_CHIP	0x00000002
#define	MEMF_FAST	0x00000004
#define	MEMF_CLEAR	0x00010000
#define	MEMF_LARGEST	0x00020000
#define	MEMF_TOTAL	0x00080000

/* ---- exec/resident.h ---- */
#define	RTC_MATCHWORD	0x4afc
#define	RT_MATCHTAG	2
#define	RT_ENDSKIP	6
#define	RT_FLAGS	10
#define	RT_VERSION	11
#define	RT_TYPE		12
#define	RT_PRI		13
#define	RT_NAME		14
#define	RT_IDSTRING	18
#define	RT_INIT		22
#define	RT_SIZE		26
#define	RTF_AUTOINIT	0x80

/* ---- libraries/configvars.h, libraries/configregs.h ---- */
#define	CD_FLAGS	14
#define	CD_ROM		16	/* struct ExpansionRom, 16 bytes */
#define	ER_TYPE		(CD_ROM + 0)
#define	ER_PRODUCT	(CD_ROM + 1)
#define	ER_FLAGS	(CD_ROM + 2)
#define	ER_MANUFACTURER	(CD_ROM + 4)
#define	ER_SERIAL	(CD_ROM + 6)
#define	ER_INITDIAGVEC	(CD_ROM + 10)
#define	CD_BOARDADDR	32
#define	CD_BOARDSIZE	36
#define	CD_SLOTADDR	40
#define	CD_SLOTSIZE	42
#define	CD_DRIVER	44
#define	CD_NEXTCD	48
#define	CD_SIZE		68
#define	CDF_SHUTUP	0x01
#define	CDF_CONFIGME	0x02
#define	CDF_BADMEMORY	0x04
#define	CDF_PROCESSED	0x08

/* expansion.library base: the private BoardList that FindConfigDev walks
 * (openpci adds and removes ConfigDevs on it directly) */
#define	EXB_BOARDLIST	60
#define	EXB_MOUNTLIST	74
#define	EXB_SIZE	96

/* ---- utility/tagitem.h, utility/hooks.h ---- */
#define	TAG_DONE	0
#define	TAG_IGNORE	1
#define	TAG_MORE	2
#define	TAG_SKIP	3
#define	H_ENTRY		8
#define	H_SUBENTRY	12
#define	H_DATA		16

/* ------------------------------------------------------------------ */

/* one of our libraries or devices */
struct amfn {
	int		af_lvo;		/* -30, -36, ... */
	void		(*af_fn)();	/* (unsigned long r[16]) */
};

struct amlib {
	char		*al_name;
	int		al_type;	/* NT_LIBRARY, NT_DEVICE */
	int		al_version;
	int		al_nlvo;	/* jump table entries */
	int		al_size;	/* positive size */
	struct amfn	*al_tab;	/* ends with af_fn == 0 */
	/* set up by am_addlib */
	char		*al_mem;	/* jump table, base and stubs, one block */
	long		al_memsize;
	char		*al_base;	/* the library base we hand out */
	char		*al_stub;	/* al_nlvo stubs "jsr am_entry" */
	void		(**al_fn)();	/* [i] serves LVO -6 * (i + 1) */
};

/* a host Zorro board, as the platform reports it */
struct amx_zboard {
	unsigned long	zb_pa;		/* physical base */
	unsigned long	zb_size;
	unsigned short	zb_manuf;
	unsigned char	zb_prod;
	unsigned char	zb_type;	/* er_Type as configured */
	unsigned char	zb_flags;	/* er_Flags */
	unsigned long	zb_serial;
	unsigned short	zb_diagvec;
};

/* amglue.s */
extern void am_entry();
extern long am_call();			/* (fn, unsigned long r[15]) */
extern long am_super();			/* (unsigned long r[15]): Supervisor() */
/* both store all fifteen registers back into r */
extern void am_isr();			/* is_Code calling *(int (**)())is_Data */
extern void am_hook();			/* h_Entry calling C: see amexec.c */

/* amexec.c */
extern char *am_sysbase;
extern int am_init();			/* (attnflags) */
extern void am_fini();
extern void am_dispatch();
extern char *am_alloc();		/* (size, flags) AllocMem */
extern void am_free();			/* (p, size) FreeMem */
extern char *am_openlib();		/* (name, version) OpenLibrary */
extern void am_closelib();
extern char *am_initres();		/* (resident, seglist) InitResident */
extern char *am_findres();		/* (seglist) the first Resident in it */
extern long am_lvo();			/* (base, lvo, unsigned long r[15]) */
extern int am_intrun();			/* (intnum) run a server chain */
extern int am_intmask;			/* chains with servers, 1 << intnum */
extern char *am_addlib();		/* (struct amlib *) -> base */
extern void am_dellib();
extern void am_addhead(), am_addtail(), am_remove(), am_enqueue();
extern void am_newlist();
extern char *am_remhead(), *am_findname();
extern int am_stricmp();		/* (a, b, n), n -1: no limit */
extern void am_iodone();		/* a device's request is complete */
extern long am_meminuse();

/* amlibs.c */
extern int am_expinit();		/* expansion: the host's boards */
extern void am_expfini();
extern struct amlib am_utility, am_expansion, am_timer;
extern int am_inboard();		/* (va) inside a mapped board */

/* amhunk.c: errors */
#define	AMH_EFORMAT	1
#define	AMH_ENOMEM	2
extern unsigned long am_loadseg();	/* (buf, len, &err) -> BPTR seglist */
extern void am_unloadseg();

/* platform (amxplat.c / hplat.c) */
extern char *amx_alloc();		/* (size, cansleep) */
extern void amx_free();			/* (p, size) */
extern int amx_spl7();			/* returns the old SR */
extern void amx_splx();
extern int amx_ipl();			/* current IPL, 0..7 */
extern void amx_delayus();
extern void amx_time();			/* (unsigned long tv[2]) */
extern void amx_cacheflush();
extern int amx_zorro();			/* (i, struct amx_zboard *) 0 = none */
extern char *amx_iomap();		/* (pa, size) -> kernel va, 0 = fail */
extern unsigned long amx_vtop();	/* kernel va -> physical, for DMA */
extern void amx_iounmap();
extern int amx_intattach();		/* (intnum) route that chain to us */
extern void amx_intdetach();
extern void amx_log();			/* (fmt, a, b, c, d): %s %d %x */
extern int amx_attnflags();		/* ExecBase AttnFlags for this CPU */
extern int amx_lock();			/* serialise callers; 0 = held */
extern int amx_trylock();		/* the same without sleeping */
extern void amx_unlock();

#endif
