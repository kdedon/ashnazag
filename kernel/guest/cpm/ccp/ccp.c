/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 */
/*--------------------------------------------------------------*\
 |	ccp.c	      CONSOLE COMMAND PROCESSOR          v1.0   |
 |		      =========================			|
 |								|
 |      P- CP/M:  A CP/M derived operating system		|
 |								|
 |      Description:					        |
 |	-----------						|
 |			The Console Command Processor is a      |
 |			distinct program which references       |
 |			the BDOS to provide a human-oriented    |
 |			interface for the console user to the   |
 |			information maintained by the BDOS on   |
 |			disk storage.				|
 |								|
 |	created by :    Tom Saulpaugh	Date created: 7/13/82	|
 |	----------			------------		|
 |      last modified:  12/20/82				|
 |      -------------						|
 |     								|
 |	(c) COPYRIGHT  Digital Research 1982			|
 |	               all rights reserved			|
 |								|
\*--------------------------------------------------------------*/


/*--------------------------------------------------------------*\
 |		     CCP Macro Definitions			|
\*--------------------------------------------------------------*/
#include	"ccpdef.h"        /* include CCP defines	*/

#include	"ccpsv.h"	  /* the state page: every global*/
				  /*  below that outlives a load	*/
				  /*  is a field of it		*/

			
/*--------------------------------------------------------------*\
 |		    CP/M Builtin Command Table			|
\*--------------------------------------------------------------*/
struct _cmd_tbl
{
	BYTE	*ident;		/* command identifer field	*/
	UWORD	cmd_code;	/* command code field		*/
}
cmd_tbl[8] = 			/* declare CP/M built-in table	*/
{
	"DIR",DIRCMD,
       "DIRS",DIRSCMD,
       "TYPE",TYPECMD,
	"REN",RENCMD,
	"ERA",ERACMD,
       "USER",UCMD,
     "SUBMIT",SUBCMD,
	 NULL,-1
};		


/*--------------------------------------------------------------*\
 |		 Table of User Prompts and Messages		|
\*--------------------------------------------------------------*/
BYTE	msg[]  = "NON-SYSTEM FILE(S) EXIST$";
BYTE	msg2[] = "Enter Filename: $";
BYTE	msg3[] = "Enter Old Name: $";
BYTE	msg4[] = "Enter New Name: $";
BYTE	msg5[] = "File already exists$";
BYTE	msg6[] = "No file$";
BYTE	msg7[] = "No wildcard filenames$";
BYTE	msg8[] = "Syntax: REN Newfile=Oldfile$";
BYTE	msg9[] = "Confirm(Y/N)? $";
BYTE   msg10[] = "Enter User No: $";
BYTE   msg11[] = ".SUB file not found$";
BYTE   msg12[] = "User # range is [0-15]$";
BYTE   msg13[] = "Too many arguments: $";
BYTE   msg14[] = " required$";
/*--------------------------------------------------------------*\
 |		    Global Arrays & Variables			|
\*--------------------------------------------------------------*/

				/********************************/
				/*  load_try, first_sub,	*/
				/*  chain_sub, submit,		*/
				/*  end_of_file, dirflag,	*/
				/*  subprompt, morecmds,	*/
				/*  sub_index, user, cur_disk,	*/
				/*  subcom, subdma, usercmd,	*/
				/*  user_ptr, glb_index,	*/
				/*  save_sub, subfcb, tail,	*/
				/*  autost, in_errhook and	*/
				/*  errline were file-scope	*/
				/*  variables here when the CCP	*/
				/*  was resident and nothing	*/
				/*  ever reloaded it.  They are	*/
				/*  now fields of the state page	*/
				/*  (ccpsv.h), reached by the	*/
				/*  same names -- the code below	*/
				/*  is unchanged.		*/
				/********************************/
UWORD index = ZERO;		/* index into cmd argument array*/
BYTE cmdfcb[FCB_LEN];		/* global fcb for Z8k files	*/
BYTE dma[DMA_LEN+3];		/* 128 byte dma buffer		*/
BYTE parm[MAX_ARGS][ARG_LEN];	/* cmd argument array		*/
BYTE del[] =	 		/* CP/M-68K set of delimeters   */
{'>','<','.',',','=','[',']',';','|','&','/','(',')','+','-','\\'};
BYTE nd_argu[4] =		/* per-argument user number from*/
{CURDU,CURDU,CURDU,CURDU};	/*  a named directory prefix,	*/
				/*  CURDU where there was none	*/
				/*  (one entry per MAX_ARGS)	*/
BYTE nullcmd[2] =		/* an empty command line: the	*/
{NULL,NULL};			/*  submit loop asks for the	*/
				/*  next line by pointing at it	*/
BYTE *xcmd;			/* transient a built-in hands	*/
				/*  its command to, or NULL	*/

/*--------------------------------------------------------------*\
 |		      Function Definitions			|
\*--------------------------------------------------------------*/

				/********************************/
extern UWORD bdos();		/* this returns a word          */
BYTE *scan_cmd();		/* this returns a ptr to a byte */
UWORD strcmp();			/* this returns a word		*/
UWORD decode();			/* this returns a word		*/
UWORD delim();			/* this returns a word		*/
BYTE true_char();		/* this returns a byte		*/
UWORD fill_fcb();		/* this returns a word		*/
UWORD user_cmd();		/* this returns a word		*/
UWORD cmd_file();		/* this returns a word		*/
UWORD dollar();			/* this returns a word		*/
UWORD comments();		/* this returns a word		*/
UWORD submit_cmd();		/* this returns a word		*/
				/********************************/


				/********************************/
				/*  CCP extensions (ccpext.c);	*/
				/*  every one of these reports	*/
				/*  "not configured" until	*/
				/*  CCP.CFG says otherwise	*/
				/********************************/
extern VOID  cfg_init();	/* read CCP.CFG once		*/
extern UWORD cfg_path();	/* number of search groups	*/
extern UWORD cfg_pathdu();	/* search group as (drive<<8)|user*/
extern BYTE *cfg_errcmd();	/* ERROR handler command name	*/
extern UWORD nd_any();		/* any named directories?	*/
extern UWORD nd_find();		/* name -> (drive<<8)|user	*/
extern UWORD flow_on();		/* IF/ELSE/FI enabled?		*/
extern UWORD flow_line();	/* consume an IF/ELSE/FI line	*/
extern UWORD flow_skip();	/* inside a false branch?	*/
extern VOID  flow_clear();	/* drop all open IFs		*/
extern UWORD ccp_err();		/* did the last command fail?	*/
extern VOID  ccp_seterr();	/* record success or failure	*/

BYTE *argp();			/* this returns a ptr to a byte */
UWORD map_names();		/* this returns a word		*/
UWORD errhook();		/* this returns a word		*/

#define cbdos(c,a) bdos(c,(long)a)
				/* call BDOS with an address in	*/
				/*  the caller's memory space.	*/
#define SET_ERRMODE	45
#define XERR(r)	((r) > 0xFF)	/* an extended error came back	*/
				/*  When the CCP was resident	*/
				/*  that meant map_adr(a,0) -- a	*/
				/*  BDOS data-segment address is	*/
				/*  not what the BDOS wants to	*/
				/*  see.  A transient's pointer	*/
				/*  IS the XADDR the BDOS wants	*/
				/*  (user/bdossc.s), so the	*/
				/*  mapping step goes away.	*/


				/********************************/
VOID cr_lf()			/*   print a CR and a Linefeed	*/
				/********************************/
{
	bdos(CONSOLE_OUTPUT,CR);
	bdos(CONSOLE_OUTPUT,LF);
}


				/********************************/
UWORD strcmp(s1,s2)	  	/*    compare 2 char strings	*/
				/********************************/
REG BYTE *s1,*s2;
{		
	while(*s1)
	{
		if(*s1 > *s2)
			return(1);
		if(*s1 < *s2)
			return(-1);
		s1++; s2++;
	}
	return((*s2 == NULL) ? 0 : -1);
}
				/********************************/
