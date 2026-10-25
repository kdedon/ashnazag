/* Paging and scheduling counters: sum runs free, cnt and rate are intervals. */
#ifndef _SYS_VMMETER_H
#define _SYS_VMMETER_H

struct vmmeter {
	unsigned v_swtch;
	unsigned v_trap;
	unsigned v_syscall;
	unsigned v_intr;
	unsigned v_pdma;
	unsigned v_pswpin;
	unsigned v_pswpout;
	unsigned v_pgin;
	unsigned v_pgout;
	unsigned v_pgpgin;
	unsigned v_pgpgout;
	unsigned v_intrans;
	unsigned v_pgrec;
	unsigned v_xsfrec;
	unsigned v_xifrec;
	unsigned v_exfod;
	unsigned v_zfod;
	unsigned v_vrfod;
	unsigned v_nexfod;
	unsigned v_nzfod;
	unsigned v_nvrfod;
	unsigned v_pgfrec;
	unsigned v_faults;
	unsigned v_scan;
	unsigned v_rev;
	unsigned v_seqfree;
	unsigned v_dfree;
	unsigned v_fastpgrec;
	unsigned v_swpin;
	unsigned v_swpout;
};

extern struct vmmeter cnt, rate, sum;

#endif
