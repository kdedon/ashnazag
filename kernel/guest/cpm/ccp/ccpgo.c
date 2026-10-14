/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/************************************************************************
*									*
*		The CCP's half of the program load			*
*									*
*	When the CCP was resident, `go.c' did the whole job: it built	*
*	the Load Parameter Block, called BDOS function 59, filled the	*
*	base page and jumped into the program with `xfer'.  Half of	*
*	that is CCP work and half of it is system work, and a		*
*	transient CCP can only do the first half:			*
*									*
*	  - the two base-page FCBs come from parsing the command tail,	*
*	    which only the CCP can do (`fill_fcb' reads `parm', `index'	*
*	    and `nd_argu', all of them CCP parse state);		*
*	  - the transfer of control is an IRET in SYSTEM mode		*
*	    (`src/glue.s' `xfer_': it writes NSPSEG/NSPOFF and resets	*
*	    the system stack).  A program in the TPA runs in Normal	*
*	    mode and cannot do it.					*
*									*
*	So the CCP leaves a request in the state page and warm boots.	*
*	`sys/ccprun.c' picks the request up on the way back and does	*
*	the second half.  CP/M 3 needs no such split because its CCP	*
*	and its programs share one address space and one privilege	*
*	level: `ccp3.asm' calls the LOADER RSX and then jumps to 100h	*
*	itself.								*
*									*
*	This is also why the request carries the FCBs and not the	*
*	command: the system does not parse command lines, the CCP	*
*	does, and function 47 (chain to program, `bdosmain.c:629')	*
*	already exists for the other direction -- a PROGRAM handing a	*
*	command line back to the CCP.					*
*									*
************************************************************************/

#include	"ccpdef.h"

#include	"ccpsv.h"

extern UWORD	bdos();
extern UWORD	fill_fcb();
extern BYTE	cmdfcb[];		/* the FCB cmd_file opened	*/

/*  THE CCP'S WORKING COPY OF ITS STATE.  The state itself is resident,
    one per process (src/bdos/proc.c); this is the copy the CCP works in,
    exchanged with it by BDOS 150/151.  It is defined here rather than in
    ccp.c because ccp.c reaches every field through the macros in ccpsv.h
    and never names the struct.

    Its address is what makes the pointer fields work: BSS is at a fixed
    link address, so this buffer sits at the same address in every
    instance of the CCP, and a pointer into it stored before a program
    load is still valid after one.  */

union ccpsvu	ccpsv_buf;

/* a new session's state: the system hands over zeros */
VOID ccpsvinit()
{
	REG BYTE	*q;
	REG UWORD	i;

	q = ccpsv_buf.pad;
	for (i = 0; i < CCPSVLEN; i++)
		q[i] = 0;
	CCPSV->sv_magic = CCPSVMAGIC;
	CCPSV->sv_first_sub = 1;
	CCPSV->sv_dirflag = 1;
	CCPSV->sv_cur_disk = 1;
}
				/* `tail' needs no declaration: it	*/
				/*  is a field of the state page	*/
				/*  (ccpsv.h) and ccp.c reaches it by	*/
				/*  the same name			*/


		/********************************/
		/*				*/
		/*        _ _ L O A D           */
		/*				*/
		/********************************/

VOID __LOAD()
{
	REG BYTE	*tp;
	REG BYTE	*sp;
	REG UWORD	i;
	REG UWORD	tlen;

/*  The FCB of the file cmd_file() opened.  It has to be copied out
    before fill_fcb() is called, because fill_fcb() rebuilds cmdfcb in
    place for the base page -- go.c relied on the load having already
    happened by then, and here the load has not happened yet.	*/

	for (i = 0; i < FCB_LEN; i++)
		CCPSV->sv_pfcb[i] = cmdfcb[i];

/*  Command tail.  go.c computed the length the same way, because the
    CCP does not supply one.	*/

	tlen = 0;
	sp = CCPSV->sv_ptail;
	for (tp = tail; *tp != NULL; tp++)
		if (tlen < SV_CMDLEN) {
			*sp++ = *tp;
			tlen++;
		}
	CCPSV->sv_ptlen = tlen;

/*  Clear the rest.  go.c copied 127 bytes straight out of `usercmd', so
    what followed the tail was the NUL the console read left; here the
    buffer is reused across commands and what follows would be the
    previous command's bytes.  A stock tool that scans the tail for a
    terminator rather than trusting the length byte reads them: STAT saw
    `STAT HELLO.TXT' as two arguments and printed the SET form of its
    output.	*/

	for (i = tlen; i < SV_CMDLEN; i++)
		CCPSV->sv_ptail[i] = NULL;

/*  The two base-page FCBs, parsed out of the tail.  */

	fill_fcb(1, cmdfcb);
	for (i = 0; i < FCB_LEN; i++)
		CCPSV->sv_pfcb1[i] = cmdfcb[i];

	fill_fcb(2, cmdfcb);
	for (i = 0; i < FCB_LEN; i++)
		CCPSV->sv_pfcb2[i] = cmdfcb[i];

/*  Post the request, hand the state back, and go.  The CCP writes its
    state into its working copy as it runs rather than saving it here, so
    posting the request is the last thing that has to change before the
    copy is stored, and there is no window in which a request is stored
    and the state behind it is not.	*/

	CCPSV->sv_pend = 1;
	bdos(CCPSV_PUT, (long) CCPSV);	/* the working copy is in the TPA,
					   and the program about to be
					   loaded will overwrite it	*/
	bdos(WARMBOOT, 0L);		/* does not return		*/
}
