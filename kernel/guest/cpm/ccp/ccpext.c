/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: GPL-2.0
 */
/*--------------------------------------------------------------*\
 |	ccpext.c      CCP CONFIGURABLE EXTENSIONS			|
 |		      ==========================			|
 |								|
 |	Z-System/ZCPR3-style shell conveniences folded into the	|
 |	DRI CCP as OPTIONS.  Nothing here changes the shell	|
 |	until A:CCP.CFG configures it: with no CCP.CFG (or an	|
 |	empty one) every accessor below reports "not		|
 |	configured" and ccp.c takes exactly the stock paths.	|
 |								|
 |	Provided:						|
 |	  - a command search path (ordered drive/user list)	|
 |	  - named directories (NAME: -> drive + user area)	|
 |	  - IF/ELSE/FI flow control for submit files		|
 |	  - an error-handler command for unresolvable commands	|
 |								|
 |	CCP.CFG is a plain text file read once, from the	|
 |	default drive, at the CCP's first command.		|
 |								|
\*--------------------------------------------------------------*/

#include	"ccpdef.h"

#include	"ccpsv.h"	/* the state page		*/

/*--------------------------------------------------------------*\
 |		     Configuration state			|
 |								|
 |	cfg_done, flow_en, path_n, path_d, path_u, nd_n,	|
 |	nd_nam, nd_d, nd_u, errcmd, fstk and fdep are fields	|
 |	of the state page (ccpsv.h), reached by these same	|
 |	names.  The configuration is there so CCP.CFG is read	|
 |	once per session rather than once per command; the	|
 |	flow stack is there because a submit file that warm-	|
 |	boots out of an IF branch has to come back still	|
 |	inside it.						|
 |								|
 |	cfcb, cbuf and cline stay here: cfg_init writes all	|
 |	three before it reads them and nothing outside it	|
 |	touches them, so a fresh copy per load is the same	|
 |	value.							|
\*--------------------------------------------------------------*/

BYTE cfcb[FCB_LEN];		/* fcb used to read CCP.CFG	*/
BYTE cbuf[DMA_LEN];		/* CCP.CFG record buffer	*/
BYTE cline[CFGLINE];		/* one CCP.CFG line		*/

/*--------------------------------------------------------------*\
 |		    Imports from the CCP			|
\*--------------------------------------------------------------*/

extern UWORD bdos();		/* this returns a word		*/
extern UWORD strcmp();		/* this returns a word		*/
extern UWORD fill_fcb();	/* this returns a word		*/
extern BYTE *argp();		/* this returns a ptr to a byte	*/
extern BYTE cmdfcb[];		/* CCP command fcb		*/
extern BYTE dma[];		/* CCP dma buffer		*/

#define cbdos(c,a) bdos(c,(long)a)
				/* call BDOS with an address in	*/
				/*  the caller's memory space --	*/
				/*  see the same macro in ccp.c	*/
#define streq(a,b) (strcmp(a,b) == MATCH)

BYTE *skipb();			/* this returns a ptr to a byte	*/
UWORD dutok();			/* this returns a word		*/

				/********************************/
BYTE *skipb(p)			/*  skip blanks and tabs	*/
				/********************************/
REG BYTE *p;
{
	while(*p == ' ' || *p == TAB)
		p++;
	return(p);
}

				/********************************/
UWORD dutok(p)			/*  decode a dU token:		*/
				/*  drive letter or '*', then	*/
				/*  a user number or '*'.	*/
				/*  Returns (drive<<8)|user with	*/
				/*  0xFF halves meaning "the	*/
				/*  one in effect", DUBAD if	*/
				/*  the token is malformed.	*/
				/********************************/
REG BYTE *p;
{
	REG UWORD d,u;

	d = 0xFF;
	u = 0xFF;
	if(*p >= 'A' && *p <= 'P')
		d = *p++ - 'A';
	else
		if(*p == '*')
			p++;
		else
			return(DUBAD);
	if(*p >= '0' && *p <= '9')
	{
		u = *p++ - '0';
		if(*p >= '0' && *p <= '9')
			u = (u * 10) + (*p++ - '0');
		if(u > 15)
			return(DUBAD);
	}
	else
		if(*p == '*')
			p++;
	if(*p > ' ')
		return(DUBAD);
	return((d << 8) | u);
}

				/********************************/