VOID copy_cmd(com_index)	/*  Save the command which	*/
				/*  started a submit file       */
				/*  Parameter substituion will  */
				/*  need this command tail.	*/
				/*  The buffer save_sub is used */
				/*  to store the command.	*/
				/********************************/ 
REG BYTE *com_index;
{
	REG BYTE *t1,*temp;

	temp = save_sub;
	if(subprompt)
	{
		t1 = &parm[0][0];
		while(*t1 != NULL)
			*temp++ = *t1++;
		*temp++ = ' ';
		subprompt = FALSE;
	}
	while(*com_index != NULL && *com_index != EXLIMPT)
		*temp++ = *com_index++;
	*temp = NULL;
}



				/********************************/
VOID prompt()  			/*   print the CCP prompt       */
				/********************************/
{
	REG UWORD cur_drive,cur_user_no;
	BYTE      buffer[3];

	cur_user_no = bdos(GET_USER_NO,(long)255);
	cur_drive  = bdos(RET_CUR_DISK,(long)0);		
	cur_drive += 'A';
	cr_lf();
	if (cur_user_no != 0)
	{
		if(cur_user_no >= 10)
		{
			buffer[0] = '1';
			cur_user_no -= 10;
			buffer[1] = (cur_user_no + '0');
			buffer[2] = '$';
		}
		else
		{
			buffer[0] = (cur_user_no + '0');
			buffer[1] = '$';
		}
		cbdos(PRINT_STRING,&buffer[0]);
	}
	bdos(CONSOLE_OUTPUT,(long)cur_drive);
	bdos(CONSOLE_OUTPUT,ARROW);
}



				/********************************/
VOID echo_cmd(cmd,mode)		/* echo any multiple commands   */
				/* or any illegal commands	*/
				/********************************/
REG BYTE *cmd;
REG UWORD mode;
{
	if(mode == GOOD && !(autost))
		prompt();
	while(*cmd != NULL && *cmd != EXLIMPT)
		bdos(CONSOLE_OUTPUT,(long)*cmd++);
	if(mode == BAD)
		bdos(CONSOLE_OUTPUT,(long)'?');
	else
		cr_lf();
}


				/********************************/	
UWORD decode(cmd)		/* Recognize the command as:	*/
				/* ---------			*/
				/* 1. Builtin			*/
				/* 2. File			*/
				/********************************/
REG BYTE *cmd;
{
	REG UWORD i,n;


	/****************************************/
	/* Check for a CP/M builtin command	*/
	/****************************************/
	for(i = 0; i < 7;i++)
		if (strcmp(cmd,cmd_tbl[i].ident) == MATCH)
			return(cmd_tbl[i].cmd_code);
	/********************************************************/
	/*	Check for a change of disk drive command	*/
	/********************************************************/
	i = 0;
	while(i < (ARG_LEN-1) && parm[0][i] != ':')
		i++;
	if(i == 1 && parm[0][2] == NULL && parm[1][0] == NULL)
		if((parm[0][0] - 'A' >= 0) && (parm[0][0] - 'A' <= 15)) 
				return(CH_DISK);
	if(i == 1 && ((parm[0][0] - 'A' < 0) || (parm[0][0] - 'A' > 15)))
		return(-1);
	if(i != 1 && parm[0][i] == ':')
		return(-1);
	/*****************************************************/
	/* Check for Wildcard Filenames			     */
	/* Check First Character of Filename for a Delimeter */
	/*****************************************************/
	if(fill_fcb(0,cmdfcb) > 0)
		return(-1);
	if(i == 1)
		i = 2;
	else
		i = 0;
	for(n = 0; n < sizeof del;n++)
		if(parm[0][i] == del[n])
			return(-1);
	for(n = 0;n < ARG_LEN-1;n++)
		if(parm[0][n] > NULL && parm[0][n] < ' ')
			return(-1);
	return(FILE);
}

					/************************/
VOID check_cmd(tcmd)			/*  Check end of cmd	*/
					/*  for an '!' which    */
					/*  starts another cmd  */
REG BYTE *tcmd;				/************************/
{
	while(*tcmd != NULL && *tcmd != EXLIMPT)
		tcmd++;
	/*----------------------------*/
	/* check for multiple command */
	/*   in case of a warmboot    */	
	/*----------------------------*/
	if(*tcmd++ == EXLIMPT && *tcmd != NULL)
	{
		morecmds = TRUE;
		while(*tcmd == ' ')
			tcmd++;
		user_ptr = tcmd;
	}
	else
		if(submit)	/* check original cmd line */
		{
			if(!(end_of_file))
				morecmds = TRUE;
				/*--------------------------*/
			else	/* restore cmd to where user*/
				/* ptr points to. User_ptr  */
				/* always points to next    */
				/* console command to exec  */
			{	/*--------------------------*/
				submit = FALSE;
				if(*user_ptr != NULL)
					morecmds = TRUE;
			}
		}
		else
			morecmds = FALSE;		
}

					/************************/
VOID get_cmd(cmd,max_chars)	        /*   Read in a command  */
					/*Strip off extra blanks*/
					/************************/
REG BYTE  *cmd;
REG LONG   max_chars;
{
	REG BYTE *c;

	dma[0] = CMD_LEN;	  /* set maximum chars to read  */
	cbdos(READ_CONS_BUF,dma);  /* then read console		*/
	if(dma[1] != 0 && dma[2] != ';')
		cr_lf();
	dma[((UWORD)dma[1] & 0xFF)+2] = '\n'; /* tack on end of line char   */ 
	if(dma[2] == ';')	  /* ';' denotes a comment	*/	
		dma[2] = '\n';
	c = &dma[2];
	while(*c == ' ' || *c == TAB)
		c++;
	while(*c != '\n' && --max_chars > 0)
	{			
		*cmd++ = toupper(*c);
		if(*c == ' ' || *c == TAB)
			while(*++c == ' ' || *c == TAB);
		else
			c++;
	}
	*cmd = NULL;	     /* tack a null character on the end*/
}
					/************************/
BYTE *scan_cmd(com_index)		/* move ptr to next cmd */
					/* in the command line	*/
					/************************/
REG BYTE *com_index;
{
	while((*com_index != EXLIMPT) && 
	      (*com_index != NULL))
		 com_index++;
	while(*com_index == EXLIMPT || *com_index == ' ' ||
	      *com_index == TAB)
		com_index++;
	return(com_index);
}


					/************************/
VOID get_parms(cmd)			/* extract cmd arguments*/
					/* from command line	*/
REG BYTE *cmd;				/************************/
{



	/************************************************/
	/*	This function parses the command line   */
	/* read in by get_cmd().  The expected command  */
	/* from that line is put into parm[0].  All     */
	/* parmeters associated with the command are put*/
	/* in in sequential order in parm[1],parm[2],   */
	/* up to parm[4]. A command ends at a NULL or   */
	/* an exlimation point.				*/
	/************************************************/


	REG BYTE *line;		/* pointer to parm array   */
	REG UWORD    i;		/* Row Index   		   */
	REG UWORD    j;		/* Column Index		   */

	line = parm;
	for(i = 0; i < (MAX_ARGS * ARG_LEN); i++)
		*line++ = NULL;

	i = 0;
	/***************************************************/
	/*  separate command line at blanks,exlimation pts */
	/***************************************************/

        while(*cmd != NULL    &&
	      *cmd != EXLIMPT &&
	      i < MAX_ARGS)
	{
		j = 0;
		while(*cmd != EXLIMPT &&
		      *cmd != ' '     &&
		      *cmd != TAB     &&	
		      *cmd != NULL)
		{
			if(j < (ARG_LEN-1))
				parm[i][j++] = *cmd;
			cmd++;
		}
		parm[i++][j] = NULL;
		if(*cmd == ' ' || *cmd == TAB)
			cmd++;
		if(i == 1)
			tail = cmd; /* mark the beginning of the tail */
	}
}


					/************************/
UWORD delim(ch)				/* check ch to see	*/ 
					/* if it's a delimeter  */
					/************************/
