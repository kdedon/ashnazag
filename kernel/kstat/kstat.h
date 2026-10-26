/*
 * /dev/kstat: system statistics as named records.  Every read(2) returns
 * one consistent snapshot from its start, a header and then records:
 *
 *	struct kstat_hdr | kstat_named_t[ks_ndata]
 *
 * A short buffer gets the first records.  Values are unsigned longs:
 * counters wrap, avenrun_* are fixed point (fscale is 1.00).
 */
#ifndef _KSTAT_H
#define _KSTAT_H

#define KSTAT_MAGIC	0x6b730001	/* "ks", version 1 */
#define KSTAT_STRLEN	20
#define KSTAT_MAJOR	69

typedef struct kstat_named {
	char		name[KSTAT_STRLEN];
	unsigned long	value;
} kstat_named_t;

struct kstat_hdr {
	unsigned long	ks_magic;
	unsigned long	ks_ndata;	/* records that follow */
	unsigned long	ks_snaptime;	/* ticks since boot */
	unsigned long	ks_hz;		/* ticks per second */
};

/* libkstat */
typedef struct kstat_ctl {
	int		kc_fd;
	struct kstat_hdr kc_hdr;
	kstat_named_t	*kc_data;
	int		kc_max;
} kstat_ctl_t;

#ifndef _KERNEL
extern kstat_ctl_t *kstat_open();
extern int kstat_read();		/* (kc) new snapshot: 0, or -1 */
extern kstat_named_t *kstat_data_lookup();	/* (kc, name) or 0 */
extern unsigned long kstat_value();	/* (kc, name) 0 when absent */
extern int kstat_close();
#endif

#endif
