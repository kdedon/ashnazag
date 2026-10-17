/* Console messages. */
#ifndef _SYS_CMN_ERR_H
#define _SYS_CMN_ERR_H

#define CE_CONT		0
#define CE_NOTE		1
#define CE_WARN		2
#define CE_PANIC	3

#ifdef __STDC__
extern void cmn_err(int, char *, ...);
extern void printf(char *, ...);
extern void panic(char *, ...);
#else
extern void cmn_err();
extern void printf();
extern void panic();
#endif

#endif