REG BYTE *ch;
{
	if(*ch <= ' ')
		return(TRUE);
	switch(*ch)
	{
		case '>':
		case '<':
		case '.':
		case ',':
		case '=':
		case ':':
		case '+':
		case '-':
		case '&':
		case '/':
		case '\\':
		case '|':
		case '(':
		case ')':
		case '[':
		case ']':
		case ';': return(TRUE);
	}
	return(FALSE);
}



					/************************/
BYTE true_char(ch)			/* return the desired	*/
					/* character for fcb	*/
					/************************/

REG BYTE *ch;
{
	if(*ch == '*') return('?');	/* wildcard		*/

	if(!delim(ch)) 			/* ascii character	*/
	{
		index++;		/* increment cmd index	*/
		return(*ch);
	}

	return(' ');			/* pad field with blank */
}

					/************************/
UWORD fill_fcb(which_parm,fcb)		/* fill the fields of	*/
					/* the file control blk */
					/************************/

REG UWORD which_parm;
REG BYTE  *fcb;
{
	REG BYTE  *ptr;
	REG BYTE  fillch;
	REG UWORD j,k;

	*fcb = 0;
	for(k = 12;k <= 35; k++)        /* fill fcb with zero	*/
		fcb[k] = ZERO;
	for(k = 1;k <= 11;k++)
		fcb[k] = BLANK;	/* blank filename+type  */
	
	/*******************************************/
	/* extract drivecode,filename and filetype */
	/*	 from parmeter blk		   */
	/*******************************************/

	if(dirflag)
		fillch = '?';
	else
		fillch = ' ';

	index = ZERO;
	ptr = fcb;
	if(parm[which_parm][index] == NULL) /* no parmemters  */
	{
		ptr++;
		for(j = 1;j <= 11;j++)
			*ptr++ = fillch; 
		*fcb = (bdos(RET_CUR_DISK,(long)0)+1);
		if(dirflag)
			return(11);
		else
			return(0);
	}
	if(parm[which_parm][index+1] == ':')
	{
		*ptr = parm[which_parm][index] - 'A' + 1;
		index += 2;
		if(parm[which_parm][index] == NULL)
		{
			ptr = &fcb[1];
			for(j = 1;j <= 11;j++)
				*ptr++ = fillch;
			if(dirflag)
				return(11);
			else
				return(0);
		}
	}							
	else	/* fill drivecode with the default disk		*/	
	             *fcb = (bdos(RET_CUR_DISK,(long)0) + 1);

	ptr = fcb;
	ptr++;			/* set pointer to fcb filename */
	for(j = 1;j <= 8;j++)	/* get filename */
	  *ptr++ = true_char(&parm[which_parm][index]);
	while((!(delim(&parm[which_parm][index])))) index++;
	if(parm[which_parm][index] == PERIOD)
	{
	  	index++;
       	  	for(j = 1;j <= 3;j++)	/* get extension */
	  	 	*ptr++ = true_char(&parm[which_parm][index]);
	}
	k = 0;
	for(j = 1;j <= 11;j++)
		if(fcb[j] == '?') k++;

	return(k);	/* return the number of question marks	*/
}


					/************************/
BYTE *argp(n)				/* address of command	*/
					/* argument n		*/
					/************************/
REG UWORD n;
{
	return(&parm[n][0]);
}


					/************************/
UWORD map_names()			/* rewrite each NAME:	*/
					/* prefix in the command*/
					/* line as the plain	*/
					/* drive prefix d: that	*/
					/* the rest of the CCP	*/
					/* already understands. */
					/*----------------------*/
					/* Returns the user	*/
					/* number of the first	*/
					/* name used, plus one,	*/
					/* or zero when no name	*/
					/* was used.  nd_argu	*/
					/* records the user of	*/
					/* each argument's name.*/
					/************************/
{
	REG BYTE  *p;
	REG UWORD i,j,k;
	UWORD du,ret;
	BYTE  nm[NDNAME+1];

	for(i = 0;i < MAX_ARGS;i++)
		nd_argu[i] = CURDU;
	if(!(nd_any()))
		return(0);
	ret = 0;
	for(i = 0;i < MAX_ARGS;i++)
	{
		p = argp(i);
		j = 0;
		while(j < NDNAME && p[j] != NULL && p[j] != ':')
		{
			nm[j] = p[j];
			j++;
		}
		if(j < 2 || p[j] != ':')	/* no name, or a d: prefix */
			continue;
		nm[j] = NULL;
		du = nd_find(nm);
		if(du == DUBAD)
			continue;
		p[0] = (du >> 8) + 'A';	/* NAME:rest -> d:rest	*/
		p[1] = ':';
		k = 2;
		j++;
		while(p[j] != NULL)
			p[k++] = p[j++];
		p[k] = NULL;
		nd_argu[i] = (du & 0xFF);
		if(ret == 0)
			ret = (du & 0xFF) + 1;
	}
	return(ret);
}


					/************************/
UWORD errhook()				/* hand an unresolvable	*/
					/* command to the	*/
					/* configured ERROR	*/
					/* handler: the handler	*/
					/* becomes the command,	*/
					/* the failing words	*/
					/* become its arguments	*/
					/* and the failing line	*/
					/* becomes its tail.	*/
					/*----------------------*/
					/* FALSE when there is	*/
					/* no handler or it too	*/
					/* cannot be resolved.	*/
					/************************/
{
	REG BYTE  *h,*p;
	REG UWORD i,j;
	BYTE  save0[ARG_LEN];

	h = cfg_errcmd();
	if(*h == NULL || in_errhook)
		return(FALSE);
	in_errhook = TRUE;
	for(i = 0;i < MAX_ARGS;i++)
		nd_argu[i] = CURDU;
	p = glb_index;			/* the whole failing command	*/
	i = 0;
	while(*p != NULL && *p != EXLIMPT && i < ERRLINE-1)
		errline[i++] = *p++;
	errline[i] = NULL;
	for(i = 0;i < ARG_LEN;i++)
		save0[i] = parm[0][i];
	for(i = MAX_ARGS-1;i > 0;i--)	/* shift the failing words up	*/
		for(j = 0;j < ARG_LEN;j++)
			parm[i][j] = parm[i-1][j];
	for(i = 0;i < ARG_LEN-1 && h[i] != NULL;i++)
		parm[0][i] = h[i];
	parm[0][i] = NULL;
	tail = errline;
	if(cmd_file(SEARCH))		/* a .SUB handler has started	*/
	{
		in_errhook = FALSE;
		return(TRUE);
	}
	in_errhook = FALSE;		/* a .68K handler never returns	*/
	for(i = 0;i < ARG_LEN;i++)	/* handler missing: put the	*/
		parm[0][i] = save0[i];	/*  failing command back	*/
	return(FALSE);
}





					/************************/
VOID dir_cmd(attrib)	       		/*     print out a	*/
					/*  directory listing   */
					/*----------------------*/
					/* attrib->1 (sysfiles) */
					/* attrib->0 (dirfiles) */
					/************************/