VOID cfg_line()			/*  act on one CCP.CFG line	*/
				/********************************/
{
	REG BYTE *p,*q;
	REG UWORD i;
	UWORD du;
	BYTE  w[NDNAME+1];

	p = skipb(cline);
	if(*p == NULL || *p == ';')
		return;
	i = 0;
	while(*p > ' ' && i < NDNAME)
		w[i++] = *p++;
	w[i] = NULL;
	while(*p > ' ')			/* discard an over-long keyword	*/
		p++;
	p = skipb(p);

	if(streq(w,"PATH"))
	{
		while(*p != NULL && path_n < PATHMAX)
		{
			du = dutok(p);
			if(du == DUBAD)
				break;
			path_d[path_n] = (du >> 8);
			path_u[path_n] = (du & 0xFF);
			path_n++;
			while(*p > ' ')
				p++;
			p = skipb(p);
		}
		return;
	}
	if(streq(w,"DIR") || streq(w,"NDIR"))
	{
		if(nd_n >= NDMAX)
			return;
		q = p;
		while(*q != NULL && *q != '=')
			q++;
		if(*q != '=')
			return;
		i = 0;
		while(p < q && *p > ' ' && i < NDNAME)
			nd_nam[nd_n][i++] = *p++;
		nd_nam[nd_n][i] = NULL;
		if(i == 0)
			return;
		du = dutok(skipb(q+1));
		if(du == DUBAD)
			return;
		nd_d[nd_n] = (du >> 8);
		nd_u[nd_n] = (du & 0xFF);
		nd_n++;
		return;
	}
	if(streq(w,"ERROR"))
	{
		i = 0;
		while(*p > ' ' && i < NDNAME)
			errcmd[i++] = *p++;
		errcmd[i] = NULL;
		return;
	}
	if(streq(w,"FLOW"))
	{
		flow_en = (p[0] == 'O' && p[1] == 'F') ? FALSE : TRUE;
		return;
	}
}

				/********************************/
VOID cfg_init()			/*  read A:CCP.CFG once.	*/
				/*  A missing file leaves every	*/
				/*  option off, i.e. the stock	*/
				/*  CCP.				*/
				/********************************/
{
	REG UWORD i,n;
	REG BYTE *p;
	BYTE eof;

	if(cfg_done)
		return;
	cfg_done = TRUE;
	for(i = 0;i < FCB_LEN;i++)
		cfcb[i] = ZERO;
	p = "CCP     CFG";
	for(i = 0;i < 11;i++)
		cfcb[i+1] = p[i];
	if(cbdos(OPEN_FILE,cfcb) > 3)
		return;
	cbdos(SET_DMA_ADDR,cbuf);
	n = 0;
	eof = FALSE;
	while(!(eof) && cbdos(READ_SEQ,cfcb) == 0)
	{
		for(i = 0;i < DMA_LEN;i++)
		{
			if(cbuf[i] == EOF || cbuf[i] == NULL)
			{
				eof = TRUE;
				break;
			}
			if(cbuf[i] == Cr || cbuf[i] == Lf)
			{
				cline[n] = NULL;
				cfg_line();
				n = 0;
			}
			else
				if(n < CFGLINE-1)
					cline[n++] = toupper(cbuf[i]);
		}
	}
	cline[n] = NULL;
	cfg_line();
	cbdos(CLOSE_FILE,cfcb);
	cbdos(SET_DMA_ADDR,dma);
}

/*--------------------------------------------------------------*\
 |		    Search path accessors			|
\*--------------------------------------------------------------*/

				/********************************/
UWORD cfg_path()		/*  number of PATH groups	*/
				/*  (0 = no PATH configured)	*/
				/********************************/
{
	return(path_n);
}

				/********************************/
UWORD cfg_pathdu(g)		/*  PATH group g as		*/
				/*  (drive<<8)|user; a 0xFF	*/
				/*  half means "the current one"	*/
				/********************************/
REG UWORD g;
{
	return((path_d[g] << 8) | path_u[g]);
}

/*--------------------------------------------------------------*\
 |		    Named directory accessors			*
\*--------------------------------------------------------------*/

				/********************************/
UWORD nd_any()			/*  any named directories?	*/
				/********************************/
{
	return(nd_n);
}

				/********************************/
UWORD nd_find(name)		/*  look a name up; returns	*/
				/*  (drive<<8)|user, or DUBAD	*/
				/*  when the name is unknown	*/
				/********************************/
REG BYTE *name;
{
	REG UWORD i;

	for(i = 0;i < nd_n;i++)
		if(streq(name,&nd_nam[i][0]))
			return((nd_d[i] << 8) | nd_u[i]);
	return(DUBAD);
}

/*--------------------------------------------------------------*\
 |		      Error handler hook			|
\*--------------------------------------------------------------*/

				/********************************/
BYTE *cfg_errcmd()		/*  the ERROR handler command	*/
				/*  name ("" when none)		*/
				/********************************/
{
	return(&errcmd[0]);
}

