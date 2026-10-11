/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/* CCP state preserved across transient reloads.
 *
 * It holds every value that must survive __LOAD: program-launch data,
 * chained-command and SUBMIT state, CCP.CFG state, and pointers into its own
 * buffers. Scratch parse and DMA buffers remain in transient BSS because each
 * CCP invocation initializes them before use.
 *
 * THIS IS RESIDENT PER-PROCESS STORAGE, NOT A PAGE IN THE TPA.  It used to
 * sit at a fixed TPA offset of 0xFA00, which made it the first thing standing
 * in the way of a TPA segment shorter than 64 KB, and left it within reach of
 * a program that ran past @MXTPA.  The system now keeps one per process
 * descriptor (src/bdos/proc.c), the transient CCP keeps a working copy in its
 * own BSS, and the two are exchanged by the two BDOS calls below.  Nothing in
 * the TPA is reserved for it any more, so the RSX ceiling is the top of the
 * segment (src/bdos/rsx.c) and every program gets those 1,536 bytes back.
 *
 * The exchange is per process by construction: both calls act on the
 * descriptor of the process that makes them, so two sessions never share one.
 */

#define	CCPSVLEN	0x0600		/* the size this must not exceed */

#define	CCPSVMAGIC	0x4343		/* 'CC': the state is initialised */

/*  The two BDOS calls the transient CCP reaches its state through.  The
    parameter is the address of its own copy.  Resident storage is SYS-only
    (src/bios/crt.s), so a Normal-mode program cannot address it directly and
    these two calls are the whole of the interface.  */

#define	CCPSV_GET	150		/* resident state -> my buffer	*/
#define	CCPSV_PUT	151		/* my buffer -> resident state	*/

/*  Command-line and buffer sizes.  These repeat ccpdef.h's CMD_LEN,
    FCB_LEN, ERRLINE, PATHMAX, NDMAX, NDNAME and IFMAX rather than
    including it, because sys/ side files (ccprun.c) need the layout
    without the CCP's macro soup -- and a mismatch is caught at cold
    boot by the size check in ccpsvinit().  */

#define	SV_CMDLEN	128		/* ccpdef.h CMD_LEN		*/
#define	SV_FCBLEN	36		/* ccpdef.h FCB_LEN		*/
#define	SV_ERRLINE	130		/* ccpdef.h ERRLINE		*/
#define	SV_PATHMAX	8		/* ccpdef.h PATHMAX		*/
#define	SV_NDMAX	8		/* ccpdef.h NDMAX		*/
#define	SV_NDNAME	8		/* ccpdef.h NDNAME		*/
#define	SV_IFMAX	8		/* ccpdef.h IFMAX		*/

struct ccpsv {

    /*  ---- the system half.  ccprun.c reads and writes these; keep
	them first so the resident side never depends on the layout of
	anything below.  ---- */

    unsigned int sv_magic;	/* CCPSVMAGIC once ccpsvinit() has run	*/
    unsigned int sv_pend;	/* a program load is pending: the CCP	*/
				/*  resolved a command and warm booted	*/
				/*  instead of loading it itself, which	*/
				/*  it cannot do -- the transfer is an	*/
				/*  IRET in system mode (`glue.s' xfer_)*/
    char	sv_pfcb[SV_FCBLEN];	/* the program's open FCB	*/
    char	sv_pfcb1[SV_FCBLEN];	/* base-page FCB 1 (tail arg 1)	*/
    char	sv_pfcb2[SV_FCBLEN];	/* base-page FCB 2 (tail arg 2)	*/
    char	sv_ptlen;		/* command tail length		*/
    char	sv_ptail[SV_CMDLEN];	/* command tail, no length byte	*/

    /*  ---- the CCP half.  One field per mutable global of ccp.c and
	ccpext.c that has to outlive a load.  ---- */

    char	sv_load_try;
    char	sv_first_sub;
    char	sv_chain_sub;
    char	sv_end_of_file;
    char	sv_dirflag;
    char	sv_subprompt;
    char	sv_in_errhook;
    char	sv_autost;
    char	sv_submit;
    char	sv_morecmds;
    unsigned int sv_sub_index;
    unsigned int sv_user;
    unsigned int sv_cur_disk;