REG UWORD attrib;
{
	BYTE		needcr_lf;
	REG UWORD	dir_index,file_cnt;
 	REG UWORD	save,j,k,curdrive,exist;

	j = 0; exist = FALSE; needcr_lf = FALSE;
	while(parm[1][j] != NULL && parm[1][j] != ':')
		j++;
	if(parm[1][j] == ':' && j != 1)
	{
		echo_cmd(&parm[1][0],BAD);
		return(0);
	}	
	if(parm[2][0] != NULL)
	{
		cbdos(PRINT_STRING,&msg13[0]);
		echo_cmd(&parm[2][0],BAD);
		return(0);
	}
	if(parm[1][1] == ':')
		if((parm[1][0] < 'A') ||
		   (parm[1][0] > 'P'))
		{
			echo_cmd(&parm[1][0],BAD);
			return(0);
		}
	fill_fcb(1,cmdfcb);
	curdrive = (cmdfcb[0] + 'A' - 1);
	dir_index = cbdos(SEARCH_FIRST,cmdfcb);
	if(dir_index == 255)
		cbdos(PRINT_STRING,&msg6[0]);
	save = (32 * dir_index) + 1;
	file_cnt = 0;		
	while(dir_index != 255)
	{
      		if(((attrib) && (dma[save+9] & 0x80)) ||
		  (!(attrib) && (!(dma[save+9] & 0x80))))
	      	{
			if(needcr_lf)
			{
				cr_lf();
				needcr_lf = FALSE;
			}	
			if(file_cnt == 0)
				bdos(CONSOLE_OUTPUT,(long)curdrive);
	      	}
		else
			{
				exist = TRUE;
				dir_index = cbdos(SEARCH_NEXT,cmdfcb);
				save = (32 * dir_index) + 1;
				continue;
			}
		dir_index = (32 * dir_index) + 1;
		bdos(CONSOLE_OUTPUT,COLON);
		bdos(CONSOLE_OUTPUT,BLANKS);
		j = 1;
		while(j <= 11)
		{
			if(j == 9)
				bdos(CONSOLE_OUTPUT,BLANKS);
			bdos(CONSOLE_OUTPUT,(long)(dma[dir_index++] & CMASK));
			j++;
		}
		bdos(CONSOLE_OUTPUT,BLANKS);
		dir_index = cbdos(SEARCH_NEXT,cmdfcb);
		if(dir_index == 255)
			break;
		file_cnt++;
		save = (32 * dir_index) + 1;
		if(file_cnt == FILES_PER_LINE)	/* SS 821221 */
		{
			file_cnt = 0;
			if((attrib && (dma[save+9] & 0x80)) ||
			  (!(attrib) && (!(dma[save+9] & 0x80))))
				cr_lf();
			else
				needcr_lf = TRUE;
		}	 

	}		/*----------------------------------------*/
	if(exist)	/* if files exist that were not displayed */
			/* print out a message to the console	  */
	{		/*----------------------------------------*/
		cr_lf();
		if(attrib)
			cbdos(PRINT_STRING,&msg[0]);
		else
			cbdos(PRINT_STRING,&msg[4]);
	}
}





					/************************/
UWORD fcall(func)			/* a file call on cmdfcb*/
					/* that returns its	*/
					/* extended error	*/
					/************************/
UWORD func;
{
	REG UWORD r;

	bdos(SET_ERRMODE,(long)0xFF);
	r = cbdos(func,cmdfcb);
	bdos(SET_ERRMODE,(long)0);
	return(r);
}


					/************************/
UWORD xform(name)			/* options or a pass-	*/
					/* word: only the	*/
					/* transient takes them	*/
					/************************/
BYTE *name;
{
	REG BYTE *p;

	for(p = tail;*p != NULL && *p != EXLIMPT;p++)
		if(*p == '[' || *p == ';')
		{
			xcmd = name;
			return(TRUE);
		}
	return(FALSE);
}


					/************************/
VOID xfer_cmd()				/* run the transient a	*/
					/* built-in handed its	*/
					/* command to		*/
					/************************/
{
	REG UWORD i;

	for(i = 0;xcmd[i] != NULL;i++)
		parm[0][i] = xcmd[i];
	parm[0][i] = NULL;
	nd_argu[0] = CURDU;
	if(!(cmd_file(SEARCH)))
	{
		for(i = 0;xcmd[i] != NULL;i++)
			bdos(CONSOLE_OUTPUT,(long)xcmd[i]);
		cbdos(PRINT_STRING,&msg14[0]);
		ccp_seterr(TRUE);
	}
}


					/************************/
VOID type_cmd()				/*   type out a file	*/
					/*   to the console 	*/
					/************************/
{
	REG 	UWORD i,r;

	if(xform("TYPE"))
		return(0);
	if(parm[1][0] == NULL)		/*prompt user for filename*/
	{
		cbdos(PRINT_STRING,&msg2[0]);
		get_cmd(&parm[1][0],(long)ARG_LEN-1);
	}
	if(parm[2][0] != NULL)
	{
		cbdos(PRINT_STRING,&msg13[0]);
		echo_cmd(&parm[2][0],BAD);
		return(0);
	}
	i = 0;
	while(parm[1][i] != NULL && parm[1][i] != ':')
		i++;
	if(parm[1][i] == ':')
	{
		if(i != 1 || parm[1][0] < 'A' || parm[1][0] > 'P')
		{	
			echo_cmd(&parm[1][0],BAD);
			return(0);
		}
	}
	i = fill_fcb(1,cmdfcb);		/*fill a file control block*/
	r = 0xFF;
	if(i == 0 && parm[1][0] != NULL)
		r = fcall(OPEN_FILE);
	if(XERR(r))
	{
		xcmd = "TYPE";
		return(0);
	}
	if(r <= 3)
	{
		/*  One function 111 per RECORD, not one function 2 per
		    character.  The BDOS cooks the block exactly as it
		    cooked the characters -- prt_blk() runs the same
		    cookdout() over it, so tabs, ^S/^Q and the column
		    are unchanged -- but TYPE now crosses the SC gate
		    128 times fewer per record.  An embedded ^Z still
		    ends only THIS record's text and the next record is
		    still read, which is what the character loop did.  */
		struct { LONG cbaddr; UWORD cblen; } blk;

		while(cbdos(READ_SEQ,cmdfcb) == 0)
		{
			for(i = 0;i <= 127;i++)
				if(dma[i] == EOF)
					break;
			if(i != 0)
			{
				blk.cbaddr = (LONG)&dma[0];
				blk.cblen = i;
				bdos(PRINT_BLOCK,(long)&blk);
			}
		}
		cbdos(CLOSE_FILE,cmdfcb);
		bdos(RESET_DRIVE,(long)cmdfcb[0]);
	} else
		if(parm[1][0] != NULL)
		{
			ccp_seterr(TRUE);
			if(i > 0)
			{
				if(i == 11)
					echo_cmd(&parm[1][0],BAD);
				else
					cbdos(PRINT_STRING,&msg7[0]);
			}
			else
				cbdos(PRINT_STRING,&msg6[0]);
		}
}



					/************************/						
VOID ren_cmd()				/*    rename a file	*/
					/************************/

{
	BYTE    	new_fcb[FCB_LEN];
	REG UWORD	i,j,k,bad_cmd;

	if(xform("RENAME"))
		return(0);
	bad_cmd = FALSE;	       /*-------------------------*/
	if(parm[1][0] == NULL)	       /*prompt user for filenames*/
	{			       /*-------------------------*/	
		cbdos(PRINT_STRING,&msg3[0]); 
		get_cmd(&parm[3][0],(long)ARG_LEN-1);
		if(parm[3][0] == NULL)
			return(0);
		cbdos(PRINT_STRING,&msg4[0]);  
		get_cmd(&parm[1][0],(long)ARG_LEN-1);
		parm[2][0] = '=';
	}			/*--------------------------------*/
	 else		        /*check for correct command syntax*/
	 {			/*--------------------------------*/
		i = 0;
		while(parm[1][i] != '=' && parm[1][i] != NULL) i++;
		if(parm[1][i] == '=')
		{
			if(!(i > 0 && parm[1][i+1] != NULL &&
			     parm[2][0] == NULL))
				bad_cmd = TRUE;
		}
		else
			if(!(parm[2][0] == '='  &&
			     parm[2][1] == NULL &&
			     parm[3][0] != NULL))
				bad_cmd = TRUE;
		if(!bad_cmd && parm[1][i] == '=')
		{
			parm[1][i] = NULL;
			i++;
			j = 0;
			while((parm[3][j++] = parm[1][i++]) != NULL);
			parm[2][0] = '=';
		}
	}
	for(j = 1;j < 4;j += 2)
	{
		k = 0;
		while(parm[j][k] != ':' && parm[j][k] != NULL)
			k++;
		if(k > 1 && parm[j][k] == ':')
			bad_cmd = TRUE;
		for(i = 0;i < sizeof del;i++)
			if(parm[j][0] == del[i])
			{
				echo_cmd(&parm[j][0],BAD);
				return(0);
			}
	}
	if(!bad_cmd && parm[1][0] != NULL && parm[3][0] != NULL)
	{
		i = fill_fcb(1,new_fcb);
		j = fill_fcb(3,cmdfcb);
		if(i == 0 && j == 0)
		{
			if(new_fcb[0] != cmdfcb[0])
			{
				if(parm[1][1] == ':' && parm[3][1] != ':')
					cmdfcb[0] = new_fcb[0];
				else
				if(parm[1][1] != ':' && parm[3][1] == ':')
					new_fcb[0] = cmdfcb[0];
				else
				bad_cmd = TRUE;
			}
			if(new_fcb[0] < 1 || new_fcb[0] > 16)
				bad_cmd = TRUE;
			if(!(bad_cmd) && cbdos(SEARCH_FIRST,new_fcb) != 255)
			{	/* RENAME offers to delete it, but	*/
				/*  prompted names are not in the tail	*/
				if(*tail == NULL)
					cbdos(PRINT_STRING,&msg5[0]);
				else
					xcmd = "RENAME";
			}
			else{
				k = 0;
				for(i = 16;i <= 35;i++)
					cmdfcb[i] = new_fcb[k++];
				if(cmdfcb[0] < 0 || cmdfcb[0] > 15)
					bad_cmd = TRUE;
				if(!(bad_cmd) &&
				XERR(i = fcall(RENAME_FILE)))
					xcmd = "RENAME";
				else if(!(bad_cmd) && i > 0)
					cbdos(PRINT_STRING,&msg6[0]);
			    }
		}
		else
		 cbdos(PRINT_STRING,&msg7[0]);
	}
	if(bad_cmd)
	         cbdos(PRINT_STRING,&msg8[0]);
}


					/************************/
