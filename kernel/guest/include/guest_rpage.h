/*
 * guest_rpage.h -- trapped register pages.
 *
 * A page in a guest I/O window is mapped absent.  Its access faults
 * reach the page's handler with the decoded access, which it applies
 * to a register model; the guest resumes after the instruction.
 * Read-modify-write forms (bset, andi.b, ...) arrive as one access
 * with ga_rmw set; a long spanning two registers arrives as one
 * 4-byte access.
 */

#ifndef _GUEST_RPAGE_H
#define _GUEST_RPAGE_H

#define	RP_EMULATE	1	/* register model */
#define	RP_ABSORB	2	/* writes kept for read-back, idle reads */
#define	RP_FORWARD	3	/* passthrough: to the real chip */

/* rp_phase: boot-phase rule for probe windows */
#define	RPP_ALWAYS	0
#define	RPP_BOOT	1	/* trapped until the OS is ready, then absent */

struct guest_access {
	unsigned long	ga_addr;
	unsigned long	ga_val;		/* written, or the result of a read */
	unsigned char	ga_size;	/* 1, 2, 4 */
	unsigned char	ga_write;
	unsigned char	ga_rmw;
	unsigned char	ga_movep;	/* alternate bytes */
};

struct guest_rpage {
	unsigned long	rp_base;
	unsigned long	rp_len;
	unsigned char	rp_kind;
	unsigned char	rp_phase;
	unsigned short	rp_flags;
	int		(*rp_access)();	/* (ctr, rpage, access): 0 done */
	char		*rp_state;	/* register model or file */
	unsigned long	rp_idle;	/* absorbed read value */
	unsigned long	rp_count;	/* accesses */
};

#endif	/* _GUEST_RPAGE_H */
