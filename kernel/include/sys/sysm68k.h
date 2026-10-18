/* sysm68k() commands and m68k exception frame details. */
#ifndef _SYS_SYSM68K_H
#define _SYS_SYSM68K_H

#define _m68k_SETNAME	56
#define _m68k_BIURW	92	/* 68881 idle frame BIU flags */
#define _m68k_EXOP	93	/* 68881 idle frame exception operand */
#define _m68k_AB_STKFRM	94	/* 68020/030 format A/B frame fields */
#define _m68k_LIMUSER	95
#define _m68k_KERNADDR	99

/* format A/B frame offsets */
#define SSW		0xa
#define DOB		0x18
#define DIB		0x2c
#define M68KFMT		1
#define EXC_PEND	0x8000000	/* BIU flags: FP exception pending */

#ifdef _KERNEL
/* SIGILL..SIGSEGV and SIGKILL: handled before the process continues */
#define SNODEFER	0x7f8
#define SDEFER		~SNODEFER
/* user stack frame flags */
#define USTKCLEAR	0x80000000
#define USTKRESTORE	0x40000000
extern char	framesz[];
#endif

#endif