VOID era_cmd()				/*  erase a file from	*/
					/*  the directory       */
					/************************/
{
	REG 	UWORD i;

	if(xform("ERASE"))
		return(0);
				        /*----------------------*/
	if(parm[1][0] == NULL)		/* prompt for a file	*/
	{				/*----------------------*/
		cbdos(PRINT_STRING,&msg2[0]);
		get_cmd(&parm[1][0],(long)ARG_LEN-1);
	}
	if(parm[1][0] == NULL)
		return(0);
	if(parm[2][0] != NULL)
	{
		cbdos(PRINT_STRING,&msg13[0]);
		echo_cmd(&parm[2][0],BAD);
		return(0);
	}
	i = 0;
	while(parm[1][i] != ':' && parm[1][i] != NULL)
		i++;
	if(parm[1][i] == ':')
	{
		if(i != 1 || parm[1][0] < 'A' || parm[1][0] > 'P' ||
		   parm[1][2] == NULL)
		{
			echo_cmd(&parm[1][0],BAD);
			return(0);
		}
	}
	i = fill_fcb(1,cmdfcb);		/* fill an fcb	   */
	if(i > 0 && !(submit))		/* no confirmation */
	{				/* if submit file  */
		cbdos(PRINT_STRING,&msg9[0]);
		parm[2][0] = bdos(CONIN,(long)0);
		parm[2][0] = toupper(parm[2][0]);
		cr_lf();
		if(parm[2][0] != 'N' && parm[2][0] != 'Y')
			return(0);
	}
	if(parm[2][0] != 'N')
		if(XERR(i = fcall(DELETE_FILE)))
			xcmd = "ERASE";
		else if(i > 0)
		{
			cbdos(PRINT_STRING,&msg6[0]);
			ccp_seterr(TRUE);
		}
}

					/************************/
UWORD user_cmd()			/* change user number	*/
					/*----------------------*/
					/* update global user   */
					/************************/

{
	REG UWORD i;

	if(parm[1][0] == NULL)		/* prompt for a number	*/
	{
		cbdos(PRINT_STRING,&msg10[0]);
		get_cmd(&parm[1][0],(long)ARG_LEN-1);
	}
	if(parm[1][0] == NULL)
		return(TRUE);
	if(parm[2][0] != NULL)
	{
		cbdos(PRINT_STRING,&msg13[0]);
		echo_cmd(&parm[2][0],BAD);
		return(TRUE);
	}
	if((parm[1][0] < '0') ||
	   (parm[1][0] > '9'))
		return(FALSE);
	i = (parm[1][0] - '0');
	if(i > 9)
		return(FALSE);
	if(parm[1][1] != NULL)
		i = ((i * 10) + (parm[1][1] - '0'));
	if(i < 16 && parm[1][2] == NULL)
		bdos(GET_USER_NO,(long)i);
	else
		return(FALSE);
	user = i;
	return(TRUE);
}




					/************************/
VOID change_cmd()			/*    change default	*/
					/*    disk drive	*/
					/*----------------------*/
					/*  update global disk  */
					/************************/
{
	cur_disk = (parm[0][0] - 'A');
	bdos(SELECT_DISK,(long)cur_disk);
}

UWORD cmd_file(mode)
					/************************/
					/*			*/
					/*      SEARCH ORDER	*/
					/*	============	*/
					/*			*/
					/* 1. 68K type on the   */
					/*    current user #	*/
					/* 2. BLANK type on     */
					/*    current user #	*/
					/* 3. SUB type on the   */
					/*    current user #	*/
					/* 4. 68K type on	*/
					/*    user 0, SYS only	*/
					/* 5. BLANK type on the */
					/*    user 0, SYS only  */
					/* 6. SUB type on 	*/
					/*    user 0, SYS only	*/
					/*			*/
					/*----------------------*/
					/* 			*/
					/*   If a filetype is   */
					/*   specified then I   */
					/*   search the current */
					/*   user # then user 0 */
					/*			*/
					/*   Steps 4-6 name the */
					/*   file but do NOT	*/
					/*   open it: the BDOS	*/
					/*   does that, and the */
					/*   user number is not */
					/*   changed, so the	*/
					/*   program runs where */
					/*   the user is.	*/
					/*			*/
					/************************/
