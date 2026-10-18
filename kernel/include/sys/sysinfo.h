/* System activity counters. */
#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H

struct sysinfo {
	time_t	cpu[5];
	time_t	wait[3];
	long	bread;
	long	bwrite;
	long	lread;
	long	lwrite;
	long	phread;
	long	phwrite;
	long	swapin;
	long	swapout;
	long	bswapin;
	long	bswapout;
	long	pswitch;
	long	syscall;
	long	sysread;
	long	syswrite;
	long	sysfork;
	long	sysexec;
	long	runque;
	long	runocc;
	long	swpque;
	long	swpocc;
	long	iget;
	long	namei;
	long	dirblk;
	ulong	readch;
	ulong	writech;
	long	rcvint;
	long	xmtint;
	long	mdmint;
	long	rawch;
	long	canch;
	long	outch;
	long	msg;
	long	sema;
	long	pnpfault;
	long	wrtfault;
	long	s5ipage;
	long	s5inopage;
	long	ufsipage;
	long	ufsinopage;
};

/* cpu[] */
#define CPU_IDLE	0
#define CPU_USER	1
#define CPU_KERNEL	2
#define CPU_WAIT	3
#define CPU_SXBRK	4

/* wait[] */
#define W_IO		0
#define W_SWAP		1
#define W_PIO		2

extern struct sysinfo sysinfo;

struct minfo {
	unsigned long	freemem[2];
	long	freeswap;
	long	vfault;
	long	demand;
	long	swap;
	long	cache;
	long	file;
	long	pfault;
	long	cw;
	long	steal;
	long	freedpgs;
	long	unmodsw;
	long	unmodfl;
};
extern struct minfo minfo;

struct syserr {
	long	inodeovf;
	long	fileovf;
	long	textovf;
	long	procovf;
};
extern struct syserr syserr;

#endif