    char	sv_subfcb[SV_FCBLEN];	/* the OPEN .SUB file: extent,	*/
					/*  record count and record	*/
					/*  position are bytes 12-15	*/
					/*  and 32-35 of this array	*/
    char	sv_subcom[SV_CMDLEN+1];
    char	sv_subdma[SV_CMDLEN];
    char	sv_save_sub[SV_CMDLEN+1];
    char	sv_usercmd[SV_CMDLEN+2];
    char	sv_errline[SV_ERRLINE];

    char	*sv_user_ptr;		/* all three point into		*/
    char	*sv_glb_index;		/*  sv_usercmd or sv_subcom	*/
    char	*sv_tail;

    /* CCP.CFG is read once per session. The flow stack survives warm boots
	within a submit-file branch. */

    char	sv_cfg_done;
    char	sv_flow_en;
    char	sv_path_n;
    char	sv_nd_n;
    char	sv_path_d[SV_PATHMAX];
    char	sv_path_u[SV_PATHMAX];
    char	sv_nd_nam[SV_NDMAX][SV_NDNAME+1];
    char	sv_nd_d[SV_NDMAX];
    char	sv_nd_u[SV_NDMAX];
    char	sv_errcmd[SV_NDNAME+1];
    char	sv_fstk[SV_IFMAX];
    unsigned int sv_fdep;

    /* PROFILE.SUB has been checked for this session. */

    char	sv_profile;
};

/* Map the CCP globals to the transient's own copy of the state.  The pointer
 * fields stay valid across a reload because the buffer is BSS at a fixed link
 * address, so it is at the same address in every instance of the CCP -- which
 * is also why the SYSTEM cannot initialise them: only the CCP knows where its
 * own buffer is.  ccpsvinit() leaves them null and ccp.c's main() points them
 * at its own usercmd when it finds them null. */

#ifdef CCPTRANSIENT

/* the whole CCPSVLEN, which is what 150 and 151 move */
extern union ccpsvu {
	struct ccpsv	sv;
	char		pad[CCPSVLEN];
} ccpsv_buf;

#define	CCPSV		(&ccpsv_buf.sv)

/* cc0 compares only the first eight identifier characters. Keep every macro
   in this block distinct within that prefix. */

#define	profile_done	(CCPSV->sv_profile)
#define	load_try	(CCPSV->sv_load_try)
#define	first_sub	(CCPSV->sv_first_sub)
#define	chain_sub	(CCPSV->sv_chain_sub)
#define	end_of_file	(CCPSV->sv_end_of_file)
#define	dirflag		(CCPSV->sv_dirflag)
#define	subprompt	(CCPSV->sv_subprompt)
#define	in_errhook	(CCPSV->sv_in_errhook)
#define	autost		(CCPSV->sv_autost)
#define	submit		(CCPSV->sv_submit)
#define	morecmds	(CCPSV->sv_morecmds)
#define	sub_index	(CCPSV->sv_sub_index)
#define	user		(CCPSV->sv_user)
#define	cur_disk	(CCPSV->sv_cur_disk)
#define	subfcb		(CCPSV->sv_subfcb)
#define	subcom		(CCPSV->sv_subcom)
#define	subdma		(CCPSV->sv_subdma)
#define	save_sub	(CCPSV->sv_save_sub)
#define	usercmd		(CCPSV->sv_usercmd)
#define	errline		(CCPSV->sv_errline)
#define	user_ptr	(CCPSV->sv_user_ptr)
#define	glb_index	(CCPSV->sv_glb_index)
#define	tail		(CCPSV->sv_tail)

#define	cfg_done	(CCPSV->sv_cfg_done)
#define	flow_en		(CCPSV->sv_flow_en)
#define	path_n		(CCPSV->sv_path_n)
#define	nd_n		(CCPSV->sv_nd_n)
#define	path_d		(CCPSV->sv_path_d)
#define	path_u		(CCPSV->sv_path_u)
#define	nd_nam		(CCPSV->sv_nd_nam)
#define	nd_d		(CCPSV->sv_nd_d)
#define	nd_u		(CCPSV->sv_nd_u)
#define	errcmd		(CCPSV->sv_errcmd)
#define	fstk		(CCPSV->sv_fstk)
#define	fdep		(CCPSV->sv_fdep)


#endif	/* CCPTRANSIENT */