UWORD mode;
{
	BYTE gflag[PATHMAX];	/* per-group hit types (T_68K...)	*/
	BYTE gdrv[PATHMAX];	/* group drive (0 = A:)			*/
	BYTE gusr[PATHMAX];	/* group user number			*/
	BYTE gsys[PATHMAX];	/* group is the BDOS user-0 fallback:	*/
				/* scan user 0 for a SYS file, but open	*/
				/* under gusr[] and let function 15 find	*/
				/* it.					*/
	BYTE submitfile,skip,done;
	REG UWORD i,n,g;
	UWORD ng,sel,which,want,mask,type,gm,du;
	UWORD open,sub_open,ok;

	open = FALSE;
	sub_open = FALSE;
	dirflag = FALSE;
	ok = TRUE;
	load_try = TRUE; /* if a warmboot occurs reset drive & user # */
	submitfile = FALSE;
	user = bdos(GET_USER_NO,(long)255);
	cur_disk = bdos(RET_CUR_DISK,(long)0);
	which = (mode == SEARCH) ? 0 : 1;
	i = fill_fcb(which,cmdfcb);
	if(i > 0)
	{
		cbdos(PRINT_STRING,&msg7[0]);
		return(FALSE);
	}
	/*--------------------------------------------------------------*/
	/* Build the ordered list of (drive,user) groups to search.	*/
	/* A configured PATH replaces the list; an explicit drive on	*/
	/* the command (d: or a named directory) keeps the stock list	*/
	/* on that drive.  With no PATH the list is the current user	*/
	/* area plus the BDOS's own user-0 fallback (gsys), which is	*/
	/* where DRI puts it: v3's CCP restores the user number before	*/
	/* loading (ccp3.asm:1304-1305) exactly because		*/
	/* function 15 does the fallback (bdos30.asm:3940-3974).	*/
	/*--------------------------------------------------------------*/
	ng = 0;
	if(parm[which][1] != ':')
		for(g = 0;g < cfg_path() && ng < PATHMAX;g++)
		{
			du = cfg_pathdu(g);
			gdrv[ng] = ((du >> 8) == CURDU) ? cur_disk : (du >> 8);
			gusr[ng] = ((du & 0xFF) == CURDU) ? user : (du & 0xFF);
			gsys[ng] = FALSE;
			ng++;
		}
	if(ng == 0)
	{
		gdrv[0] = cmdfcb[0] - 1;
		gdrv[1] = cmdfcb[0] - 1;
		/* The mask is load-bearing: nd_argu is BYTE (signed char), so
		   a plain `!= CURDU' compares -1 against 255 always true. */
		gusr[0] = ((nd_argu[which] & 0xFF) != CURDU)
			  ? nd_argu[which] : user;
		gsys[0] = FALSE;
		ng = 1;
		if(gusr[0] != 0)
		{	/* the fallback group runs in gusr[0], not in 0	*/
			gusr[1] = gusr[0];
			gsys[1] = TRUE;
			ng = 2;
		}
	}
	sel = 0;
	if(cmdfcb[9] == ' ')
	{
		for(g = 0;g < ng;g++)
			gflag[g] = 0;
		want = (mode == SUB_FILE) ? T_SUB : T_68K;
		mask = (mode == SUB_FILE) ? T_SUB : T_ANY;
		/*------------------------------------------------------*/
		/* One directory scan per distinct drive, collecting the	*/
		/* hit types for every group on it.  Stop as soon as an	*/
		/* earlier group has a usable hit.			*/
		/*------------------------------------------------------*/
		for(g = 0;g < ng;g++)
		{
			skip = FALSE;
			done = FALSE;
			for(n = 0;n < g;n++)
			{
				if(gflag[n] & mask)
					done = TRUE;
				if(gdrv[n] == gdrv[g])
					skip = TRUE;
			}
			if(done)
				break;
			if(skip)
				continue;
			bdos(SELECT_DISK,(long)gdrv[g]);
			cmdfcb[0] = '?';
			cmdfcb[12] = NULL;
			i = cbdos(SEARCH_FIRST,cmdfcb);
			while(i != 255)
			{
				i *= 32;
				gm = 0;
				for(n = 0;n < ng;n++)
					if(gdrv[n] == gdrv[g] &&
					   (gsys[n] ? 0 : (gusr[n] & 0xFF))
						 == (dma[i] & 0xFF) &&
					   (!gsys[n] || (dma[i+10] & 0x80)))
						gm |= (1 << n);
					/* a fallback group sees user area 0,
					   and only its SYS files: that is the
					   rule function 15 will apply when it
					   opens (bdos30.asm:4006-4015), so
					   naming anything else here would
					   promise a file the open refuses */
				if(gm)
				{
					for(n = 9;n <= 11;n++)
						dma[i+n] &= CMASK;
					dma[i+12]  = NULL;
					type = 0;
					cmdfcb[9]  = '6';
					cmdfcb[10] = '8';
					cmdfcb[11] = 'K';
					if(strcmp(&cmdfcb[1],&dma[i+1]) == MATCH)
						type = T_68K;
					else
					{
					    cmdfcb[9]  = ' ';
					    cmdfcb[10] = ' ';
					    cmdfcb[11] = ' ';
					    if(strcmp(&cmdfcb[1],&dma[i+1]) == MATCH)
						type = T_BLANK;
					    else
					    {
					      cmdfcb[9]  = 'S';
					      cmdfcb[10] = 'U';
					      cmdfcb[11] = 'B';
					      if(strcmp(&cmdfcb[1],&dma[i+1]) == MATCH)
						type = T_SUB;
					    }
					}
					if(type)
						for(n = 0;n < ng;n++)
							if(gm & (1 << n))
								gflag[n] |= type;
				}
				if(gflag[0] & want)
					i = 255;
				else
					i = cbdos(SEARCH_NEXT,cmdfcb);
			}
		}
		fill_fcb(which,cmdfcb);
		bdos(SELECT_DISK,(long)cur_disk);
		sel = PATHMAX;
		for(g = 0;g < ng;g++)
			if(sel == PATHMAX && (gflag[g] & mask))
				sel = g;
		if(sel == PATHMAX)
			ok = FALSE;
		else
		{
			cmdfcb[0] = gdrv[sel] + 1;
			if(gflag[sel] & mask & T_68K)
			{
				cmdfcb[9]  = '6';
				cmdfcb[10] = '8';
				cmdfcb[11] = 'K';
			}
			else
				if(!(gflag[sel] & mask & T_BLANK))
				{
					cmdfcb[9]  = 'S';
					cmdfcb[10] = 'U';
					cmdfcb[11] = 'B';
				}
		}
	}
	if(cmdfcb[9] == 'S' && cmdfcb[10] == 'U' && cmdfcb[11] == 'B')
		submitfile = TRUE;
	/*--------------------------------------------------------------*/
	/* Open the file, walking the rest of the group list if it is	*/
	/* not there after all (the stock user-0 fallback, generalized).	*/
	/*--------------------------------------------------------------*/
	g = sel;
	while(!(open) && !(sub_open) && (ok) && g < ng)
	{
		bdos(GET_USER_NO,(long)gusr[g]);
		cmdfcb[0] = gdrv[g] + 1;
		if(cbdos(OPEN_FILE,cmdfcb) <= 3)
		{
			if(submitfile)
			{
				sub_open = TRUE;
				if(submit)
					chain_sub = TRUE;
				else
					first_sub = TRUE;
				for(i = 0;i < FCB_LEN;i++)
					subfcb[i] = cmdfcb[i];
				submit = TRUE;
				end_of_file = FALSE;
			}
			else
				open = TRUE;
		}
		g++;
	}
	if(open)
	{
		check_cmd(glb_index);
		__LOAD();
	}
	if(!(sub_open) && mode == SUB_FILE)
	{
		cbdos(PRINT_STRING,&msg11[0]);
		ccp_seterr(TRUE);
	}
	bdos(GET_USER_NO,(long)user);
	dirflag = TRUE;
	load_try = FALSE;
	return(sub_open);
}

					/************************/	
UWORD dollar(k,mode,com_index)		/*    Translate $n to   */ 
					/*  nth argument on the */
					/*  command line	*/
					/************************/
REG UWORD k;
REG UWORD mode;				
REG BYTE *com_index;			
{
	REG UWORD n,j,p_index;
	REG BYTE *p1;

	j = sub_index;
	if(k >= CMD_LEN)
	{
		k = 0;
		if(cbdos(READ_SEQ,subfcb) != 0)
		{
			end_of_file = TRUE;
			return(k);
		}
	}
	if((subdma[k] >= '0') && (subdma[k] <= '9'))
	{
		p_index = (subdma[k] - '0');
		p1 = com_index;
		if(*p1++ == 'S' &&
		   *p1++ == 'U' &&
		   *p1++ == 'B' &&
		   *p1++ == 'M' &&
	  	   *p1++ == 'I' &&
		   *p1++ == 'T' &&
		   *p1   == ' ')
			p_index++;
		p1 = com_index;
		for(n = 1; n <= p_index; n++)
		{
			while(*p1 != ' ' && *p1 != NULL)
				p1++;
			if(*p1 == ' ')
				p1++;
		}
		while(*p1 != ' ' && *p1 != NULL && j < CMD_LEN)
			if(mode == FILL)
				subcom[j++] = *p1++;
			else
				bdos(CONSOLE_OUTPUT,(long)*p1++);
		k++;
	}
	else
	{
		if(mode == FILL)
			subcom[j++] = '$';
		else
			bdos(CONSOLE_OUTPUT,(long)'$');
		if(subdma[k] == '$')
			k++;
	}
	sub_index = j;
	if(k >= CMD_LEN)
	{
		k = 0;
		if(cbdos(READ_SEQ,subfcb) != 0)
			end_of_file = TRUE;
	}
	return(k);
}
					/************************/
UWORD comments(k,com_index)		/* Strip and echo submit*/
					/* file comments	*/
					/************************/
