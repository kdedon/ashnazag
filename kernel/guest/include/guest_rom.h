/*
 * guest_rom.h -- ROM descriptors.
 *
 * A known ROM image is identified by the hash of the user's file.  Its
 * patch list holds offsets, the expected bytes and our replacement,
 * never ROM contents; patches apply to the private copy only and are
 * undone before a guest reset.
 */

#ifndef _GUEST_ROM_H
#define _GUEST_ROM_H

#define	GROM_HASHLEN	32	/* SHA-256 */

/* gpt_kind */
#define	GPT_LOAD	1	/* applied when the copy is made */
#define	GPT_HOT		2	/* trapped access rewritten at run time */

struct guest_patch {
	unsigned long	gpt_off;
	unsigned short	gpt_len;
	unsigned short	gpt_kind;
	unsigned char	*gpt_expect;
	unsigned char	*gpt_repl;
};

struct guest_romdesc {
	unsigned char	grd_hash[GROM_HASHLEN];
	char		grd_model[16];
	char		grd_version[16];
	unsigned long	grd_addr;	/* native load address */
	unsigned long	grd_size;
	unsigned long	grd_resetsp;	/* offsets of the reset vector pair */
	unsigned long	grd_resetpc;
	long		grd_sumoff;	/* checksum fix-up, -1 = none */
	int		grd_npatch;
	struct guest_patch *grd_patch;
};

#endif	/* _GUEST_ROM_H */
