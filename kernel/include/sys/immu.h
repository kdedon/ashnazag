/* 68k MMU: page and segment descriptors, address split, memory counts. */
#ifndef _SYS_IMMU_H
#define _SYS_IMMU_H

typedef union pte {
	struct {
		uint	pg_pfn	: 21,
			pg_tag	: 3,
				: 3,
			pg_mod	: 1,
			pg_ref	: 1,
			pg_w	: 1,
				: 1,
			pg_pres	: 1;
	} pgm;
	uint	pg_pte;
} pte_t;
#define pg_v		pg_pres

#define NPGPT		64		/* ptes per table */
#define NBPP		2048
#define NBPPT		256		/* bytes per page table */
#define BPTSHFT		8
#define NPTPP		8		/* page tables per page */
#define NPTPPSHFT	3
#define PNUMSHFT	11
#define PNUMMASK	0x3F
#define POFFMASK	0x7FF
#define PNDXMASK	0x7FFFF
#define PGFNMASK	0x1FFFFF
#define PG_ADDR		0xFFFFF800
#define PG_M		0x00000010
#define PG_REF		0x00000008
#define PG_W		0x00000004
#define PG_V		0x00000001

#define pgndx(x)	(((x) >> PNUMSHFT) & PNDXMASK)
#define mkpte(mode,pfn)	(mode | ((pfn) << PNUMSHFT))
#define PG_ISVALID(pte)	((pte)->pgm.pg_v)
#define PG_SETVALID(pte) ((pte)->pg_pte |= PG_V)
#define PG_CLRVALID(pte) ((pte)->pg_pte &= ~PG_V)
#define PG_SETMOD(pte)	((pte)->pg_pte |= PG_M)
#define PG_CLRMOD(pte)	((pte)->pg_pte &= ~PG_M)
#define PG_SETW(pte)	((pte)->pg_pte |= PG_W)
#define PG_CLRW(pte)	((pte)->pg_pte &= ~PG_W)
#define PG_SETREF(pte)	((pte)->pg_pte |= PG_REF)
#define PG_CLRREF(pte)	((pte)->pg_pte &= ~PG_REF)

typedef struct sde {
	uint	seg_len		: 16,
		seg_ring	: 6,
		seg_prot	: 8,
		seg_type	: 2;
	union {
		paddr_t		address;
		struct sde	*sdeptr;
	} wd2;
} sde_t;

/* seg_prot */
#define SG_bit		0x80
#define S_bit		0x40
#define G_bit		0x20
#define CI_bit		0x10
#define L_bit		0x08
#define M_bit		0x04
#define U_bit		0x02
#define WP_bit		0x01

/* seg_type */
#define SDE_invalid	0
#define SDE_contiguous	1
#define SDE_paged	2
#define SDE_toplevel	3

#define SEG_RW_UBLK	(S_bit)
#define SEG_RW_SYS	(SG_bit|S_bit)
#define SEG_RO_SYS	(SG_bit|S_bit|WP_bit)
#define SEG_RO		WP_bit
#define SEG_RW		0x00

#define SD_ISVALID(sde)	((sde)->seg_type != SDE_invalid)
#define SD_ISCONTIG(sde) ((sde)->seg_type == SDE_contiguous)

#define NPPS		64		/* pages per segment */
#define PPSSHFT		6
#define SOFFMASK	0x1FFFF
#define SEGOFF(x)	((uint)(x) & SOFFMASK)

/* virtual address: section (2 bits), segment (13), page (6), byte (11) */
#define SECNMASK	0x3
#define SECNSHFT	30
#define SEGNMASK	0x1FFF
#define SEGNSHFT	17
#define SECNUM(x)	(((uint)(x) >> SECNSHFT) & SECNMASK)
#define SEGNUM(x)	(((uint)(x) >> SEGNSHFT) & SEGNMASK)
#define PAGNUM(x)	(((uint)(x) >> PNUMSHFT) & PNUMMASK)
#define PAGOFF(x)	(((uint)(x)) & POFFMASK)
#define MSK_IDXSEG	0x1ffff

#define VSECT0		0x00000000
#define VSECT1		0x40000000
#define VSECT2		0x80000000
#define VSECT3		0xC0000000

#define SCN0		0
#define SCN1		1
#define SCN2		2
#define SCN3		3

#define flushaddr(vaddr)	flushmmu((vaddr), 1)

extern int	kpbase;
extern int	maxmem;
extern int	freemem;
extern int	availrmem;
extern int	availsmem;

#define svtop(x)	((uint)(x) >> PNUMSHFT)
#define ptosv(x)	((uint)(x) << PNUMSHFT)
#define phystokv(paddr)	(paddr)
#define phystopfn(paddr) ((u_int)(paddr) >> PNUMSHFT)
#define pfntophys(pfn)	((pfn) << PNUMSHFT)
#define kvtopfn(vaddr)	(kvtophys(vaddr) >> PNUMSHFT)
#define pfntokv(pfn)	((pfn) << PNUMSHFT)

extern paddr_t	svirtophys();
extern paddr_t	uvirtophys();
extern sde_t	*uvatosde();

/* kmem flags */
#define PHYSCONTIG	02
#define NOSLEEP		01

#define pflusha()	asm("pflusha")

/* switch the user (CRP, through the userroot descriptor) or kernel (SRP) root table */
static inline void
loadmmu(paddr_t urp)
{
	asm volatile("mov.l %0,userroot+4; mov.l &userroot,%%a1; pmove (%%a1),%%crp; pflusha"
		: : "a" (urp) : "%a1");
}

static inline void
loadkrp(void *krp)
{
	asm volatile("pmove (%0),%%srp; pflusha" : : "a" (krp));
}

#ifdef _KERNEL
#ifdef __STDC__
extern sde_t *vatosde(caddr_t);
extern void flushmmu(caddr_t, int);
#else
extern sde_t *vatosde();
extern void flushmmu();
#endif
#endif

#endif