/*--------------------------------------------------------------*\
 |		  Last-command error predicate			|
\*--------------------------------------------------------------*/

/*  The IF ERROR predicate.  BDOS function 108's program return code
 *  is the one place the status lives, for both kinds of failure:
 *
 *    - a transient's own exit status, which the program sets by
 *	calling fn 108 itself, and which the BDOS also sets when it
 *	ends a program (RC_CTLC on ^C, RC_BDOS on an error abort);
 *    - the CCP's own failures (an unresolvable command, a .SUB that
 *	is not there, TYPE/ERA reporting "No file"), which reach the
 *	same word through ccp_seterr().
 *
 *  execute_cmd() clears it (ccp_seterr(FALSE)) before every command
 *  it runs, so a program that never calls fn 108 reports success and
 *  a code left by an earlier command cannot be read twice.  The clear
 *  happens after the IF/ELSE/FI words are consumed, which is why an
 *  `IF ERROR' still sees the previous command's status.  Warm boot
 *  does not touch the code (bdosmisc.c), so it survives the transient
 *  exit that hands the console back to the CCP.
 */

				/********************************/
UWORD ccp_err()			/*  did the last command fail?	*/
				/********************************/
{
	return(bdos(PROG_RC,(long)RC_GET));
}

				/********************************/
VOID ccp_seterr(v)		/*  record command success (0)	*/
				/*  or failure (nonzero)	*/
				/********************************/
REG UWORD v;
{
	bdos(PROG_RC,(long)((v) ? 1 : 0));
}

/*--------------------------------------------------------------*\
 |		    IF / ELSE / FI flow control			|
\*--------------------------------------------------------------*/

				/********************************/
UWORD flow_on()			/*  is flow control enabled?	*/
				/********************************/
{
	return(flow_en);
}

				/********************************/
UWORD flow_skip()		/*  are we inside a branch	*/
				/*  that must not execute?	*/
				/********************************/
{
	REG UWORD i;

	for(i = 0;i < fdep;i++)
		if(fstk[i] != F_EXEC)
			return(TRUE);
	return(FALSE);
}

				/********************************/
VOID flow_clear()		/*  drop all open IFs		*/
				/********************************/
{
	fdep = 0;
}

				/********************************/
UWORD fexist(n)			/*  does the file named by	*/
				/*  argument n exist?		*/
				/********************************/
REG UWORD n;
{
	REG BYTE *p;

	p = argp(n);
	if(*p == NULL)
		return(FALSE);
	if(fill_fcb(n,cmdfcb) > 11)
		return(FALSE);
	return(cbdos(SEARCH_FIRST,cmdfcb) != 255);
}

				/********************************/
UWORD flow_line()		/*  handle IF/ELSE/FI.  TRUE	*/
				/*  when the line was one of	*/
				/*  them and is now consumed.	*/
				/********************************/
{
	REG BYTE *c,*a;
	REG UWORD n;
	UWORD v,neg;

	c = argp(0);
	if(streq(c,"ELSE"))
	{
		if(fdep > 0)
		{
			if(fstk[fdep-1] == F_EXEC)
				fstk[fdep-1] = F_SKIP;
			else
				if(fstk[fdep-1] == F_SKIP)
					fstk[fdep-1] = F_EXEC;
		}
		return(TRUE);
	}
	if(streq(c,"FI") || streq(c,"ENDIF"))
	{
		if(fdep > 0)
			fdep--;
		return(TRUE);
	}
	if(!(streq(c,"IF")))
		return(FALSE);
	if(fdep >= IFMAX)
		return(TRUE);		/* too deeply nested: ignore	*/
	if(flow_skip())
	{				/* a dead branch stays dead	*/
		fstk[fdep++] = F_DEAD;	/* through its own ELSE		*/
		return(TRUE);
	}
	n = 1;
	neg = FALSE;
	a = argp(1);
	if(*a == '~')
	{
		neg = TRUE;
		a++;
		if(*a == NULL)
		{
			n = 2;
			a = argp(2);
		}
	}
	else
		if(streq(a,"NOT"))
		{
			neg = TRUE;
			n = 2;
			a = argp(2);
		}
	if(streq(a,"ERROR"))
		v = (ccp_err() != 0);
	else
		if(streq(a,"EXIST"))
			v = fexist(n+1);
		else
			if(streq(a,"TRUE"))
				v = TRUE;
			else
				v = FALSE;	/* unknown condition	*/
	if(neg)
		v = !v;
	fstk[fdep++] = (v) ? F_EXEC : F_SKIP;
	return(TRUE);
}