REG UWORD k;
REG BYTE *com_index;
{
	REG UWORD done;

	done = FALSE;
	prompt();
	do
	{
		while(k < CMD_LEN     &&
		     subdma[k] != EOF && 
		     subdma[k] != Cr)
		{
			if(subdma[k] == '$')
			{
				k++;
				k = dollar(k,NOFILL,com_index);
			}
			else
				bdos(CONSOLE_OUTPUT,(long)subdma[k++]);
		}
		/*  The scan above is bounded by k < CMD_LEN, so the ONLY way
		    out with k == CMD_LEN is the bound itself -- neither EOF
		    nor Cr was seen, by construction.  Re-testing subdma[k]
		    here therefore proved nothing and read subdma[CMD_LEN],
		    one byte past sv_subdma[SV_CMDLEN] (ccpsv.h), which is
		    whatever follows the DMA buffer in the save area.  The
		    bound alone is the whole condition; dropping the two
		    reads also makes the else arm below safe, since it is now
		    reached only with k < CMD_LEN.  */
		if(k >= CMD_LEN)
		{
			k = 0;
			if(cbdos(READ_SEQ,subfcb) != 0)
			{
				end_of_file = TRUE;
				done = TRUE;
			}
		}
		else
		{
			if(subdma[k] == Cr)
			{
				k += 2;
				if(k >= CMD_LEN)
				{
					k = 0;
					if(cbdos(READ_SEQ,subfcb) != 0)
						end_of_file = TRUE;
				}
			}
			else
				end_of_file = TRUE;
			done = TRUE;
		}
	}while(!(done));
	return(k);
}

					/************************/
VOID translate(com_index)		/* TRANSLATE the subfile*/
					/* and fill sub buffer. */
					/************************/
REG BYTE *com_index;
{
	REG BYTE *p1;
	REG UWORD j,n,k,p_index;

	j = 0;
	k = sub_index;
	while(!(end_of_file) && j < CMD_LEN
		&& subdma[k] != Cr && subdma[k] != EXLIMPT)
	{
		switch(subdma[k])
		{
			case ';': k = comments(k,com_index);
				  break; 
			case TAB:
			case ' ': if(j > 0)
				  	subcom[j++] = subdma[k++];
	blankout:		  while(k < CMD_LEN && 
				       (subdma[k] == ' ' || subdma[k] == TAB))
						k++;
				  if(k >= CMD_LEN)
				  {
					k = 0;
					if(cbdos(READ_SEQ,subfcb) != 0)
						end_of_file = TRUE;
					else
						goto blankout;						
				  }
				  break;
			case '$': k++;
				  sub_index = j;
				  k = dollar(k,FILL,com_index);
				  j = sub_index;
				  break;
			case Lf:  k++;
				  if(k >= CMD_LEN)
				  {
					k = 0;
					if(cbdos(READ_SEQ,subfcb) != 0)
						end_of_file = TRUE;
			          }			
				  break;
			case EOF: 
				  end_of_file = TRUE;
				  break;
                         default: subcom[j++] = subdma[k++];
				  if(k >= CMD_LEN)
				  {
					k = 0;
					if(cbdos(READ_SEQ,subfcb) != 0)
						end_of_file = TRUE;
			          }
		}
	}
	/*------------------------------------------------------------------*/
	/*	       TRANSLATION OF A COMMAND IS COMPLETE		    */
	/*	       -Now move sub_index to next command-          	    */
        /*------------------------------------------------------------------*/

	if(subdma[k] == Cr || subdma[k] == EXLIMPT)
	do
	{
		while(k < CMD_LEN && 
		     (subdma[k] == Cr ||
		      subdma[k] == Lf ||
		      subdma[k] == EXLIMPT))
				k++;	
		if(k == CMD_LEN)
		{
			k = 0;
			if(cbdos(READ_SEQ,subfcb) != 0)
			{
				end_of_file = TRUE;
				break;
			}
		}
		else
		{
			if(subdma[k] == EOF)
				end_of_file = TRUE;
			break;
		}
	}while(TRUE);
	sub_index = k;
}

					/************************/
UWORD submit_cmd(com_index)		/* fill up the subcom   */
					/*      buffer 		*/
					/************************/
/*--------------------------------------------------------------*\
 |								|
 |	Submit_Cmd is a Procedure that returns exactly		|
 |	one command from the submit file.  Submit_Cmd is	|
 |	called only when the end of file marker has not 	|
 |	been read yet.  Upon leaving submit_cmd,the variable    |
 |	sub_index points to the beginning of the next command   |
 |	to translate and execute.  The buffer subdma is used    |
 |      to hold the UN-translated submit file contents.  	|
 |	The buffer subcom holds a translated command.		|
 |	Comments are echoed to the screen by the procedure      |
 |	"comments".  Parameters are substituted in comments     |
 |      as well as command lines.				|
 |								|
\*--------------------------------------------------------------*/

REG BYTE *com_index;
{
	REG UWORD i;

	for(i = 0;i <= CMD_LEN;i++)
		subcom[i] = NULL;
	cbdos(SET_DMA_ADDR,subdma);
	if(first_sub || chain_sub)
	{
		for(i = 0;i < CMD_LEN;i++)
			subdma[i] = NULL;
		if(cbdos(READ_SEQ,subfcb) != 0)
		{
			end_of_file = TRUE;
			subcom[0] = NULL;
		}
		sub_index = 0;
	}
	if(!(end_of_file))
		translate(com_index);
	for(i = 0;i < CMD_LEN;i++)
		subcom[i] = toupper(subcom[i]);
	cbdos(SET_DMA_ADDR,dma);
}

					/************************/	
VOID execute_cmd(cmd)			/*    branch to		*/
		 			/* appropriate routine	*/
					/************************/

REG BYTE *cmd;
{
	REG UWORD i,flag;
	UWORD code,nu,saveu;

	/*--------------------------------------------------------------*/
	/* IF/ELSE/FI, when configured: the flow-control words never	*/
	/* reach the command decoder, and lines inside a branch that	*/
	/* was not taken are dropped.					*/
	/*--------------------------------------------------------------*/
	if(flow_on())
	{
		if(flow_line())
			return(0);
		if(flow_skip())
			return(0);
	}
	ccp_seterr(FALSE);
	xcmd = NULL;
	nu = map_names();	/* NAME: -> d: ; nu = its user + 1	*/
	code = decode(cmd);
	saveu = 0;
	if(nu && code == CH_DISK)
	{			/* NAME: alone changes drive AND user	*/
		user = nu - 1;
		bdos(GET_USER_NO,(long)user);
	}
	else
		if(nu && code != FILE && code != SUBCMD)
		{		/* a builtin runs in the named user	*/
			saveu = bdos(GET_USER_NO,(long)255) + 1;
			bdos(GET_USER_NO,(long)(nu - 1));
		}
	switch( code )
	{
		case  DIRCMD:	dir_cmd(0);
				break;
		case DIRSCMD:	dir_cmd(1);
				break;
		case TYPECMD:	type_cmd();
				break;
		case  RENCMD:	ren_cmd();
				break;
		case  ERACMD:	era_cmd();
				break;
		case    UCMD:	if(!(user_cmd()))
					cbdos(PRINT_STRING,&msg12[0]);
				break;
		case CH_DISK:   change_cmd();
				break;
		case  SUBCMD:   flag = SUB_FILE;
				if(parm[1][0] == NULL)
				{
					cbdos(PRINT_STRING,&msg2[0]);
					get_cmd(subdma,(long)CMD_LEN-1);
					i = 0;
					while(subdma[i] != ' ' &&
					      subdma[i] != NULL) {
						parm[1][i] = subdma[i];
						i++;
					}
					parm[1][i] = NULL;
					subprompt = TRUE;
				}
				else
					subprompt = FALSE;
				/* The name to submit is parm[1], whether it
				   was typed on the command line or read at
				   the prompt above.  Only an empty prompt
				   response ends the command here: subdma is
				   the raw prompt buffer, and on a line that
				   did not prompt it still holds the previous
				   submit file's record -- or, on the first
				   command after a boot, nothing at all. */
				if(parm[1][0] == NULL)
					break;
				goto gosub;
		case    FILE:   flag = SEARCH;
gosub:				if(cmd_file(flag))
					break;
				if(flag == SUB_FILE)
					break;
		default	    :	ccp_seterr(TRUE);
				if(!(errhook()))
					echo_cmd(&parm[0][0],BAD);
	}
	if(saveu)
		bdos(GET_USER_NO,(long)(saveu - 1));
	if(xcmd != NULL)
		xfer_cmd();
}





				/********************************/
BYTE profcmd[] = "PROFILE.SUB";	/*  the command line a cold	*/
				/*  start runs before its first	*/
				/*  prompt			*/
				/********************************/

				/********************************/
UWORD profstart()		/*  is there a PROFILE.SUB to	*/
				/*  run, and has this session	*/
				/*  already looked?		*/
				/*------------------------------*/
				/* CP/M 3's CCP does this at	*/
				/* ccp3.asm:460-473		*/
				/* (`ckboot'): on a cold start	*/
				/* -- the `coldboot' bit of	*/
				/* ccpflag3 still clear -- it	*/
				/* sets the bit so the next CCP	*/
				/* will not, sets errflg to	*/
				/* ignore errors, and CHAINS to	*/
				/* the command line `PROFILE.S'.*/
				/* It does not open the file	*/
				/* itself: it hands the ordinary	*/
				/* command path a command line	*/
				/* and lets that path find the	*/
				/* submit file, which is why v3	*/
				/* needs the error suppression.	*/
				/*				*/
				/* This is the same shape.  The	*/
				/* `coldboot' bit is sv_profile	*/
				/* in the state page, which is	*/
				/* zeroed at cold boot and never	*/
				/* again (ccprun.c ccpsvinit),	*/
				/* so it is false exactly once	*/
				/* per session.  errflg is what	*/
				/* is done differently: rather	*/
				/* than running the command and	*/
				/* swallowing the complaint, we	*/
				/* ask the directory first and	*/
				/* run nothing when the file is	*/
				/* not there.  A system with no	*/
				/* PROFILE.SUB then pays ONE	*/
				/* directory search per boot and	*/
				/* prints nothing at all, which	*/
				/* is what it should look like;	*/
				/* v3's route would print		*/
				/* `PROFILE.S?' if errflg ever	*/
				/* failed to suppress it.	*/
				/********************************/
{
	BYTE		pfcb[FCB_LEN];
	REG UWORD	i;

	profile_done = TRUE;		/* asked; do not ask again	*/
	for(i = 0;i < FCB_LEN;i++)
		pfcb[i] = NULL;
	pfcb[0] = DISK_A;		/* v3's profile is on the system*/
					/*  drive; ours says so		*/
	for(i = 1;i <= 11;i++)
		pfcb[i] = ' ';
	for(i = 0;profcmd[i] != NULL && profcmd[i] != '.';i++)
		pfcb[1+i] = profcmd[i];
	pfcb[9]  = 'S';
	pfcb[10] = 'U';
	pfcb[11] = 'B';
	cbdos(SET_DMA_ADDR,dma);
	return((UWORD)(cbdos(SEARCH_FIRST,pfcb) <= 3));
}


main()
{					 /*---------------------*/
	REG BYTE *com_index;		 /* cmd execution ptr   */
	REG UWORD i;			 /*---------------------*/

/*  THIS CCP'S STATE, OUT OF RESIDENT STORAGE.  It is not in the TPA any
    more (ccpsv.h): the system holds one per process and hands it over on
    request, so it survives the transient programs this CCP loads without
    occupying any of their address space.  Read it before touching a
    single field -- every macro in ccpsv.h reads the buffer this fills.  */

	bdos(CCPSV_GET,(long)CCPSV);
	if(CCPSV->sv_magic != CCPSVMAGIC)
		ccpsvinit();

/*  The three pointer fields, if the state has just been built.  The system
    leaves them null because they point into THIS buffer and only the CCP
    knows where that is.  They are what ccp.c's `BYTE *user_ptr = usercmd'
    was when usercmd was a plain BSS array.  Non-null means an earlier
    instance set them, and they are still right: the buffer is at the same
    address in every instance.  */

	if(user_ptr == 0)
	{
		user_ptr = usercmd;
		glb_index = usercmd;
		tail = usercmd;
	}

	dirflag = TRUE;		         /* init fcb fill flag  */
	cbdos(SET_DMA_ADDR,dma);          /* set system dma addr */
	cfg_init();			 /* read CCP.CFG once   */
	in_errhook = FALSE;		 /*---------------------*/
	if(load_try)
	{
		bdos(SELECT_DISK,(long)cur_disk);
		bdos(GET_USER_NO,(long)user);
		load_try = FALSE;
	}
					 /*---------------------*/
	if(morecmds)			 /* if a warmboot 	*/
	{				 /* occured & there were*/
					 /* more cmds to do	*/
		if(submit)		 /*---------------------*/
		{
			com_index = subcom;
			submit_cmd(save_sub);
		}
		else
			com_index = user_ptr;
		morecmds = FALSE;
		echo_cmd(com_index,GOOD);
	}
	else
	if(!(profile_done) && profstart())
	{				/*----------------------*/
					/* COLD START, and there*/
					/* is a PROFILE.SUB on	*/
					/* A:.  Run it as a	*/
					/* command line, exactly*/
					/* as if it had been	*/
					/* typed: the parse loop*/
					/* below sees a name of	*/
					/* type SUB, cmd_file	*/
					/* opens it as a submit	*/
					/* file, and everything	*/
					/* after that is the	*/
					/* ordinary submit path.*/
					/* v3 chains to the	*/
					/* same command line	*/
					/* (ccp3.asm:470-473).	*/
					/*----------------------*/
		flow_clear();
		for(i = 0;profcmd[i] != NULL;i++)
			usercmd[i] = profcmd[i];
		usercmd[i] = NULL;
		com_index = usercmd;
		echo_cmd(com_index,GOOD);
	}
	else
	{				/*----------------------*/
	       flow_clear();		/* a fresh console line	*/
					/* closes any open IF	*/
               prompt();		/* prompt for command   */
	       com_index = usercmd;	/* set execution pointer*/
	       if(!(autost))		/* check autostart flag */	
	       get_cmd(usercmd,(long)CMD_LEN);/* read a command */
	       else			/* -------------------- */
	       {			/* -------------------- */
		echo_cmd(usercmd,GOOD);	/* echo auto cmd and	*/
		autost = FALSE;		/* turn off flag   	*/
	       }			/*----------------------*/
	}

/*--------------------------------------------------------------*\
 |								|
 |		       MAIN CCP PARSE LOOP			|
 |		       ===================			|
 |								|
\*--------------------------------------------------------------*/
			

	while(*com_index != NULL)
	{				  /*--------------------*/
		glb_index = com_index;	  /* save for use in    */
					  /* check_cmd call	*/
		get_parms(com_index);	  /* parse command line */
		if(parm[0][0] != NULL)	  /* ------------------	*/
		execute_cmd(&parm[0][0]); /* execute command    */
					  /*--------------------*/
		if(!(submit))
		com_index = scan_cmd(com_index);/* inc pointer  */
		else					  
		{			  /*--------------------*/
			if(first_sub) 	  /* save ptr to next	*/
					  /* console command    */
					  /*--------------------*/	
			{
				if(subprompt)
					copy_cmd(subdma);
				else				
					copy_cmd(com_index);
				com_index = subcom;
				user_ptr = scan_cmd(glb_index);
				submit_cmd(save_sub);
				first_sub = FALSE;
			}
			else
			if(chain_sub)
			{
				if(subprompt)
					copy_cmd(subdma);
				else
					copy_cmd(com_index);
				com_index = subcom;
				submit_cmd(save_sub);
				chain_sub = FALSE;
			}
			else		  /* ask the loop below	*/
				com_index = nullcmd;
					  /* for the next line	*/
			while(*com_index == NULL)
			{
				if(end_of_file)
				{
					com_index = user_ptr;
					submit = FALSE;
					break;
				}
				else
				{
					com_index = subcom;
					submit_cmd(save_sub);
				}
			}
		}
		if(*com_index != NULL)
			echo_cmd(com_index,GOOD);
	}

/*  Hand the state back.  ccpcrt.s calls main() once per command line in a
    loop, so this runs between lines as well as before a reload, and the
    resident copy is never more than one command line out of date.  The
    program-launch path does not return through here: it stores the state
    itself and warm boots (ccpgo.c __LOAD).  */

	bdos(CCPSV_PUT,(long)CCPSV);
}
