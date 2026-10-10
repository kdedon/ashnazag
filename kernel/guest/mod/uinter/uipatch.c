/*
 * Mac OS 8.1 uses live redirects to retain the Unix File Manager,
 * restore startup callbacks and adapt filesystem and heap conventions.
 * Version and code signatures constrain each redirect to its caller.
 */

#include "uinter.h"
#include "sys/mman.h"
#include "sys/resource.h"
#include "vm/as.h"

int	uinter_patches = 1;		/* 0: none */
int	uinter_fmgroup = 0;		/* 0: 8.1's File Manager groups install */
long	uinter_patched;
long	uinter_nalwatch;

extern void dlm_cacheflush();

#define	PSKIP	0		/* continue at p_to */
#define	PCALL	1		/* _AUXDispatch(p_to >> 16, p_to & 0xffff) first */
#define	PSLEEPQ	2		/* an empty SleepQHdr first */
#define	PSOUND	3		/* early calls while _SoundDispatch is unimplemented */
#define	PGROUP	4		/* leave the lpch group out (see lpgroup) */
#define	PSPEC	5		/* FSMakeFSSpec through PBGetCatInfo (see mkspec) */
#define	PFIX	6		/* tmhead over the bytes, p_at before the trap */
#define	PRZONE	7		/* _RsrcZoneInit first, at every _SetApplBase */
#define	PHFS	8		/* hfstramp as _HFSDispatch first */
#define	PHFSINIT 10		/* advance past external and converted VCBs */
#define	PFSM	9		/* fsmFFSNotFoundErr for an unimplemented _FSMDispatch */
#define	MSPEC	2		/* catspec's */
#define	MOPEN	3		/* hopen's */
#define	MPARMS	4		/* volparms' */
#define	MEXTFS	5		/* extcall's */
#define	MHFS	6		/* hfsalloc's */
#define	MCAT	7		/* trashcat's: the lookup */
#define	MFOLD	8		/* its FindFolder */
#define	MTRASH	9		/* the Trash's own PBGetCatInfo */
#define	MTPAR	10		/* trashid's */
extern long uin_forcewne;
long uin_forcealine, uin_forceop, uin_forcepc;

#define MCLASS 15
#define MREGEXIT 16
#define MLOAD 17
#define MLOADEXIT 18
#define MGET 19
#define MMPSLOAD 20
#define MGETEXIT 22
#define MGINSTALL 24
#define MPMIDLE 25
#define MASLM 14
int uinter_aslmtrace = 0;
long uin_aslmreads,uin_aslmcode;

#define	MPATH	13
#define	MFORCE	12		/* foreground forced quit */
#define	MXVOL	11		/* xvolinfo's */
#define	RESUME	0xa89f		/* _Unimplemented, should it run unwatched */
#define	FMAGIC	0x75697063	/* 'uipc' */
#define	FSIZE	96
#define	GPF_DONE(i)	(0x10000UL << (i))

static unsigned char hfsplus[] = {	/* 'ptch' -20217 +0, the trap at +$12 */
	0x4e, 0xfa, 0x00, 0x0a, 0x70, 0x74, 0x63, 0x68, 0xb1, 0x07, 0x00, 0x02,
	0x20, 0x3c, 0x66, 0x73, 0x20, 0x20, 0xa1, 0xad, 0x66, 0x28, 0x20, 0x08,
	0x02, 0x80, 0x00, 0x00, 0x02, 0x00, 0x67, 0x1e, 0x43, 0xfa, 0x00, 0x1c,
	0x41, 0xfa, 0x00, 0x14, 0x93, 0xc8, 0x20, 0x09, 0x43, 0xf8, 0x03, 0xd6,
	0xa0, 0x2e, 0xa0, 0xbd, 0x20, 0x41, 0xa0, 0x2a, 0x4e, 0xd1, 0xa0, 0x23,
	0x4e, 0x75
};

#define HFSWALK 0x1e6
static unsigned char hfswalk[] = {
	0x20, 0x38, 0x03, 0x58, 0x60, 0x00, 0x00, 0x1a, 0x24, 0x40,
	0x4a, 0x6a, 0x00, 0x4c, 0x66, 0x10, 0x0c, 0x6a, 0xd2, 0xd7,
	0x00, 0x08, 0x67, 0x08, 0x4e, 0xba, 0xff, 0x1c, 0x66, 0xac,
	0x20, 0x12, 0x66, 0xe6
};

/* Native disk initialization requires a native VCB and disk cache. */
static int
hfsvolumes()
{
	unsigned char b[80];
	long v;
	int n, native = 0;

	if (copyin((caddr_t)0x358, (caddr_t)b, 4) || !(v = G32(b)))
		return -1;
	for (n = 0; v && n < 256; n++) {
		if ((v & 1) || copyin((caddr_t)v, (caddr_t)b, sizeof b))
			return -1;
		if (!G16(b + 0x4c))
			native = 1;
		v = G32(b);
	}
	return v ? -1 : !native;
}

static unsigned char boot3a[] = {	/* 'boot' 3 +$A7E, _Gestalt 'mach' at +$A9A */
	0x08, 0xb8, 0x00, 0x04, 0x0b, 0x21, 0x2b, 0x7c, 0x00, 0x06, 0x40, 0x00,
	0x00, 0x16, 0x2b, 0x7c, 0x00, 0x0c, 0x80, 0x00, 0x00, 0x1a, 0x20, 0x3c,
	0x6d, 0x61, 0x63, 0x68, 0xa1, 0xad
};

static unsigned char boot3b[] = {	/* 'boot' 3 +$B14, _ScriptUtil at +$B34 */
	0x08, 0x38, 0x00, 0x05, 0x0b, 0x20, 0x67, 0x10, 0x24, 0x7c, 0x73, 0x63,
	0x72, 0x69, 0x2e, 0x3c, 0x65, 0x78, 0x74, 0x6e, 0x61, 0x00, 0x03, 0x78,
	0x55, 0x4f, 0x2f, 0x3c, 0x82, 0x00, 0xff, 0xca, 0xa8, 0xb5
};

static unsigned char adbc0[] = {	/* 'boot' 3 +$5D06, _CountADBs at +$5D0E */
	0x4e, 0x56, 0xff, 0xf6, 0x48, 0xe7, 0x03, 0x00, 0xa0, 0x77, 0x3e, 0x00,
	0x43, 0xee, 0xff, 0xf6, 0x20, 0x49, 0x30, 0x07, 0xa0, 0x78, 0x48, 0x80,
	0x3c, 0x00, 0x70, 0x00, 0x10, 0x2e, 0xff, 0xf6, 0x0c, 0x40, 0x00, 0xc0,
	0x66, 0x04, 0x70, 0x01, 0x60, 0x08, 0x53, 0x47, 0x4a, 0x47, 0x66, 0xdc
};

static unsigned char lodrp2[] = {	/* 'lodr' -16385 +$460, pass 1's walk; the trap at +$476 or +$47A */
	0x24, 0x6f, 0x00, 0x06, 0x43, 0xfa, 0x01, 0x06, 0x61, 0x00, 0x00, 0x8c,
	0x20, 0x2d, 0xff, 0xfc, 0x20, 0x46, 0x90, 0x88, 0x67, 0x04, 0xa0, 0x20,
	0x60, 0x02, 0xa0, 0x1f
};
#define	LODRTAB	6		/* the table's address, on the stack at the trap */
#define	LODRSCAN 0x160		/* table bytes walked to find fmgroups */

static unsigned char fmgroups[] = {	/* lpch 63 +$F1..+$15A: the File Manager's groups */
	0xfe, 0x00, 0x00, 0x1f, 0x04, 0x00, 0x00, 0x6c, 0xa2, 0x60, 0x08, 0xa0,
	0x41, 0x01, 0xa0, 0x42, 0x04, 0xa0, 0x01, 0xfe, 0x00, 0x00, 0x07, 0x01,
	0xc0, 0x31, 0xfe, 0x00, 0x00, 0x1f, 0x01, 0xc9, 0xc9, 0x01, 0xc8, 0x73,
	0x01, 0xa0, 0x0b, 0x02, 0x07, 0x70, 0x04, 0xa0, 0x0a, 0xfe, 0x00, 0x00,
	0x1c, 0x04, 0x07, 0xbc, 0xfe, 0x00, 0x00, 0x1f, 0x01, 0xa0, 0x0f, 0x01,
	0xa0, 0x35, 0x01, 0xa0, 0x17, 0x03, 0xa0, 0x0e, 0x01, 0x07, 0x38, 0x01,
	0x03, 0xea, 0x14, 0x07, 0x20, 0x01, 0xc0, 0xdb, 0x0c, 0x00, 0x00, 0xfe,
	0x00, 0x00, 0x01, 0x01, 0x07, 0x34, 0x01, 0x07, 0x48, 0x01, 0x07, 0x40,
	0x01, 0x07, 0x3c, 0xfe, 0x00, 0x00, 0x1c, 0x01, 0x00, 0x00
};
static unsigned char tmwalk[] = {	/* 'scod' -16465 +$C11C, 'scod' -16463 +$BB40 */
	0x59, 0x8f, 0x2e, 0xb8, 0x0b, 0x30, 0x20, 0x5f, 0x28, 0x50, 0x60, 0x20
};

static unsigned char tmhead[] = {	/* movea.l TimeVars,a0; movea.l 2(a0),a4; nop */
	0x20, 0x78, 0x0b, 0x30, 0x28, 0x68, 0x00, 0x02, 0x4e, 0x71
};
#define	NOTAUX	0x01		/* condition bit 8 (not under A/UX), in its middle byte */

static struct uipatch {
	unsigned short	p_sysver;	/* SysVersion it is for */
	unsigned short	p_trap;		/* the A-line word */
	unsigned char	*p_code;	/* original bytes */
	int		p_len;
	int		p_at;		/* the trap's offset in them */
	int		p_kind;
	long		p_to;		/* PSKIP: where the task continues */
} uipatches[] = {
	{ 0x0810, 0xa1ad, hfsplus, sizeof hfsplus, 0x12, PHFSINIT, 0 },
	{ 0x0810, 0xa1ad, boot3a, sizeof boot3a, 0x1c, PCALL, (36L << 16) | 1 },
	{ 0x0810, 0xa8b5, boot3b, sizeof boot3b, 0x20, PCALL, 36L << 16 },
	{ 0x0810, 0xa800, 0, 0, 0, PSLEEPQ, 0 },
	{ 0x0810, 0xa077, adbc0, sizeof adbc0, 8, PSKIP, 0x30 },
	{ 0x0810, 0xa800, 0, 0, 0, PSOUND, 0 },
	{ 0x0810, 0xa020, lodrp2, sizeof lodrp2, 0x16, PGROUP, 0 },
	{ 0x0810, 0xa01f, lodrp2, sizeof lodrp2, 0x1a, PGROUP, 0 },
	{ 0x0810, 0xa060, 0, 0, 0, PSPEC, 0 },	/* sync, either HFS bit */
	{ 0x0810, 0xa829, tmwalk, sizeof tmwalk, 0x372, PFIX, 0 },
	{ 0x0810, 0xa829, tmwalk, sizeof tmwalk, 0x366, PFIX, 0 },
	{ 0x0810, 0xa057, 0, 0, 0, PRZONE, 0 },
	{ 0x0810, 0xa060, 0, 0, 0, PHFS, 0 },
	{ 0x0810, 0xa0ac, 0, 0, 0, PFSM, 0 },
};

#define	TOOLTAB	0xe00		/* toolbox trap table */

static unsigned short rzinit[] = {	/* f+4, up to RESUME */
	0xa996, 0x4e71, 0x4e71, 0x4e71, 0x4e71, 0x4e71
};

static unsigned short sleepq[] = {	/* f+4, up to RESUME */
	0x700a, 0xa71e, 0x21c8, 0x0bbc, 0x4e71, 0x4e71
};
#define	NPATCH	(sizeof uipatches / sizeof uipatches[0])
typedef char uipatch_flaglimit[NPATCH <= 15 ? 1 : -1];

/*
 * _HFSDispatch's OS trap table entry: HFSTRAP, taken here, gives the call
 * the A-line's treatment; then rts, or jmp to the previous entry.
 * CFM-68K glue calls the entry directly, without an A-line.
 */
#define	HFSTRAP	0xaffc
#define	HFSENT	(0x400 + 4 * 0x60)
static unsigned short hfstramp[] = { HFSTRAP, 0x4e75, 0x4ef9, 0, 0 };
#define WNEENT 0xf80
#define WNEOFF 12
#define WNELINK 0x86
#define WNEWORD 0x110
#define WNELEAVE 0x102
static unsigned char wneentry[] = {
	0x48, 0xe7, 0xff, 0xfe, 0x61, 0x00, 0x01, 0xf8, 0x4e, 0x71, 0x4e, 0x71,
	0x4a, 0x90, 0x67, 0x00, 0x00, 0x70, 0x0c, 0x90, 0x00, 0x00, 0x00, 0x03,
	0x6c, 0x00, 0x00, 0x66, 0x2a, 0x78, 0x09, 0x04, 0x42, 0xa7, 0xa9, 0x75,
	0x20, 0x1f, 0x41, 0xfb, 0x01, 0x70, 0x00, 0x00, 0x00, 0xe8, 0x0c, 0x90,
	0x00, 0x00, 0x00, 0x01, 0x66, 0x00, 0x00, 0x14, 0x22, 0x00, 0x06, 0x81,
	0x00, 0x00, 0x01, 0x2c, 0x21, 0x41, 0x00, 0x04, 0x20, 0xbc, 0x00, 0x00,
	0x00, 0x02, 0x90, 0xa8, 0x00, 0x04, 0x6a, 0x00, 0x00, 0x0c, 0x61, 0x00,
	0x00, 0x36, 0x4a, 0x80, 0x67, 0x00, 0x00, 0x26, 0x61, 0x00, 0x00, 0x90,
	0x64, 0x00, 0x00, 0x14, 0x41, 0xfb, 0x01, 0x70, 0x00, 0x00, 0x00, 0xaa,
	0x20, 0xbc, 0x00, 0x00, 0x00, 0x04, 0x60, 0x00, 0x00, 0x0c, 0x41, 0xfb,
	0x01, 0x70, 0x00, 0x00, 0x00, 0x98, 0x42, 0x90, 0x4c, 0xdf, 0x7f, 0xff,
	0x4e, 0xf9, 0x00, 0x00, 0x00, 0x00, 0x4e, 0x56, 0xff, 0xf0, 0x42, 0x67,
	0x48, 0x6e, 0xff, 0xf8, 0x70, 0xff, 0x2f, 0x00, 0x3f, 0x3c, 0x00, 0x39,
	0xa8, 0x8f, 0x30, 0x1f, 0x66, 0x00, 0x00, 0x46, 0x42, 0x67, 0x48, 0x6e,
	0xff, 0xf0, 0x3f, 0x3c, 0x00, 0x37, 0xa8, 0x8f, 0x30, 0x1f, 0x66, 0x00,
	0x00, 0x34, 0x20, 0x2e, 0xff, 0xf8, 0xb0, 0xae, 0xff, 0xf0, 0x66, 0x00,
	0x00, 0x28, 0x20, 0x2e, 0xff, 0xfc, 0xb0, 0xae, 0xff, 0xf4, 0x66, 0x00,
	0x00, 0x1c, 0x41, 0xfb, 0x01, 0x70, 0x00, 0x00, 0x00, 0x40, 0x20, 0xbc,
	0x00, 0x00, 0x00, 0x03, 0x30, 0x3c, 0x4e, 0x22, 0xa9, 0xc9, 0x70, 0x01,
	0x60, 0x00, 0x00, 0x04, 0x70, 0x00, 0x4e, 0x5e, 0x4e, 0x75, 0x48, 0xe7,
	0xff, 0xfe, 0x70, 0x01, 0x2f, 0x00, 0x2f, 0x3c, 0x00, 0x00, 0x00, 0x6e,
	0x42, 0xa7, 0x70, 0x26, 0x4e, 0x40, 0x4e, 0x71, 0x4f, 0xef, 0x00, 0x0c,
	0x4c, 0xdf, 0x7f, 0xff, 0x4e, 0x75, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};
#define POLLUP 0x16
#define POLLTRACK 0x30
#define POLLTRAP 0x44
#define POLLHIT 0x72
#define POLLSTATEPC 0x6a
#define POLLDIAG 0xd4
#define HITPOINTPC 0xb4
#define HITRESULTPC 0xcc
#define ROMHIT 0x2a6e
#define ROMDOWN 0x2a38
#define ROMUP 0x2a5c
#define ROMTRACK 0x2a52
static unsigned char pollentry[] = {
	0x48, 0xe7, 0xff, 0xfe, 0x61, 0x00, 0x00, 0x30, 0x4c, 0xdf, 0x7f, 0xff,
	0x4a, 0x38, 0x01, 0x72, 0x66, 0x00, 0xff, 0xee, 0x4e, 0x75, 0x48, 0xe7,
	0xff, 0xfe, 0x61, 0x00, 0x00, 0x1a, 0x4c, 0xdf, 0x7f, 0xff, 0x4a, 0x38,
	0x01, 0x72, 0x66, 0x00, 0x00, 0x0c, 0x4f, 0xef, 0x00, 0x04, 0x4e, 0xf9,
	0x00, 0x00, 0x00, 0x00, 0x4e, 0x75, 0x42, 0xa7, 0x2f, 0x3c, 0x00, 0x00,
	0x00, 0x6e, 0x42, 0xa7, 0x70, 0x26, 0x4e, 0x40, 0x4e, 0x71, 0x4f, 0xef,
	0x00, 0x0c, 0x4a, 0xba, 0x00, 0x88, 0x67, 0x00, 0x00, 0x20, 0x70, 0x00,
	0x10, 0x38, 0x01, 0x72, 0x2f, 0x00, 0x48, 0x78, 0x00, 0x04, 0x2f, 0x3c,
	0x00, 0x00, 0x00, 0x6e, 0x42, 0xa7, 0x70, 0x26, 0x4e, 0x40, 0x4e, 0x71,
	0x4f, 0xef, 0x00, 0x10, 0x4e, 0x75, 0x55, 0x4f, 0x2f, 0x38, 0x08, 0x30,
	0x48, 0x57, 0xa8, 0x71, 0x48, 0xe7, 0xff, 0xfe, 0x42, 0xe7, 0x61, 0x00,
	0x00, 0x20, 0x44, 0xdf, 0x4c, 0xdf, 0x7f, 0xff, 0x48, 0x53, 0xa8, 0xad,
	0x10, 0x1f, 0x48, 0xe7, 0xff, 0xfe, 0x42, 0xe7, 0x61, 0x00, 0x00, 0x22,
	0x44, 0xdf, 0x4c, 0xdf, 0x7f, 0xff, 0x4e, 0x75, 0x48, 0x78, 0x00, 0x02,
	0x2f, 0x3c, 0x00, 0x00, 0x00, 0x6e, 0x42, 0xa7, 0x70, 0x26, 0x4e, 0x40,
	0x4e, 0x71, 0x4f, 0xef, 0x00, 0x0c, 0x4e, 0x75, 0x48, 0x78, 0x00, 0x03,
	0x2f, 0x3c, 0x00, 0x00, 0x00, 0x6e, 0x42, 0xa7, 0x70, 0x26, 0x4e, 0x40,
	0x4e, 0x71, 0x4f, 0xef, 0x00, 0x0c, 0x4e, 0x75, 0x00, 0x00, 0x00, 0x00,
};
static unsigned char polldown[] = { 0x4a,0x38,0x01,0x72,0x66,0xfa };
static unsigned char pollup[] = { 0x4a,0x38,0x01,0x72,0x67,0xf0 };
static unsigned char pollmiddle[] = {
	0x26, 0x4a, 0x36, 0x1b, 0x54, 0x4b, 0x61, 0x28, 0x66, 0x10, 0x47, 0xeb,
	0x00, 0x0c, 0x53, 0x43, 0x66, 0xf4, 0x60, 0xe6, 0x61, 0x1a, 0xb6, 0x00,
	0x67, 0x04, 0x16, 0x00, 0x61, 0x24,
};
static unsigned char hitnative[] = {
    0x55,0x4f,0x2f,0x38,0x08,0x30,0x48,0x57,0xa8,0x71,0x2f,0x0b,0xa8,0xad,0x10,0x1f,0x4e,0x75
};
static unsigned char frontentry[] = {
	0x42,0xe7,0x4a,0xbb,0x01,0x70,0x00,0x00,0x00,0x98,0x67,0x00,
	0x00,0x80,0x4e,0x56,0xfe,0xe8,0x2a,0x78,0x09,0x04,0x2d,0x4d,
	0xfe,0xfc,0x42,0x67,0x48,0x6e,0xfe,0xe8,0x3f,0x3c,0x00,0x37,
	0xa8,0x8f,0x30,0x1f,0x66,0x00,0x00,0x60,0x42,0x67,0x48,0x6e,
	0xfe,0xf0,0x70,0xff,0x2f,0x00,0x3f,0x3c,0x00,0x39,0xa8,0x8f,
	0x30,0x1f,0x66,0x00,0x00,0x4a,0x20,0x2e,0xfe,0xe8,0xb0,0xae,
	0xfe,0xf0,0x66,0x00,0x00,0x3e,0x20,0x2e,0xfe,0xec,0xb0,0xae,
	0xfe,0xf4,0x66,0x00,0x00,0x32,0x42,0xa7,0xa9,0x24,0x2d,0x5f,
	0xfe,0xf8,0x4a,0xae,0xfe,0xf8,0x67,0x00,0x00,0x22,0x2f,0x2e,
	0xfe,0xf8,0x48,0x6e,0xff,0x00,0xa9,0x19,0x48,0x6e,0xfe,0xe8,
	0x48,0x78,0x00,0x05,0x48,0x78,0x00,0x6e,0x42,0xa7,0x70,0x26,
	0x4e,0x40,0x4f,0xef,0x00,0x10,0x4e,0x5e,0x44,0xdf,0x41,0xfb,
	0x01,0x70,0xff,0xff,0xfe,0x82,0x4e,0x75,0x46,0x57,0x4e,0x31,
	0x00,0x00,0x00,0x00,
};
#define FRONTEXT (sizeof wneentry+14+sizeof pollentry)
#define FRONTTRAP 0x86
#define FRONTMAGIC 0x98
#define FRONTFLAG 0x9c
int uin_frontdiag;
long uin_frontflag,uin_frontpid,uin_frontcount,uin_frontrefused,uin_frontserial;
long uin_frontlast[5];
unsigned char uin_fronttitle[256];

void
ui_frontclear()
{
    uin_frontflag=uin_frontpid=uin_frontcount=uin_frontrefused=uin_frontserial=0;
    bzero((caddr_t)uin_frontlast,sizeof uin_frontlast);
    bzero((caddr_t)uin_fronttitle,sizeof uin_fronttitle);
}

int uin_polldiag;
static int forcepollinstall(), forcepollremove();
static unsigned char wnenative[] = {
	0x4e, 0x56, 0xff, 0xfc, 0x48, 0xe7, 0x1f, 0x08, 0x2a, 0x2e, 0x00, 0x0c,
	0x28, 0x6e, 0x00, 0x10, 0x38, 0x2e, 0x00, 0x14, 0x20, 0x0d, 0x22, 0x15,
	0x2a, 0x78, 0x0b, 0x7c, 0x2a, 0x81, 0x26, 0x00, 0x20, 0x6d, 0xfb, 0xae,
	0x52, 0x68, 0x00, 0x42, 0x20, 0x2d, 0xfb, 0xae, 0xb0, 0xad, 0xfb, 0xaa,
	0x66, 0x0c, 0x2b, 0x6e, 0x00, 0x08, 0xfe, 0xca, 0x70, 0xff, 0x2b, 0x40,
	0xfe, 0xc6, 0x2e, 0x05,
};

long uin_forceentry, uin_forceword;

/* A checked native entry also catches cached procedure pointers. */
static int
ui_wneentry(a,force)
	long a;
    int force;
{
	unsigned char b[4], code[sizeof wneentry + 14], old[sizeof wneentry];
	unsigned char native[sizeof wnenative], redirect[8], front[sizeof frontentry];
	long target, previous;
	int i, installed = 0;

	uin_forceentry = uin_forceword = 0;
	if (copyin((caddr_t)0x15a, (caddr_t)b, 2) || G16(b) != 0x0810 ||
	    copyin((caddr_t)WNEENT, (caddr_t)b, 4) || !(target = G32(b)) || (target & 1) ||
	    copyin((caddr_t)target, (caddr_t)native, sizeof native)) return 0;
	if (G16(native) == 0x4ef9 && G16(native + 6) == 0x4e71) {
		a = G32(native + 2);
		if (!a || (a & 1) || copyin((caddr_t)a, (caddr_t)old, sizeof old)) return 0;
		for (i = 0; i < WNEWORD; i++)
			if ((i < WNELINK || i >= WNELINK + 4) && old[i] != wneentry[i]) return 0;
		previous = G32(old + WNELINK);
		if (previous != a + sizeof wneentry ||
		    copyin((caddr_t)previous, (caddr_t)code, 14)) return 0;
		for (i = 0; i < 8; i++) if (code[i] != wnenative[i]) return 0;
		if (G16(code + 8) != 0x4ef9 || G32(code + 10) != target + 8) return 0;
		installed = 1;
	} else
		for (i = 0; i < 8; i++) if (native[i] != wnenative[i]) return 0;
	for (i = 8; i < sizeof native; i++)
		if (native[i] != wnenative[i]) return 0;
	if (!a || (a & 1) || as_checkprot(u.u_procp->p_as, (caddr_t)target, sizeof native,
	    PROT_READ | PROT_WRITE | PROT_EXEC) ||
	    as_checkprot(u.u_procp->p_as, (caddr_t)a, FRONTEXT+sizeof frontentry,
	    PROT_READ | PROT_WRITE | PROT_EXEC)) return 0;
	uin_forceentry = a;
	uin_forceword = installed ? G32(old + WNEWORD) : 0;
	if(installed) {
        if(copyin((caddr_t)(a+FRONTEXT),(caddr_t)front,sizeof front)) return 0;
        for(i=0;i<sizeof front;i++)
            if((i<FRONTFLAG || i>=FRONTFLAG+4) && front[i]!=frontentry[i]) return 0;
        if((unsigned long)G32(front+FRONTFLAG)>1UL) return 0;
    }
    if (installed && uin_forceword == 3) return 2;
    if (installed && uin_forceword == 4 && forcepollremove(a)) return 0;
	if (copyin((caddr_t)0x904, (caddr_t)b, 4) || !G32(b) || (G32(b) & 1) ||
	    copyin((caddr_t)G32(b), (caddr_t)b, 4)) return 0;
	if (!installed) {
		if ((target & 0xfff) > 0xff8 ||
		    copyout((caddr_t)native, (caddr_t)target, 8)) return 0;
		bcopy((char *)wneentry, (char *)code, sizeof wneentry);
		P32(code + WNELINK, a + sizeof wneentry);
		bcopy((char *)native, (char *)code + sizeof wneentry, 8);
		P16(code + sizeof wneentry + 8, 0x4ef9);
		P32(code + sizeof wneentry + 10, target + 8);
		bcopy((caddr_t)frontentry,(caddr_t)front,sizeof front);
        P32(front+FRONTFLAG,uin_frontdiag ? 1 : 0);
        if (copyout((caddr_t)front,(caddr_t)(a+FRONTEXT),sizeof front) ||
            copyout((caddr_t)code, (caddr_t)a, sizeof code)) return 0;
		dlm_cacheflush();
		P16(redirect, 0x4ef9); P32(redirect + 2, a); P16(redirect + 6, 0x4e71);
		if (copyout((caddr_t)redirect, (caddr_t)target, 8)) {
			(void)copyout((caddr_t)native, (caddr_t)target, 8);
			dlm_cacheflush();
			return 0;
		}
		dlm_cacheflush();
	}
    uin_frontflag=a+FRONTEXT+FRONTFLAG;
    uin_frontpid=u.u_procp->p_pid;
    if(!force)return 1;
    P32(b,4);
    if(copyout((caddr_t)b,(caddr_t)(a+WNEWORD),4)) return 0;
    if(!forcepollinstall(a)) {
        if(!forcepollremove(a)) {
            P32(b,0); (void)copyout((caddr_t)b,(caddr_t)(a+WNEWORD),4);
        }
        return 0;
    }
	P32(b, 1);
    if(copyout((caddr_t)b,(caddr_t)(a+WNEWORD),4)) {
        if(!forcepollremove(a)) {
            P32(b,0); (void)copyout((caddr_t)b,(caddr_t)(a+WNEWORD),4);
        }
        return 0;
    }
    return 1;
}

int
ui_forceentry(a)
long a;
{
    return ui_wneentry(a,1);
}

extern int uin_pmidle,uin_pmidlestub;
int uin_pmidlestate;
#define pmstate uin_pmidlestate

static int
pmwatchneeded(gp)
struct guest_proc *gp;
{
    unsigned char b[2];
    if(!uin_pmidle || gp!=ui.l_gp || gp!=ui.l_active ||
        pmstate==3 || (uin_pmidlestub && pmstate==2))return 0;
    if(gp->gp_proc!=u.u_procp || copyin((caddr_t)0x15a,(caddr_t)b,2))return 1;
    return G16(b)==0x0810;
}

void
ui_fronttick(gp)
struct guest_proc *gp;
{
    struct uitask *t;unsigned char code[sizeof frontentry],b[4];long a;
    int i;
    if(gp!=ui.l_gp || gp!=ui.l_active) return;
    if(pmwatchneeded(gp))gp->gp_flags|=GPF_AWATCH;
    for(t=ui.l_task;t<ui.l_task+UI_NTASK;t++)if(t->t_gp==gp)break;
    if(t==ui.l_task+UI_NTASK || !(a=t->t_forceentry))return;
    if(!uin_frontflag) {
        if(uin_frontdiag)(void)ui_wneentry(a,0);
        return;
    }
    if(uin_frontpid!=u.u_procp->p_pid || uin_frontflag!=a+FRONTEXT+FRONTFLAG ||
        copyin((caddr_t)(a+FRONTEXT),(caddr_t)code,sizeof code))return;
    for(i=0;i<sizeof code;i++)
        if((i<FRONTFLAG || i>=FRONTFLAG+4) && code[i]!=frontentry[i])return;
    if((unsigned long)G32(code+FRONTFLAG)>1UL)return;
    if(G32(code+FRONTFLAG)!=(uin_frontdiag ? 1 : 0)) {
        P32(b,uin_frontdiag ? 1 : 0);
        (void)copyout((caddr_t)b,(caddr_t)uin_frontflag,4);
    }
}

int uin_pmidle=1;
int uin_pmidlestub=1;
long uin_pmidlerequests,uin_pmidleallocations,uin_pmidlerestores,uin_pmidlewatchedcalls;
static struct guest_proc *pmowner;
static struct proc *pmproc;
static long pmblock,pmsite,pmsys;
static unsigned char pmnative[8]={0xab,0xf7,0x20,0x0c,0x67,0,0xff,0x64};
static unsigned char pmprefix[16]={0xc,0x47,0x0,0x1,0x66,0xa,0x28,0x6d,0xfb,0xaa,0x38,0xbc,0x0,0x4,0x60,0x8};
static unsigned char pmcode[60]={0x48,0xe7,0xff,0xfe,0x42,0xe7,0x48,0x78,0x0,0x6,0x2f,0x3c,0x0,0x0,0x0,0x6e,0x42,0xa7,0x70,0x26,0x4e,0x40,0x4e,0x71,0x4f,0xef,0x0,0xc,0x44,0xdf,0x4c,0xdf,0x7f,0xff,0xc,0x47,0x0,0x1,0x66,0xe,0x28,0x6d,0xfb,0xaa,0x38,0xbc,0x0,0x4,0x4e,0xf9,0x0,0x0,0x0,0x0,0x4e,0xf9,0x0,0x0,0x0,0x0};
static int pmstubvalid();
static int pmstubpoll();
static void pmstubfinish();
static int pmstubstart();
long uin_pmidlecalls,uin_pmidlerefused;
long uin_pmidlebranches[2];
int uin_pmphaseon;
long uin_pmphasecount,uin_pmphase[8][16],uin_pmwatch[8][8];
extern long uin_watchskipped;
static long pmphasewake;
static int pmphaseseen;

static int
pmidleframe(gp,r,busy)
struct guest_proc *gp;
char *r;
int busy;
{
    static unsigned char dispatch[]={0x22,0x0d,0x2a,0x78,0x0b,0x7c,0x0c,0x6f,0,0x10,0,4,0x66,0x74,0x4a,0x6d,0xfb,0x20,0x66,0x6e};
    static unsigned char pm28[]={0x4e,0x56,0,0,0x48,0xe7,1,8,0x20,0x0d,0x2a,0x78,0x0b,0x7c,0x2e,0,0x28,0x6d,0xfb,0xae,0x70,4,0xb0,0x54,0x67,0x0c,0x70,3,0xb0,0x54,0x67,6,0x20,7,0x2a,0x40,0x60,0x20,0x4a,0x6d,0xfb,0x68,0x67,6,0x20,7,0x2a,0x40,0x60,0x14,0x2f,0x2e,0,8,0x2f,0x0c,0x4e,0xba,0x72,0x5c,0x4e,0xba,0x72,0x54};
    static unsigned char suffix[]={0x0c,0x47,0,1,0x66,0x0a,0x28,0x6d,0xfb,0xaa,0x38,0xbc,0,4,0x60,8,0xab,0xf7,0x20,0x0c,0x67,0,0xff,0x64,0x20,0x0c,0x4c,0xee,0x10,0x80,0xff,0xe8,0x4e,0x5e,0x4e,0x75};
    unsigned char b[64];long sys,entry,target,site;struct uitask *t;
    if((pmstate==3 && pmowner==gp) || !uin_pmidle || gp!=ui.l_gp || gp!=ui.l_active || gp->gp_proc!=u.u_procp ||
        ui.l_state!=LS_INUSE || !ui.l_tid || (gp->gp_vsr&0x700) || GR_A(r,4) ||
        ((GR_D(r,7)&0xffff)==1 && !busy) ||
        (busy && (pmstate!=2 || pmowner!=gp)))return 0;
    for(t=ui.l_task;t<ui.l_task+UI_NTASK;t++)if(t->t_gp==gp && t->t_proc==u.u_procp)break;
    if(t==ui.l_task+UI_NTASK || copyin((caddr_t)0x15a,(caddr_t)b,2) || G16(b)!=0x0810 ||
        copyin((caddr_t)0xb7c,(caddr_t)b,4) || (sys=G32(b))<0x3000L ||
        !ui_macrange(gp,(unsigned long)(sys-3308),3308+224) || GR_A(r,5)!=sys ||
        copyin((caddr_t)0x103c,(caddr_t)b,4) || !(entry=G32(b)) || (entry&1) ||
        !ui_macrange(gp,(unsigned long)entry,sizeof dispatch) ||
        copyin((caddr_t)entry,(caddr_t)b,sizeof dispatch) || bcmp((caddr_t)b,(caddr_t)dispatch,sizeof dispatch) ||
        copyin((caddr_t)(sys-3308+28*8),(caddr_t)b,8) || G16(b+4)!=4 || G16(b+6) ||
        !ui_macrange(gp,(unsigned long)(target=G32(b)),0x8eb0) ||
        copyin((caddr_t)target,(caddr_t)b,sizeof pm28) || bcmp((caddr_t)b,(caddr_t)pm28,sizeof pm28))return 0;
    site=target+0x8e86;
    if(GR_PC(r)!=site || copyin((caddr_t)(site-16),(caddr_t)b,sizeof suffix))return 0;
    if(pmstate==2 && pmowner==gp && pmsite==site) {
        if(!pmstubvalid(gp))return 0;
        bcopy((caddr_t)pmprefix,(caddr_t)b,16);
    }
    if(bcmp((caddr_t)b,(caddr_t)suffix,sizeof suffix))return 0;
    return 1;
}

static int
pmidleowned(gp,r)
struct guest_proc *gp;
char *r;
{
    return pmidleframe(gp,r,0);
}

long uin_wnestats[4],uin_wnelast[6];

static void
wnecapture(ptr)
long ptr;
{
    unsigned char b[20];unsigned long sleep,sp;
    if((unsigned long)ptr>0xffffffffUL-350)return;
    sp=(unsigned long)ptr+350;
    if(!ui_macrange(GUESTP(u.u_procp),sp,sizeof b) ||
        copyin((caddr_t)sp,(caddr_t)b,sizeof b))return;
    sleep=G32(b+8);
    uin_wnestats[0]++;
    uin_wnestats[!sleep ? 1 : sleep==0xffffffffUL ? 3 : 2]++;
    uin_wnelast[0]=G32(b);uin_wnelast[1]=G16(b+16);
    uin_wnelast[2]=sleep;uin_wnelast[3]=G32(b+4);
    uin_wnelast[4]=G32(b+12);uin_wnelast[5]=sp;
}

static int
frontcapture(gp,r)
struct guest_proc *gp;
char *r;
{
    unsigned char code[sizeof wneentry],ext[sizeof frontentry],rec[280],b[112],h[4],text[256];
    long a,at,ptr,w; int i,n;
    struct uitask *t;
    if(gp!=ui.l_gp || gp!=ui.l_active || uin_frontpid!=u.u_procp->p_pid)goto bad;
    for(t=ui.l_task;t<ui.l_task+UI_NTASK;t++)if(t->t_gp==gp)break;
    if(t==ui.l_task+UI_NTASK || !(a=t->t_forceentry) ||
        uin_frontflag!=a+FRONTEXT+FRONTFLAG || GR_PC(r)!=a+FRONTEXT+FRONTTRAP ||
        copyin((caddr_t)a,(caddr_t)code,sizeof code) ||
        copyin((caddr_t)(a+FRONTEXT),(caddr_t)ext,sizeof ext))goto bad;
    for(i=0;i<WNEWORD;i++)
        if((i<WNELINK || i>=WNELINK+4) && code[i]!=wneentry[i])goto bad;
    if(G32(code+WNELINK)!=a+sizeof wneentry || G32(ext+FRONTFLAG)!=1 ||
        copyin((caddr_t)WNEENT,(caddr_t)h,4) || !(w=G32(h)) || (w&1) ||
        copyin((caddr_t)w,(caddr_t)b,8) || G16(b)!=0x4ef9 ||
        G32(b+2)!=a || G16(b+6)!=0x4e71 ||
        copyin((caddr_t)(a+sizeof wneentry),(caddr_t)b,14) ||
        bcmp((caddr_t)b,(caddr_t)wnenative,8) || G16(b+8)!=0x4ef9 ||
        G32(b+10)!=w+8)goto bad;
    for(i=0;i<sizeof ext;i++)
        if((i<FRONTFLAG || i>=FRONTFLAG+4) && ext[i]!=frontentry[i])goto bad;
    if(!uin_frontdiag) {
        P32(h,0);return copyout((caddr_t)h,(caddr_t)uin_frontflag,4) ? EFAULT : 0;
    }
    at=GR_USP(r);
    if(at<0x3000L || (unsigned long)at>0xffffffdfUL ||
        copyin((caddr_t)(at+12),(caddr_t)h,4) || (ptr=G32(h))!=at+16 ||
        copyin((caddr_t)ptr,(caddr_t)rec,sizeof rec))goto bad;
    wnecapture(ptr);
    if(G32(rec)!=G32(rec+8) || G32(rec+4)!=G32(rec+12) ||
        (!G32(rec) && (unsigned long)G32(rec+4)<=2UL) ||
        copyin((caddr_t)0x904,(caddr_t)h,4) || G32(rec+20)!=G32(h))goto bad;
    w=G32(rec+16);
    if(w<0x3000L || w>=0x2400000L || (w&1) ||
        copyin((caddr_t)w,(caddr_t)b,sizeof b) || !b[110])goto bad;
    n=rec[24];if(!n)goto bad;
    bcopy((caddr_t)(rec+25),(caddr_t)text,n);
    uin_frontserial++;
    bzero((caddr_t)uin_fronttitle,sizeof uin_fronttitle);
    bcopy((caddr_t)text,(caddr_t)uin_fronttitle,n);
    uin_frontlast[0]=G32(rec);uin_frontlast[1]=G32(rec+4);
    uin_frontlast[2]=w;uin_frontlast[3]=G32(rec+20);uin_frontlast[4]=GR_PC(r);
    uin_frontcount++;uin_frontserial++;return 0;
bad:
    uin_frontrefused++;return EINVAL;
}

static int
pollsites(a, before, wanted)
long a;
unsigned char before[3][6],wanted[3][6];
{
    unsigned char tail[12],middle[sizeof pollmiddle];
    long rom=(long)ui.l_romaddr,p=a+sizeof wneentry+14;
    int i; long offsets[3];
    unsigned char *native[3];
    offsets[0]=ROMDOWN; offsets[1]=ROMUP; offsets[2]=ROMHIT;
    native[0]=polldown; native[1]=pollup; native[2]=hitnative;
    if(!rom || copyin((caddr_t)(rom+ROMDOWN+6),(caddr_t)middle,sizeof middle) ||
       bcmp((char *)middle,(char *)pollmiddle,sizeof middle) ||
       copyin((caddr_t)(rom+ROMHIT+6),(caddr_t)tail,sizeof tail) ||
       bcmp((char *)tail,(char *)hitnative+6,sizeof tail)) return EINVAL;
    for(i=0;i<3;i++) {
        P16(wanted[i],i==2 ? 0x4ef9 : 0x4eb9);
        P32(wanted[i]+2,p+(i==0 ? 0 : i==1 ? POLLUP : POLLHIT));
        if(copyin((caddr_t)(rom+offsets[i]),(caddr_t)before[i],6) ||
           (bcmp((char *)before[i],(char *)native[i],6) &&
            bcmp((char *)before[i],(char *)wanted[i],6))) return EINVAL;
    }
    return 0;
}

static int
forcepollremove(a)
long a;
{
    unsigned char old[3][6],wanted[3][6];
    long rom=(long)ui.l_romaddr,offsets[3];
    unsigned char *native[3]; int i,e,failed=0;
    if((e=pollsites(a,old,wanted))!=0) return e;
    offsets[0]=ROMDOWN; offsets[1]=ROMUP; offsets[2]=ROMHIT;
    native[0]=polldown; native[1]=pollup; native[2]=hitnative;
    for(i=0;i<3;i++) if(!bcmp((char *)old[i],(char *)wanted[i],6))
        if(copyout((caddr_t)native[i],(caddr_t)(rom+offsets[i]),6)) {
            (void)copyout((caddr_t)wanted[i],(caddr_t)(rom+offsets[i]),6);
            failed=1;
        }
    dlm_cacheflush(); return failed ? EFAULT : 0;
}

static int
forcepollinstall(a)
long a;
{
    unsigned char old[3][6],wanted[3][6],code[sizeof pollentry];
    long rom=(long)ui.l_romaddr,p=a+sizeof wneentry+14,offsets[3];
    int i;
    if(pollsites(a,old,wanted) ||
       as_checkprot(u.u_procp->p_as,(caddr_t)(rom+ROMDOWN),ROMHIT-ROMDOWN+sizeof hitnative,
         PROT_READ|PROT_WRITE|PROT_EXEC) ||
       as_checkprot(u.u_procp->p_as,(caddr_t)p,sizeof code,
         PROT_READ|PROT_WRITE|PROT_EXEC)) return 0;
    offsets[0]=ROMDOWN; offsets[1]=ROMUP; offsets[2]=ROMHIT;
    /* Fault in the private ROM page before publishing redirects. */
    for(i=0;i<3;i++) if(copyout((caddr_t)old[i],(caddr_t)(rom+offsets[i]),6)) return 0;
    bcopy((char *)pollentry,(char *)code,sizeof code); P32(code+POLLTRACK,rom+ROMTRACK);
    P32(code+POLLDIAG,uin_polldiag ? 1 : 0);
    if(copyout((caddr_t)code,(caddr_t)p,sizeof code)) return 0;
    dlm_cacheflush();
    offsets[0]=ROMDOWN; offsets[1]=ROMUP; offsets[2]=ROMHIT;
    for(i=0;i<3;i++) if(copyout((caddr_t)wanted[i],(caddr_t)(rom+offsets[i]),6)) {
        (void)copyout((caddr_t)old[i],(caddr_t)(rom+offsets[i]),6);
        (void)forcepollremove(a);
        return 0;
    }
    dlm_cacheflush(); return 1;
}

int uin_pollportdone;
unsigned char uin_pollport[256];

static void
forceport(r)
char *r;
{
    unsigned char b[8];
    long a5=GR_A(r,5),port=0,world=0,worldport=0,callback=0,a2=GR_A(r,2);
    if(!uin_polldiag || uin_pollportdone) return;
    uin_pollportdone=1;
    P32(uin_pollport,a5);
    if(!copyin((caddr_t)0x904,(caddr_t)b,4)) P32(uin_pollport+4,G32(b));
    if(a5 && !copyin((caddr_t)(a5-4),(caddr_t)b,8)) {
        port=G32(b); world=G32(b+4);
        P32(uin_pollport+8,port); P32(uin_pollport+12,world);
        if(world && !copyin((caddr_t)world,(caddr_t)b,4)) {
            worldport=G32(b); P32(uin_pollport+16,worldport);
        }
    }
    if(!copyin((caddr_t)0x1a5c,(caddr_t)b,4)) callback=G32(b);
    P32(uin_pollport+20,callback);
    P32(uin_pollport+24,a2); P32(uin_pollport+28,GR_A(r,3));
    if(callback) (void)copyin((caddr_t)callback,(caddr_t)uin_pollport+32,64);
    if(port) (void)copyin((caddr_t)port,(caddr_t)uin_pollport+96,64);
    if(a2) (void)copyin((caddr_t)a2,(caddr_t)uin_pollport+160,32);
    if(worldport) (void)copyin((caddr_t)worldport,(caddr_t)uin_pollport+192,64);
}

long uin_forcehits[32][8];
int uin_forcehitcount,uin_forcehitpending;
long uin_forcehitcalls[2],uin_forcehitlast[4];

static void
forcehit(r,mode)
char *r;
int mode;
{
    unsigned char b[12]; long sp=GR_USP(r),a3=GR_A(r,3),*h;
    if(!uin_polldiag) return;
    if(mode==2) {
        uin_forcehitpending=0;
        if(uin_forcehitcount>=32 || copyin((caddr_t)(sp+78),(caddr_t)b,4)) return;
        h=uin_forcehits[uin_forcehitcount++];
        h[0]=G32(b); h[1]=a3; h[4]=GR_D(r,3);
        if(!copyin((caddr_t)a3,(caddr_t)b,8)) { h[2]=G32(b); h[3]=G32(b+4); }
        if(!copyin((caddr_t)0x172,(caddr_t)b,1)) h[7]=b[0];
        uin_forcehitpending=1;
    } else if(uin_forcehitpending) {
        uin_forcehitpending=0;
        h=uin_forcehits[uin_forcehitcount-1];
        if(!copyin((caddr_t)(sp+16),(caddr_t)b,6)) { h[6]=G16(b); h[5]=G32(b+2); }
    }
}

long uin_forcepoll,uin_forceunpoll,uin_pollword,uin_pollmode;
long uin_pollnative[4];
long uin_pollcalls[5],uin_pollstatelast[5];
int
ui_forcepoll(gp,r,leave)
struct guest_proc *gp;
char *r;
int leave;
{
    unsigned char b[4],expected[sizeof pollentry],actual[sizeof pollentry];
    unsigned char old[3][6],wanted[3][6];
    long rom=(long)ui.l_romaddr,a,p,pc=GR_PC(r),entrypc;
    struct uitask *t;
    int result;
    if(leave==6)return pmstubpoll(gp,r);
    if(leave==5)return frontcapture(gp,r);
    if(uin_polldiag && leave>=0 && leave<=4) {
        uin_pollcalls[leave]++;
        if(leave==4) {uin_pollstatelast[0]=pc;uin_pollstatelast[1]=0;uin_pollstatelast[2]=1;uin_pollstatelast[3]=GR_USP(r);uin_pollstatelast[4]=0;}
    }
    if(uin_polldiag && (leave==2 || leave==3)) {
        uin_forcehitcalls[leave-2]++;
        uin_forcehitlast[0]=pc; uin_forcehitlast[1]=0;
        uin_forcehitlast[2]=leave; uin_forcehitlast[3]=1;
    }
    if(gp!=ui.l_active || !rom || leave<0 || leave>4) return EINVAL;
    for(t=ui.l_task;t<ui.l_task+UI_NTASK;t++) if(t->t_gp==gp) break;
    if(t==ui.l_task+UI_NTASK || !(a=t->t_forceentry)) return EINVAL;
    p=a+sizeof wneentry+14;
    entrypc=leave==0 ? p+POLLTRAP : leave==1 ? a+WNELEAVE :
        leave==2 ? p+HITPOINTPC : leave==3 ? p+HITRESULTPC : p+POLLSTATEPC;
    if(uin_polldiag && (leave==2 || leave==3)) {
        uin_forcehitlast[1]=entrypc; uin_forcehitlast[3]=2;
    }
    if(uin_polldiag && leave==4) {uin_pollstatelast[1]=entrypc;uin_pollstatelast[2]=2;}
    if(pc!=entrypc || copyin((caddr_t)(a+WNEWORD),(caddr_t)b,4) ||
       G32(b)<1 || G32(b)>4 || pollsites(a,old,wanted)) return EINVAL;
    if(uin_polldiag && (leave==2 || leave==3)) uin_forcehitlast[3]=3;
    if(uin_polldiag && leave==4) uin_pollstatelast[2]=3;
    bcopy((char *)pollentry,(char *)expected,sizeof expected);
    P32(expected+POLLTRACK,rom+ROMTRACK);
    if(copyin((caddr_t)p,(caddr_t)actual,sizeof actual) || G32(actual+POLLDIAG)>1) return EINVAL;
    P32(expected+POLLDIAG,G32(actual+POLLDIAG));
    if(bcmp((char *)actual,(char *)expected,sizeof actual)) return EINVAL;
    uin_pollword=G32(b); uin_pollmode=leave;
    if(leave==4) {
        if(uin_polldiag) uin_pollstatelast[2]=4;
        if(uin_polldiag) {
            unsigned char state[4],caller[4];
            int se=copyin((caddr_t)(GR_USP(r)+12),(caddr_t)state,4);
            int ce=copyin((caddr_t)(GR_USP(r)+80),(caddr_t)caller,4);
            uin_pollstatelast[4]=(se ? 1 : 0)|(ce ? 2 : 0);
            if(!se && !ce) {
                uin_pollnative[0]++; uin_pollnative[1]=G32(state);
                uin_pollnative[2]=G32(caller);
                if(!G32(state)) uin_pollnative[3]++;
            }
        }
        return 0;
    }
    if(leave==2 || leave==3) {
        if(uin_polldiag) uin_forcehitlast[3]=4;
        forcehit(r,leave); return 0;
    }
    if(!leave) {
        ui_forcepollwatch(gp,1);
        forceport(r); uin_forcepoll++;
        return ui_inputpoll(gp);
    }
    {
        unsigned char error[2];
        if(copyin((caddr_t)0xaf0,(caddr_t)error,2)) return EFAULT;
        result=forcepollremove(a);
        if(!result && G16(error)==20002) {
            P16(error,0);
            if(copyout((caddr_t)error,(caddr_t)0xaf0,2)) {
                psignal(gp->gp_proc,SIGSEGV);return EFAULT;
            }
        }
    }
    if(!result) { uin_forceunpoll++; ui_forcepollwatch(gp,0); }
    return result;
}

/* A forced native exit abandons the wrapper's normal return and cleanup. */
int
ui_forceexit(gp)
    struct guest_proc *gp;
{
    struct uitask *t;
    unsigned char b[4],syserr[2],actual[sizeof pollentry],expected[sizeof pollentry];
    unsigned char old[3][6],wanted[3][6];
    long a,p;
    if(gp!=ui.l_active) return EINVAL;
    for(t=ui.l_task;t<ui.l_task+UI_NTASK;t++) if(t->t_gp==gp) break;
    if(t==ui.l_task+UI_NTASK || t->t_proc!=u.u_procp || !(t->t_force & 4) ||
       !(a=t->t_forceentry) || copyin((caddr_t)(a+WNEWORD),(caddr_t)b,4) ||
       G32(b)!=3 || ui_forceentry(a)!=2 || uin_forceentry!=a) return EINVAL;
    p=a+sizeof wneentry+14;
    bcopy((char *)pollentry,(char *)expected,sizeof expected);
    P32(expected+POLLTRACK,(long)ui.l_romaddr+ROMTRACK);
    if(copyin((caddr_t)p,(caddr_t)actual,sizeof actual) || G32(actual+POLLDIAG)>1) return EINVAL;
    P32(expected+POLLDIAG,G32(actual+POLLDIAG));
    if(bcmp((char *)actual,(char *)expected,sizeof actual) ||
       pollsites(a,old,wanted)) return EINVAL;
    if(copyin((caddr_t)0xaf0,(caddr_t)syserr,2)) return EFAULT;
    P32(b,4);
    if(copyout((caddr_t)b,(caddr_t)(a+WNEWORD),4) || forcepollremove(a)) return EFAULT;
    P32(b,0);
    if(copyout((caddr_t)b,(caddr_t)(a+WNEWORD),4)) return EFAULT;
    /* A completed Force Quit must not leave the shutdown error path selected. */
    if(G16(syserr)==20002) {
        P16(syserr,0);
        if(copyout((caddr_t)syserr,(caddr_t)0xaf0,2)) return EFAULT;
    }
    ui_forcepollwatch(gp,0);
    return 0;
}

static unsigned short hfsalloc[] = {	/* f+4, up to RESUME; marked at f+18 */
	0x203c, 0, WNEOFF + FRONTEXT + sizeof frontentry, 0xa71e, 0x4e71, 0x4e71
};
static long hfst;			/* the installed hfstramp */
static int trashbusy;			/* a Trash lookup's FindFolder runs */
static long trashdir, trashpar;		/* the Trash given as a root folder, and its real parent */
static short trashvref;
static long spectab;			/* Patch.067C's special folder table, -1 if not found */

static unsigned short catcall[] = {	/* f+4: lea f+FSIZE,a0; PBGetCatInfo */
	0x41fa, FSIZE - 6, 0x7009, 0xa260, 0x4e71, 0x4e71
};
static unsigned short hopen[] = {	/* f+4: PBHOpen on the caller's a0 */
	0xa200, 0x4e71, 0x4e71, 0x4e71, 0x4e71, 0x4e71
};
static unsigned short popen[] = {	/* PBOpen, for the call without the HFS bit */
	0xa000, 0x4e71, 0x4e71, 0x4e71, 0x4e71, 0x4e71
};
#define	CINFO	108		/* CInfoPBRec, then the name */
#define	SPECX	(CINFO + 68)
#define	WDCBS	0x372		/* WDCBsPtr */
#define	DEFVCB	0x352		/* DefVCBPtr */
#define	VCBQ	0x358		/* VCBQHdr's head */
#define	REQSTVOL 0x3ee
#define	TOEXTFS	0x3f2
#define	FNFERR	(-43)
#define	PARAMERR (-50)
#define	FSMNOFFS (-431)	/* fsmFFSNotFoundErr */

static void mkspec();
static int pathnext();
static long extvcb();
static long mkparms();
static int extcall();
static int trashnext();
static int trashundo();
static void trashroot();
static void xvolbytes();

static int
hfsreuse()
{
	unsigned char b[10], p[4];

	if (!hfst || copyin((caddr_t)HFSENT, (caddr_t)p, 4) || G32(p) != hfst ||
	    copyin((caddr_t)hfst, (caddr_t)b, sizeof b) ||
	    G16(b) != HFSTRAP || G16(b + 2) != 0x4e75 || G16(b + 4) != 0x4ef9 ||
	    !G32(b + 6) || (G32(b + 6) & 1))
		return 0;
	return 1;
}

int uin_coffdiag;
int uinter_coffnative47=1;
long uin_coff47[16];
long uin_coffsyserr[2][160];
int uin_coffsyserrcount;
static struct guest_proc *coffsyserrparent;
static struct proc *coffsyserrproc;
long uin_coffsyserrparentpid;
static unsigned short coffsyserrversion;
long uin_coffcalls[2], uin_cofflast[8], uin_coffclosed, uin_coffseen;
long uin_cofftrace[64][8], uin_cofftracecount;
static struct guest_proc *coffdiagowner;
static long coffdiaguntil;
static unsigned long coffdiagpolls;

long uin_coffalert[136],uin_coffalertcount;
long uin_coffstartup[3][8];
long uin_coffqdfirst[8];

long uin_coffdraw[8][48],uin_coffdrawcount;
long uin_coffset[8][8],uin_coffsetcount;

static void
coffdrawrecord(r,pc,tw)
    char *r;
    long pc;
    unsigned short tw;
{
    static unsigned char drawtext[]={0x48,0xe7,0x20,4,0x2a,0x78,9,4,0x2f,0x2f,0,0xc,0x3f,0x2f,0,0x16,0x3f,0x2f,0,0x1c};
    unsigned char code[20],data[32];
    long *row,sp=GR_USP(r),a5=GR_A(r,5),port=0,ptr;
    int draw=pc==0x47e0a8d8L && tw==0xa885;
    int string=pc==0x47e0b3a0L && tw==0xa884;
    int setter=(pc==0x47e0a858L && tw==0xa887) || (pc==0x47e0a894L && tw==0xa88a);
    int n=draw?20:12,i,j,count,off;
    if((!draw && !string && !setter) ||
       (setter ? uin_coffsetcount>=8 : uin_coffdrawcount>=8))return;
    if(copyin((caddr_t)(pc-n),(caddr_t)code,n) ||
       G32(code)!=0x48e72004L || G32(code+4)!=0x2a780904L)return;
    if(draw ? bcmp((char *)code,(char *)drawtext,20) :
       G32(code+8)!=(setter?0x3f2f000eL:0x2f2f000cL))return;
    row=setter?uin_coffset[uin_coffsetcount++]:uin_coffdraw[uin_coffdrawcount++];
    bzero((caddr_t)row,(setter?8:48)*sizeof *row);
    row[0]=pc;row[1]=tw;row[2]=a5;row[4]=sp;
    row[3]=copyin((caddr_t)0x904,(caddr_t)data,4)?-1:G32(data);
    if(!copyin((caddr_t)(a5-4),(caddr_t)data,4))port=G32(data);
    if(setter) {
        row[5]=copyin((caddr_t)sp,(caddr_t)data,2)?-1:G16(data);
        row[6]=copyin((caddr_t)(sp+6),(caddr_t)data,4)?-1:G32(data);
        row[7]=port;return;
    }
    row[5]=port;
    for(i=0;port>=0x10000L && port<0x1000000L && i<108;i+=32) {
        n=108-i<32?108-i:32;
        if(copyin((caddr_t)(port+i),(caddr_t)data,n))break;
        for(j=0;j<n/4;j++)row[10+i/4+j]=G32(data+4*j);
        row[9]+=n;
    }
    if(copyin((caddr_t)sp,(caddr_t)data,draw?8:4))return;
    ptr=G32(data+(draw?4:0));off=draw?(short)G16(data+2):1;
    count=draw?(short)G16(data):0;
    if(string) {
        if(copyin((caddr_t)ptr,(caddr_t)data,1))return;
        count=data[0];
    }
    row[6]=ptr;row[7]=off;row[8]=count;
    row[45]=copyin((caddr_t)(sp+(draw?12:8)),(caddr_t)data,4)?-1:G32(data);
    if(off<0 || count<=0 || ptr<0x10000L || ptr>=0x1000000L || off>0x1000000L-ptr)return;
    n=count>32?32:count;
    bzero((caddr_t)data,sizeof data);
    if(!copyin((caddr_t)(ptr+off),(caddr_t)data,n)) {
        for(i=0;i<8;i++)row[37+i]=G32(data+4*i);
        row[46]=n;
    }
}

long uin_coffnatdraw[8][48],uin_coffnatdrawcount;

static void
coffnatdrawrecord(r,pc,tw)
    char *r;
    long pc;
    unsigned short tw;
{
    unsigned char data[32];
    long *row,sp=GR_USP(r),a5=GR_A(r,5),port=0,ptr;
    int draw=tw==0xa885,string=tw==0xa884,n,i,j,count,off;
    if((!draw && !string) || uin_coffnatdrawcount>=8 ||
        !uin_coffdiag || (long)(lbolt-coffdiaguntil)>=0 ||
        pc==0x47e0a8d8L || pc==0x47e0b3a0L)return;
    if(a5<0x10000L || a5>0x1000000L-4 || copyin((caddr_t)a5,(caddr_t)data,4))return;
    ptr=G32(data);
    if(ptr<0x10000L || ptr>0x1000000L-4 || copyin((caddr_t)ptr,(caddr_t)data,4))return;
    port=G32(data);
    if(port<0x10000L || port>0x1000000L-108 ||
        copyin((caddr_t)(port+68),(caddr_t)data,2) || G16(data)!=0x7f0d)return;
    row=uin_coffnatdraw[uin_coffnatdrawcount++];
    bzero((caddr_t)row,48*sizeof *row);
    row[0]=pc;row[1]=tw;row[2]=a5;row[4]=sp;
    row[3]=copyin((caddr_t)0x904,(caddr_t)data,4)?-1:G32(data);
    row[5]=port;
    row[47]=copyin((caddr_t)(pc-4),(caddr_t)data,4)?0:G32(data);
    for(i=0;port>=0x10000L && port<0x1000000L && i<108;i+=32) {
        n=108-i<32?108-i:32;
        if(copyin((caddr_t)(port+i),(caddr_t)data,n))break;
        for(j=0;j<n/4;j++)row[10+i/4+j]=G32(data+4*j);
        row[9]+=n;
    }
    if(copyin((caddr_t)sp,(caddr_t)data,draw?8:4))return;
    ptr=G32(data+(draw?4:0));off=draw?(short)G16(data+2):1;
    count=draw?(short)G16(data):0;
    if(string) {
        if(copyin((caddr_t)ptr,(caddr_t)data,1))return;
        count=data[0];
    }
    row[6]=ptr;row[7]=off;row[8]=count;

    if(off<0 || count<=0 || ptr<0x10000L || ptr>=0x1000000L || off>0x1000000L-ptr)return;
    n=count>32?32:count;
    bzero((caddr_t)data,sizeof data);
    if(!copyin((caddr_t)(ptr+off),(caddr_t)data,n)) {
        for(i=0;i<8;i++)row[37+i]=G32(data+4*i);
        row[46]=n;
    }
}

long uin_cofftooltext[8][48],uin_cofftooltextcount;
long uin_cofftoolbits[8][48],uin_cofftoolbitscount;
static unsigned char cofftoolsig[7][24]={
    {0x3f,0x2e,0xff,0xf0,0x30,0x2e,0xff,0xac,0x48,0xc0,0xd0,0xae,0xff,0xcc,0x2f,0x00,0x2f,0x2e,0xff,0xc4,0x2f,0x2e,0xff,0xc8},
    {0x20,0x68,0x00,0x3c,0x48,0x68,0x00,0x02,0x48,0x6e,0xff,0xd8,0x48,0x6e,0xff,0xca,0x70,0x02,0x3f,0x00,0x72,0x00,0x2f,0x01},
    {0x20,0x68,0x00,0x3c,0x48,0x68,0x00,0x02,0x48,0x6e,0xff,0xd8,0x48,0x6e,0xff,0xc2,0x70,0x02,0x3f,0x00,0x72,0x00,0x2f,0x01},
    {0x20,0x68,0x00,0x3c,0x48,0x68,0x00,0x02,0x48,0x6e,0xff,0xd8,0x48,0x6e,0xff,0xca,0x70,0x02,0x3f,0x00,0x72,0x00,0x2f,0x01},
    {0x20,0x68,0x00,0x3c,0x48,0x68,0x00,0x02,0x48,0x6e,0xff,0xd8,0x48,0x6e,0xff,0xca,0x70,0x01,0x3f,0x00,0x72,0x00,0x2f,0x01},
    {0x20,0x6e,0xff,0xfc,0x48,0x68,0x00,0x02,0x48,0x6e,0xff,0xf4,0x48,0x6e,0xff,0xe6,0x70,0x00,0x3f,0x00,0x72,0x00,0x2f,0x01},
    {0x20,0x68,0x00,0x3c,0x48,0x68,0x00,0x02,0x48,0x6e,0xff,0xf8,0x48,0x6e,0xff,0xea,0x3f,0x2e,0x00,0x1a,0x70,0x00,0x2f,0x00}
};

long uin_cofffont[8][64],uin_cofffontcount;
long uin_cofffontpost[8][64],uin_cofffontpostcount;
static int cofffontphase;
static struct guest_proc *cofffontowner;
static long cofffontfp,cofffontsp,cofffonta5,cofffontpc,cofffontport;

long uin_coffpixels[8][48],uin_coffpixelscount;
static unsigned char coffpixelbefore[32][8];
static long coffpixelmeta[8],coffpixelwm[3];
static int coffpixelready;

long uin_coffvis[8][32],uin_coffviscount;

static void
coffvisrecord(pc,port,p)
long pc,port;
unsigned char *p;
{
    unsigned char b[96];long *row,handle,ptr,size;int i;
    if(uin_coffviscount>=8)return;
    row=uin_coffvis[uin_coffviscount++];bzero((caddr_t)row,32*sizeof *row);
    row[0]=pc;row[1]=port;row[2]=G32(p+48);row[3]=handle=G32(p+24);row[7]=G32(p+28);
    if(handle<0x10000L || handle>0x1000000L-4 || copyin((caddr_t)handle,(caddr_t)b,4))return;
    row[4]=ptr=G32(b);
    if(ptr<0x10000L || ptr>0x1000000L-10 || copyin((caddr_t)ptr,(caddr_t)b,10))return;
    row[5]=size=G16(b);
    if(size<10 || size>96 || (size&1) || ptr>0x1000000L-size)return;
    bzero((caddr_t)b,sizeof b);
    if(copyin((caddr_t)ptr,(caddr_t)b,(int)size))return;
    for(i=0;i<24;i++)row[8+i]=G32(b+4*i);
    row[6]=size;
}

long uin_coffwin[8][96],uin_coffwincount;

static void
coffwinrecord(pc,port)
long pc,port;
{
    unsigned char w[126],b[96];long *row,handle,ptr,size;int i,j;
    if(uin_coffwincount>=8 || port<0x10000L || port>0x1000000L-126 ||
        copyin((caddr_t)0x9d6,(caddr_t)b,4) || G32(b)!=port ||
        copyin((caddr_t)port,(caddr_t)w,126) || (G16(w+6)&0xc000))return;
    row=uin_coffwin[uin_coffwincount++];bzero((caddr_t)row,96*sizeof *row);
    for(i=0;i<3;i++,row+=32) {
        row[0]=port;row[1]=handle=G32(w+114+4*i);row[6]=G32(w+48);row[7]=pc;
        if(handle<0x10000L || handle>0x1000000L-4 || copyin((caddr_t)handle,(caddr_t)b,4))continue;
        row[2]=ptr=G32(b);
        if(ptr<0x10000L || ptr>0x1000000L-10 || copyin((caddr_t)ptr,(caddr_t)b,10))continue;
        row[3]=size=G16(b);
        if(size<10 || size>96 || (size&1) || ptr>0x1000000L-size)continue;
        bzero((caddr_t)b,sizeof b);
        if(copyin((caddr_t)ptr,(caddr_t)b,(int)size))continue;
        for(j=0;j<24;j++)row[8+j]=G32(b+4*j);
        row[4]=size;
    }
}

static void
coffpixelarm(pc,port)
long pc,port;
{
    unsigned char b[52],h[4],gd[62],pm[50];unsigned long base,end;long stride,top,left,height,width,y,x,rows,handle,ptr,depth;int i;
    coffpixelready=0;
    if(uin_coffpixelscount>=8 || port<0x10000L || port>0x1000000L-52 || copyin((caddr_t)port,(caddr_t)b,52))return;
    coffvisrecord(pc,port,b);coffwinrecord(pc,port);
    base=(unsigned long)G32(b+2);stride=G16(b+6);
    top=(short)G16(b+8);left=(short)G16(b+10);
    height=(short)G16(b+12)-top;width=(short)G16(b+14)-left;
    if(base<0xe0000000UL || base>=0xe0100000UL || stride<=0 || stride>1024 ||
        width<=0 || width>8*stride || height<=0 || height>2048)return;
    y=(short)G16(b+48)-top-24;x=(short)G16(b+50)-left;
    if(copyin((caddr_t)0xcc8,(caddr_t)h,4))return;
    handle=G32(h);
    if(handle<0x1000L || handle>0x1000000L-4 || (handle&1) ||
        copyin((caddr_t)handle,(caddr_t)h,4))return;
    ptr=G32(h);
    if(ptr<0x1000L || ptr>0x1000000L-62 || (ptr&1) ||
        copyin((caddr_t)ptr,(caddr_t)gd,62))return;
    handle=G32(gd+22);
    if(handle<0x1000L || handle>0x1000000L-4 || (handle&1) ||
        copyin((caddr_t)handle,(caddr_t)h,4))return;
    ptr=G32(h);
    if(ptr<0x1000L || ptr>0x1000000L-50 || (ptr&1) ||
        copyin((caddr_t)ptr,(caddr_t)pm,50) ||
        (unsigned long)G32(pm)!=base)return;
    depth=G16(pm+32);stride=G16(pm+4)&0x3fff;
    if(depth!=1 && depth!=2 && depth!=4 && depth!=8 && depth!=16 && depth!=32)return;
    top=(short)G16(pm+6);left=(short)G16(pm+8);
    height=(short)G16(pm+10)-top;width=(short)G16(pm+12)-left;
    if(stride<=0 || stride>4096 || width<=0 || width>8*stride/depth ||
        height<=0 || height>2048)return;
    end=base+(unsigned long)(stride*height);
    if(end<base || end>0xe0100000UL)return;
    y-=top;x-=left;
    if(y<0)y=0;
    if(x<0 || x>=width || y>=height)return;
    x=x*depth/8;if(x>stride-8)return;
    rows=height-y;if(rows>32)rows=32;
    for(i=0;i<rows;i++)if(copyin((caddr_t)(base+(y+i)*stride+x),(caddr_t)coffpixelbefore[i],8))return;
    coffpixelmeta[0]=pc;coffpixelmeta[1]=(long)base;coffpixelmeta[2]=stride;
    coffpixelmeta[3]=y;coffpixelmeta[4]=x;coffpixelmeta[5]=rows;coffpixelmeta[6]=port;
    coffpixelmeta[7]=G32(b+48);
    for(i=0;i<3;i++)coffpixelwm[i]=-1;
    if(!copyin((caddr_t)0x9d6,(caddr_t)b,12))for(i=0;i<3;i++)coffpixelwm[i]=G32(b+4*i);
    coffpixelready=1;
}

static void
coffpixelafter(pc)
long pc;
{
    unsigned char b[8];long *row,changed=0;unsigned long before=0,after=0;unsigned int v;int i,j,k,n;
    if(!coffpixelready)return;
    coffpixelready=0;
    if(uin_coffpixelscount>=8)return;
    row=uin_coffpixels[uin_coffpixelscount++];bzero((caddr_t)row,48*sizeof *row);
    for(i=0;i<8;i++)row[i]=coffpixelmeta[i];row[43]=pc;
    for(i=0;i<coffpixelmeta[5];i++) {
        if(copyin((caddr_t)((unsigned long)coffpixelmeta[1]+(coffpixelmeta[3]+i)*coffpixelmeta[2]+coffpixelmeta[4]),(caddr_t)b,8))return;
        n=0;
        for(j=0;j<8;j++) {
            before=before*33^coffpixelbefore[i][j];after=after*33^b[j];
            v=b[j]^coffpixelbefore[i][j];for(k=0;k<8;k++) {n+=v&1;v>>=1;}
        }
        row[8+i]=n;changed+=n;
    }
    row[40]=changed;row[41]=before;row[42]=after;row[44]=1;
    row[45]=coffpixelwm[2];row[46]=coffpixelwm[0];row[47]=coffpixelwm[1];
}

static void
cofffontrecord(pc,a5,port)
long pc,a5,port;
{
    unsigned char b[32];long *row,slot,ptr,handle;int i,j,n,first,last,height,words,x0,x1,bytes;
    if((cofffontphase?uin_cofffontpostcount:uin_cofffontcount)>=8)return;
    row=cofffontphase?uin_cofffontpost[uin_cofffontpostcount++]:uin_cofffont[uin_cofffontcount++];bzero((caddr_t)row,64*sizeof *row);
    row[0]=pc;row[1]=a5;row[3]=port;
    if(a5<0x10000L || a5>0x1000000L-4 || copyin((caddr_t)a5,(caddr_t)b,4))return;
    slot=G32(b);row[2]=slot;
    if(slot<0x100b0L || slot>0x1000000L-4 || copyin((caddr_t)(slot-176),(caddr_t)b,4))return;
    ptr=G32(b);row[4]=ptr;
    if(ptr==0x998L || (ptr>=0x10000L && ptr<=0x1000000L-26)) {
        bzero((caddr_t)b,sizeof b);
        if(!copyin((caddr_t)ptr,(caddr_t)b,26)) {
            for(i=0;i<8;i++)row[18+i]=G32(b+4*i);
            row[17]=26;row[59]|=1;handle=G32(b+2);row[5]=handle;
            if(handle>=0x10000L && handle<=0x1000000L-4 && !copyin((caddr_t)handle,(caddr_t)b,4)) {
                ptr=G32(b);row[6]=ptr;bzero((caddr_t)b,sizeof b);
                if(ptr>=0x10000L && ptr<=0x1000000L-26 && !copyin((caddr_t)ptr,(caddr_t)b,26)) {
                    for(i=0;i<8;i++)row[26+i]=G32(b+4*i);
                    first=(short)G16(b+2);last=(short)G16(b+4);height=(short)G16(b+14);words=(short)G16(b+24);
                    row[7]=height;row[8]=words;row[9]=((unsigned long)(first&0xffff)<<16)|(last&0xffff);row[59]|=2;
                    if(first<=35 && last>=35 && first>=0 && last<=255 && height>0 && height<=64 && words>0 && words<=512) {
                        bytes=2*words*height;row[58]=bytes;
                        slot=ptr+26+bytes+2*(35-first);
                        if(slot>=ptr && slot<=0x1000000L-4 && !copyin((caddr_t)slot,(caddr_t)b,4)) {
                            x0=G16(b);x1=G16(b+2);row[10]=x0;row[11]=x1;
                            if(x0<=x1 && x1<=words*16)for(i=0;i<height && i<16;i++) {
                                slot=ptr+26+2*words*i+x0/8;n=2*words-x0/8;if(n>4)n=4;
                                bzero((caddr_t)b,sizeof b);
                                if(n<=0 || slot<ptr || slot>0x1000000L-n || copyin((caddr_t)slot,(caddr_t)b,n))break;
                                row[34+i]=G32(b);row[12]++;row[59]|=4;
                            }
                        }
                    }
                }
            }
        }
    }
    if(port<0x10000L || port>0x1000000L-32)return;
    for(i=0;i<2;i++) {
        if(copyin((caddr_t)(port+24+4*i),(caddr_t)b,4))continue;
        handle=G32(b);row[13+2*i]=handle;
        if(handle<0x10000L || handle>0x1000000L-4 || copyin((caddr_t)handle,(caddr_t)b,4))continue;
        ptr=G32(b);row[14+2*i]=ptr;bzero((caddr_t)b,sizeof b);
        if(ptr<0x10000L || ptr>0x1000000L-10 || copyin((caddr_t)ptr,(caddr_t)b,10))continue;
        for(j=0;j<4;j++)row[50+4*i+j]=G32(b+4*j);
        row[59]|=8<<i;
    }
}

long uin_coffconvert[8][40],uin_coffconvertcount;
static int coffconvertsampled;

static void
coffconvertrecord(gp,r,pc,tw)
struct guest_proc *gp;
char *r;
long pc;
unsigned short tw;
{
    static unsigned char entry[16]={0x20,0x40,0x20,0x0f,0xe4,0x40,0x55,0xc0,0x64,0x02,0x55,0x4f,0x4e,0x56,0xfd,0x84};
    unsigned char b[52],call[4],head[16];long frame=GR_A(r,6),next,ret,target,parent,older,*row;
    int i,j,linked;
    if(gp!=cofffontowner || !uin_coffdiag || coffconvertsampled ||
        uin_coffconvertcount<0 || uin_coffconvertcount>=8 ||
        (long)(lbolt-coffdiaguntil)>=0)return;
    for(i=0;i<8;i++) {
        if(frame<0x1027cL || (frame&1) || (unsigned long)frame>0xfffffff7UL ||
            copyin((caddr_t)frame,(caddr_t)b,8))return;
        next=G32(b);ret=G32(b+4);
        if(ret>=0x10004L && ret<0x1000000L && !(ret&1) &&
            !copyin((caddr_t)(ret-4),(caddr_t)call,4) && G16(call)==0x4eba) {
            target=ret-2+(short)G16(call+2);
            if(target>=0x10000L && target<=0x1000000L-16 &&
                !copyin((caddr_t)target,(caddr_t)head,16) &&
                !bcmp((caddr_t)head,(caddr_t)entry,16)) {
                parent=next;linked=0;
                for(j=0;j<8;j++) {
                    if(parent==cofffontfp) {linked=1;break;}
                    if(parent<0x10000L || (parent&1) || (unsigned long)parent>0xfffffffbUL ||
                        copyin((caddr_t)parent,(caddr_t)b,4))return;
                    older=G32(b);
                    if((unsigned long)older<=(unsigned long)parent ||
                        (unsigned long)older-(unsigned long)parent>0x10000UL)return;
                    parent=older;
                }
                if(!linked || copyin((caddr_t)(frame-388),(caddr_t)b,50))return;
                row=uin_coffconvert[uin_coffconvertcount++];bzero((caddr_t)row,40*sizeof *row);
                row[0]=pc;row[1]=tw;row[2]=cofffontpc;row[3]=frame;row[4]=ret;row[5]=target;
                row[8]=50;row[9]=cofffontport;row[23]=GR_A(r,5);
                b[50]=b[51]=0;for(j=0;j<13;j++)row[10+j]=G32(b+4*j);
                row[6]=copyin((caddr_t)(frame-154),(caddr_t)b,4)?-1:G32(b);
                row[7]=0;
                if(!copyin((caddr_t)(frame-419),(caddr_t)b,1))row[7]=(unsigned char)b[0];else row[7]=-1;
                if(!copyin((caddr_t)(frame-543),(caddr_t)b,1))row[7]|=(long)(unsigned char)b[0]<<8;else row[7]=-1;
                row[24]=copyin((caddr_t)0x904,(caddr_t)b,4)?-1:G32(b);
                coffconvertsampled=1;return;
            }
        }
        if((unsigned long)next<=(unsigned long)frame || (unsigned long)next-(unsigned long)frame>0x10000UL)return;
        frame=next;
    }
}

static void
cofffontafter(gp,r,pc)
struct guest_proc *gp;
char *r;
long pc;
{
    long n;
    if(cofffontowner!=gp || !uin_coffdiag || (long)(lbolt-coffdiaguntil)>=0 ||
        GR_A(r,6)!=cofffontfp || GR_A(r,5)!=cofffonta5 || GR_USP(r)<cofffontsp)return;
    coffpixelafter(pc);cofffontowner=0;cofffontphase=1;n=uin_cofffontpostcount;
    cofffontrecord(cofffontpc,cofffonta5,cofffontport);cofffontphase=0;
    if(uin_cofffontpostcount>n) {
        uin_cofffontpost[n][60]=pc;uin_cofffontpost[n][61]=cofffontfp;
        uin_cofffontpost[n][62]=cofffontsp;uin_cofffontpost[n][63]=1;
    }
}

static void
cofffontarm(r,pc,port)
char *r;
long pc,port;
{
    unsigned char b[16];long target;
    if(copyin((caddr_t)0x1008,(caddr_t)b,4))return;
    target=G32(b);
    if(!((target>=0x10000L && target<=0x1000000L-16) ||
        (target>=0x40800000L && target<=0x40900000L-16)) || copyin((caddr_t)target,(caddr_t)b,16))return;
    if(G32(b)==0x20552050L && G32(b+4)==0x0c680031L && G32(b+8)==0x00486704L && G16(b+12)==0x4efa) {
        target+=14+(short)G16(b+14);
        if(!((target>=0x10000L && target<=0x1000000L-4) ||
            (target>=0x40800000L && target<=0x40900000L-4)) || copyin((caddr_t)target,(caddr_t)b,4))return;
    }
    if(G16(b)!=0x4e56)return;
    coffconvertsampled=0;cofffontowner=coffdiagowner;cofffontfp=GR_A(r,6);cofffontsp=GR_USP(r);
    cofffonta5=GR_A(r,5);cofffontpc=pc;cofffontport=port;coffpixelarm(pc,port);
}

static void
cofftoolrecord(r,pc,tw)
    char *r;
    long pc;
    unsigned short tw;
{
    unsigned char data[32];long *row,sp=GR_USP(r),a5=GR_A(r,5),port=0,ptr;
    int text=tw==0xa882,bits=tw==0xa8ec,i,j,n,count;
    if((!text && !bits) || !uin_coffdiag || (long)(lbolt-coffdiaguntil)>=0 ||
        pc<0x10018L || pc>=0x1000000L ||
        (text?uin_cofftooltextcount:uin_cofftoolbitscount)>=8 ||
        copyin((caddr_t)(pc-24),(caddr_t)data,24))return;
    for(i=text?0:1;i<(text?1:7);i++)if(!bcmp((caddr_t)data,(caddr_t)cofftoolsig[i],24))break;
    if(i==(text?1:7))return;
    row=text?uin_cofftooltext[uin_cofftooltextcount++]:uin_cofftoolbits[uin_cofftoolbitscount++];
    bzero((caddr_t)row,48*sizeof *row);
    row[0]=pc;row[1]=tw;row[2]=a5;row[4]=sp;
    row[3]=copyin((caddr_t)0x904,(caddr_t)data,4)?-1:G32(data);
    if(a5>=0x10000L && a5<=0x1000000L-4 && !copyin((caddr_t)a5,(caddr_t)data,4)) {
        ptr=G32(data);
        if(ptr>=0x10000L && ptr<=0x1000000L-4 && !copyin((caddr_t)ptr,(caddr_t)data,4))port=G32(data);
    }
    row[5]=port;
    for(i=0;port>=0x10000L && port<=0x1000000L-108 && i<108;i+=32) {
        n=108-i<32?108-i:32;
        if(copyin((caddr_t)(port+i),(caddr_t)data,n))break;
        for(j=0;j<n/4;j++)row[10+i/4+j]=G32(data+4*j);
        row[9]+=n;
    }
    if(text) {cofffontrecord(pc,a5,port);cofffontarm(r,pc,port);}
    if(copyin((caddr_t)sp,(caddr_t)data,text?14:22))return;
    if(text) {
        ptr=G32(data+8);count=(short)G16(data+12);
        row[6]=ptr;row[7]=G32(data+4);row[8]=count;row[45]=G32(data);
        if(count<=0 || ptr<0x10000L || ptr>=0x1000000L)return;
        n=count>32?32:count;
        if(ptr>0x1000000L-n)return;
        bzero((caddr_t)data,sizeof data);
        if(!copyin((caddr_t)ptr,(caddr_t)data,n)) {
            for(i=0;i<8;i++)row[37+i]=G32(data+4*i);
            row[46]=n;
        }
    } else {
        row[6]=G32(data+18);row[7]=G32(data+14);row[8]=(short)G16(data+4);
        row[45]=G32(data+10);row[46]=G32(data+6);row[47]=G32(data);
        for(i=0;i<2;i++) {
            ptr=row[6+i];bzero((caddr_t)data,sizeof data);
            if(ptr<0x10000L || ptr>0x1000000L-14 || copyin((caddr_t)ptr,(caddr_t)data,14))continue;
            for(j=0;j<4;j++)row[37+4*i+j]=G32(data+4*j);
        }
    }
}

long uin_coffviswrite[8][128],uin_coffviswritecount;

static void
coffviswriterecord(gp,r,pc,tw)
struct guest_proc *gp;
char *r;
long pc;
unsigned short tw;
{
    unsigned char b[96],args[12],w[32];long *row,*part,win,ptr,size,handle;
    unsigned long sp=GR_USP(r);int i,j,n=tw==0xa8dc ? 8 : 12;
    if((tw!=0xa8e6 && tw!=0xa8dc && tw!=0xa8e4) || !uin_coffdiag || gp!=coffdiagowner ||
        (long)(lbolt-coffdiaguntil)>=0 || uin_coffviswritecount>=8 ||
        sp>0xffffffffUL-n || copyin((caddr_t)sp,(caddr_t)args,n) ||
        copyin((caddr_t)0x9d6,(caddr_t)b,4))return;
    win=G32(b);
    if(win<0x10000L || win>0x1000000L-32 ||
        copyin((caddr_t)win,(caddr_t)w,32) || (G16(w+6)&0xc000) ||
        G32(args)!=G32(w+24))return;
    row=uin_coffviswrite[uin_coffviswritecount++];bzero((caddr_t)row,128*sizeof *row);
    row[0]=pc;row[1]=sp;row[2]=GR_A(r,6);row[3]=GR_A(r,5);row[4]=win;
    for(i=0;i<n/4;i++)row[5+i]=G32(args+4*i);
    if(!copyin((caddr_t)0x904,(caddr_t)b,4))row[8]=G32(b);
    row[10]=copyin((caddr_t)0x1004c260L,(caddr_t)b,4) ? -1 : G32(b);row[11]=tw;
    if((unsigned long)row[2]<=0xfffffff7UL && !copyin((caddr_t)row[2],(caddr_t)b,8)) {
        row[12]=G32(b);row[13]=G32(b+4);
    }
    if(pc>=16 && !copyin((caddr_t)(pc-16),(caddr_t)b,32))
        for(i=0;i<8;i++)row[16+i]=G32(b+4*i);
    for(i=0;i<n/4;i++) {
        part=row+32+32*i;part[0]=handle=G32(args+4*i);
        if(handle<0x10000L || handle>0x1000000L-4 || copyin((caddr_t)handle,(caddr_t)b,4))continue;
        part[1]=ptr=G32(b);
        if(ptr<0x10000L || ptr>0x1000000L-10 || copyin((caddr_t)ptr,(caddr_t)b,10))continue;
        part[2]=size=G16(b);
        if(size<10 || size>96 || (size&1) || ptr>0x1000000L-size)continue;
        bzero((caddr_t)b,sizeof b);
        if(copyin((caddr_t)ptr,(caddr_t)b,(int)size))continue;
        for(j=0;j<24;j++)part[8+j]=G32(b+4*j);
        part[3]=size;
    }
}

static void
coffalertrecord(r,pc,tw)
    char *r;
    long pc;
    unsigned short tw;
{
    unsigned char bytes[32],prefix[12];
    long sp=GR_USP(r),*sample;
    int i,j,k=-1;

    coffdrawrecord(r,pc,tw);
    if(pc==0x47e0a5c0L && tw==0xa86e && !uin_coffqdfirst[0] &&
       !copyin((caddr_t)(pc-12),(caddr_t)prefix,12) &&
       G32(prefix)==0x48e72004L && G32(prefix+4)==0x2a780904L &&
       G32(prefix+8)==0x2f2f000cL) {
        uin_coffqdfirst[0]=pc;uin_coffqdfirst[1]=GR_A(r,5);
        uin_coffqdfirst[2]=copyin((caddr_t)0x904,(caddr_t)bytes,4)?-1:G32(bytes);
        uin_coffqdfirst[3]=copyin((caddr_t)(GR_A(r,5)-4),(caddr_t)bytes,4)?-1:G32(bytes);
    }
    if(pc==0x100278d6L)k=0;
    if(pc==0x100280f2L)k=1;
    if(pc==0x1002825aL)k=2;
    if(k>=0 && tw==0xa08b &&
       !copyin((caddr_t)(pc-10),(caddr_t)prefix,12) &&
       G32(prefix)==0x201f3f3cL && G16(prefix+4)==(k==0?0x301:k==1?0x501:0x401) &&
       G32(prefix+6)==0x204f2f00L && G16(prefix+10)==tw) {
        sample=uin_coffstartup[k];sample[0]=pc;sample[1]=sp;
        sample[2]=GR_D(r,0);sample[3]=GR_A(r,0);
        if(!copyin((caddr_t)sp,(caddr_t)bytes,16))
            for(i=0;i<4;i++)sample[4+i]=G32(bytes+4*i);
    }
    if(tw<0xa985 || tw>0xa988 || uin_coffalertcount>=4)return;
    uin_coffalertcount++;
    sample=uin_coffalert;
    for(i=0;i<136;i++)sample[i]=0;
    sample[0]=pc;sample[1]=sp;sample[2]=tw;
    sample[3]=GR_D(r,0);sample[4]=GR_A(r,0);sample[5]=GR_A(r,5);
    sample[6]=copyin((caddr_t)0xaf0,(caddr_t)bytes,2)?-1:G16(bytes);
    for(i=0;i<512;i+=32) {
        if(copyin((caddr_t)(sp+i),(caddr_t)bytes,32))break;
        for(j=0;j<8;j++)sample[8+i/4+j]=G32(bytes+4*j);
        sample[7]+=32;
    }
}

/* Exact runtime bridges expose their frames without changing dispatch. */
static int
coffdiagwatch(gp, r)
    struct guest_proc *gp;
    char *r;
{
    static unsigned char launch[] = {
        0x48,0xe7,0x20,0x04,0x2a,0x79,0,0,9,4,0x42,0x67,
        0x2f,0x2f,0,0x0e,0x2f,0x2f,0,0x16,0x2f,0x2f,0,0x1e,
        0x3f,0x2f,0,0x28,0x2f,0x2f,0,0x2c,0x3f,0x2f,0,0x36,0xa8,0x8f
    };
    static unsigned char front[] = {
        0x48,0xe7,0x20,0x04,0x2a,0x79,0,0,9,4,0x42,0xa7,
        0x3f,0x2f,0,0x12,0x3f,0x3c,0,0x11,0xa8,0x8f
    };
    unsigned char code[38],frame[22];
    long pc=GR_PC(r),sp=GR_USP(r),at;
    int which,n,reason;

    if(gp!=coffdiagowner)return 0;
    reason=!uin_coffdiag ? 1 : !ui_patchshared(gp) ||
        !(AUXP(gp)->ap_mac & APM_COFF) ? 2 :
        (long)(lbolt-coffdiaguntil)>=0 ? 3 : ++coffdiagpolls>65536UL ? 4 : 0;
    uin_coffseen=coffdiagpolls;
    if(reason) {uin_coffclosed=reason;coffdiagowner=0;return 0;}
    if(pc!=0x47e061beL && pc!=0x47e095f2L)return 1;
    which=pc==0x47e095f2L;at=which ? 0x47e095deL : 0x47e0619aL;
    n=which ? sizeof front : sizeof launch;
    if(copyin((caddr_t)at,(caddr_t)code,n) ||
        bcmp((char *)code,(char *)(which ? front : launch),n) ||
        copyin((caddr_t)sp,(caddr_t)frame,which ? 8 : 22) ||
        G16(frame)!=(which ? 17 : 55))return 1;
    uin_coffcalls[which]++;
    uin_cofflast[0]=pc;uin_cofflast[1]=sp;uin_cofflast[2]=G16(frame);
    uin_cofflast[3]=which ? G16(frame+2) : G32(frame+2);
    uin_cofflast[4]=which ? G32(frame+4) : G16(frame+6);
    uin_cofflast[5]=which ? 0 : G32(frame+8);
    uin_cofflast[6]=which ? 0 : G32(frame+12);
    uin_cofflast[7]=which ? 0 : G32(frame+16);
    if(uin_coffcalls[which]<=4) printf("uinter: COFF bridge %ld PC %lx SP %lx fields %lx %lx %lx %lx %lx\n",
        uin_cofflast[2],pc,sp,uin_cofflast[3],uin_cofflast[4],
        uin_cofflast[5],uin_cofflast[6],uin_cofflast[7]);
    if(which) {uin_coffclosed=5;coffdiagowner=0;return 0;}
    return 1;
}

static int
coffsyserrowned(gp)
struct guest_proc *gp;
{
    unsigned char b[2];
    if(!uin_coffdiag || !coffsyserrparent || gp!=coffsyserrparent ||
        ui.l_state!=LS_INUSE || ui.l_gp!=gp || ui.l_proc!=coffsyserrproc ||
        gp->gp_proc!=coffsyserrproc || u.u_procp!=coffsyserrproc ||
        prfind(uin_coffsyserrparentpid)!=coffsyserrproc || !ui_task(gp))return 0;
    return !copyin((caddr_t)0x15a,(caddr_t)b,2) && G16(b)==coffsyserrversion;
}

static void
coffsyserrrecord(r,tw)
char *r;
unsigned short tw;
{
    unsigned char b[32];long *sample,sp=GR_USP(r);int i,j,error;
    if(!uin_coffdiag || uin_coffsyserrcount<0 || uin_coffsyserrcount>=2 ||
        (unsigned)sp>0xfffffdffU ||
        (tw!=0xa9c9 && tw!=0xabf7))return;
    error=copyin((caddr_t)0xaf0,(caddr_t)b,2)?-1:G16(b);
    if(tw==0xabf7 && error<=0)return;
    sample=uin_coffsyserr[uin_coffsyserrcount++];
    sample[0]=GR_PC(r);sample[1]=sp;sample[2]=tw;sample[3]=error;
    sample[4]=GR_A(r,6);sample[5]=GR_A(r,5);sample[6]=GR_D(r,0);
    sample[7]=GR_A(r,0);sample[8]=GR_SR(r);
    sample[9]=copyin((caddr_t)0x904,(caddr_t)b,4)?-1:G32(b);
    sample[10]=copyin((caddr_t)0x908,(caddr_t)b,4)?-1:G32(b);
    sample[11]=copyin((caddr_t)0x1ef4,(caddr_t)b,4)?-1:G32(b);
    for(i=0;i<15;i++)sample[16+i]=G32(r+4+4*i);
    for(i=0;i<512;i+=32) {
        if(copyin((caddr_t)(sp+i),(caddr_t)b,32))break;
        for(j=0;j<8;j++)sample[32+i/4+j]=G32(b+4*j);
        sample[12]+=32;
    }
}

/* UI_SET */
void
ui_patcharm(gp)
	struct guest_proc *gp;
{
	int i, shared = ui_patchshared(gp), reuse = 0;

    if(uin_coffdiag && ui_patchshared(gp) && (AUXP(gp)->ap_mac & APM_COFF)) {
        coffdiagowner=gp;coffdiaguntil=lbolt+15*HZ;coffdiagpolls=0;
        uin_coffcalls[0]=uin_coffcalls[1]=0;uin_coffclosed=uin_coffseen=0;
        for(i=0;i<8;i++)uin_cofflast[i]=0;
        uin_cofftracecount=uin_coffalertcount=0;
        bzero((caddr_t)uin_coffalert,sizeof uin_coffalert);
        bzero((caddr_t)uin_coffstartup,sizeof uin_coffstartup);
        bzero((caddr_t)uin_coffqdfirst,sizeof uin_coffqdfirst);
        bzero((caddr_t)uin_coff47,sizeof uin_coff47);
        uin_coffsyserrcount=0;bzero((caddr_t)uin_coffsyserr,sizeof uin_coffsyserr);
        uin_coffnatdrawcount=0;bzero((caddr_t)uin_coffnatdraw,sizeof uin_coffnatdraw);
        uin_cofftooltextcount=uin_cofftoolbitscount=uin_cofffontcount=uin_cofffontpostcount=0;
        uin_coffconvertcount=0;bzero((caddr_t)uin_coffconvert,sizeof uin_coffconvert);
        cofffontowner=0;cofffontphase=0;coffpixelready=0;uin_coffpixelscount=0;
        bzero((caddr_t)uin_coffpixels,sizeof uin_coffpixels);uin_coffviscount=0;
        bzero((caddr_t)uin_coffvis,sizeof uin_coffvis);uin_coffwincount=0;
        bzero((caddr_t)uin_coffwin,sizeof uin_coffwin);
        uin_coffviswritecount=0;bzero((caddr_t)uin_coffviswrite,sizeof uin_coffviswrite);
        bzero((caddr_t)uin_cofffontpost,sizeof uin_cofffontpost);
        bzero((caddr_t)uin_cofffont,sizeof uin_cofffont);
        bzero((caddr_t)uin_cofftooltext,sizeof uin_cofftooltext);
        bzero((caddr_t)uin_cofftoolbits,sizeof uin_cofftoolbits);
        bzero((caddr_t)uin_coffdraw,sizeof uin_coffdraw);
        bzero((caddr_t)uin_coffset,sizeof uin_coffset);
        uin_coffdrawcount=uin_coffsetcount=0;
        gp->gp_flags |= GPF_AWATCH;
    }
	gp->gp_flags &= ~((GPF_DONE(NPATCH) - GPF_DONE(0)) | GPF_STUB);
	if (!shared) {
		hfst = 0;
		trashbusy = 0;
		trashdir = spectab = 0;
	}
	if (shared && hfst)
		reuse = hfsreuse();
	for (i = 0; i < NPATCH; i++)
		if ((uipatches[i].p_kind == PGROUP && !uinter_fmgroup) ||
		    (shared && hfst && uipatches[i].p_kind == PHFS))
			gp->gp_flags |= GPF_DONE(i);
	if (uinter_trace)
		printf("uinter: patcharm shared %d hfst %x reuse %d\n", shared, (int)hfst, reuse);
	if (uinter_patches)
		gp->gp_flags |= GPF_AWATCH;
}

/*
 * run the stub (6 words, or _AUXDispatch(sel, arg)) below the stack, then
 * the trap at pc; ext bytes follow the frame. Path requests are preinitialized.
 */
static int
ui_auxcall(r, sel, arg, code, mark, ext)
	char *r;
	int sel;
	long arg;
	unsigned short *code;
	int mark, ext;
{
	unsigned char b[FSIZE];
	long sp = GR_USP(r), f = (sp - FSIZE - ext) & ~3L;
	int i;

	P32(b, 0);
	P16(b + 4, 0x3f3c);
	P16(b + 6, sel);
	P16(b + 8, 0x2f3c);
	P32(b + 10, arg);
	P16(b + 14, 0xabf9);
	for (i = 0; code && i < 6; i++)
		P16(b + 4 + 2 * i, code[i]);
	P16(b + 16, RESUME);
	P16(b + 18, mark);
	P32(b + 20, FMAGIC);
	bcopy(r + 4, (char *)b + 24, 60);
	P32(b + 84, GR_PC(r));
	P16(b + 88, GR_SR(r));
	P16(b + 90, 0);
	P32(b + 92, sp);
	if (copyout((caddr_t)b, (caddr_t)f, FSIZE))
		return 0;
	for (i = 0; mark != MPATH && i < ext; i += 4)
		if (copyout((caddr_t)b, (caddr_t)(f + FSIZE + i), 4))
			return 0;
	dlm_cacheflush();
	GR_USP(r) = f;
	GR_PC(r) = f + 4;
	return 1;
}

static void
pmexpected(code,redirect)
unsigned char *code,*redirect;
{
    int i;
    bcopy((caddr_t)pmcode,(caddr_t)code,60);
    P32(code+50,pmsite+8);P32(code+56,pmsite);
    P16(redirect,0x4ef9);P32(redirect+2,pmblock);
    for(i=6;i<16;i+=2)P16(redirect+i,0x4e71);
}

static int
pmstubvalid(gp)
struct guest_proc *gp;
{
    unsigned char code[60],redirect[16],b[60];
    if(pmowner!=gp || pmproc!=gp->gp_proc || pmstate!=2 ||
        !ui_macrange(gp,(unsigned long)pmblock,60))return 0;
    pmexpected(code,redirect);
    if(copyin((caddr_t)(pmsite-16),(caddr_t)b,24) ||
        bcmp((caddr_t)b,(caddr_t)redirect,16) || bcmp((caddr_t)b+16,(caddr_t)pmnative,8) ||
        copyin((caddr_t)pmblock,(caddr_t)b,60) || bcmp((caddr_t)b,(caddr_t)code,60))return 0;
    return 1;
}

static int
pmstubstart(gp,r)
struct guest_proc *gp;
char *r;
{
    unsigned short call[]={0x203c,0,60,0xa71e,0x4e71,0x4e71};
    if(pmstate || (gp->gp_flags&GPF_STUB) || !pmidleowned(gp,r))return 0;
    pmphaseseen=0;pmowner=gp;pmproc=gp->gp_proc;pmsite=GR_PC(r);pmsys=GR_A(r,5);pmstate=1;uin_pmidlerequests++;
    if(!ui_auxcall(r,0,0L,call,MPMIDLE,0))pmstate=3;
    return pmstate==1;
}

static void
pmstubrollback(gp)
struct guest_proc *gp;
{
    unsigned char code[60],redirect[16],b[24];
    pmexpected(code,redirect);
    if(copyin((caddr_t)(pmsite-16),(caddr_t)b,24) ||
        bcmp((caddr_t)b+16,(caddr_t)pmnative,8)) {psignal(gp->gp_proc,SIGKILL);return;}
    if(!bcmp((caddr_t)b,(caddr_t)pmprefix,16))return;
    if(bcmp((caddr_t)b,(caddr_t)redirect,16) ||
        copyout((caddr_t)pmprefix,(caddr_t)(pmsite-16),16) ||
        copyin((caddr_t)(pmsite-16),(caddr_t)b,16) || bcmp((caddr_t)b,(caddr_t)pmprefix,16))
        psignal(gp->gp_proc,SIGKILL);
    dlm_cacheflush();
}

static void
pmstubfinish(gp,r,block)
struct guest_proc *gp;
char *r;
long block;
{
    unsigned char code[60],redirect[16],b[60];
    if(pmowner!=gp || pmproc!=u.u_procp || pmstate!=1) {psignal(u.u_procp,SIGSEGV);return;}
    if(GR_PC(r)!=pmsite || GR_A(r,5)!=pmsys || !pmidleowned(gp,r) ||
        !ui_macrange(gp,(unsigned long)block,60)) {pmstate=3;return;}
    uin_pmidleallocations++;pmblock=block;pmexpected(code,redirect);
    if(copyout((caddr_t)code,(caddr_t)pmblock,60) ||
        copyin((caddr_t)pmblock,(caddr_t)b,60) || bcmp((caddr_t)b,(caddr_t)code,60))
        {pmstate=3;return;}
    dlm_cacheflush();
    if(!pmidleowned(gp,r) || copyin((caddr_t)(pmsite-16),(caddr_t)b,24) ||
        bcmp((caddr_t)b,(caddr_t)pmprefix,16) || bcmp((caddr_t)b+16,(caddr_t)pmnative,8)) {pmstate=3;return;}
    if(copyout((caddr_t)redirect,(caddr_t)(pmsite-16),16) ||
        copyin((caddr_t)(pmsite-16),(caddr_t)b,24) || bcmp((caddr_t)b,(caddr_t)redirect,16) ||
        bcmp((caddr_t)b+16,(caddr_t)pmnative,8) ||
        copyin((caddr_t)pmblock,(caddr_t)b,60) || bcmp((caddr_t)b,(caddr_t)code,60)) {
        pmstubrollback(gp);pmstate=3;return;
    }
    dlm_cacheflush();pmstate=2;
}

static int
pmstubpoll(gp,r)
struct guest_proc *gp;
char *r;
{
    unsigned char saved[62],b[4];char native[72];long sp=GR_USP(r),*phase=0;
    unsigned long fp;
    if(!uin_pmidle || !uin_pmidlestub || !pmstubvalid(gp) ||
        GR_PC(r)!=pmblock+0x16 || !ui_macrange(gp,(unsigned long)sp,74) ||
        copyin((caddr_t)(sp+12),(caddr_t)saved,62))return EINVAL;
    bcopy(r,native,sizeof native);bcopy((caddr_t)saved+2,native+4,60);
    GR_SR(native)=(GR_SR(native)&~0x1f)|(G16(saved)&0x1f);
    GR_PC(native)=pmsite;GR_USP(native)=sp+74;
    if(!pmidleframe(gp,native,1))return EINVAL;
    uin_pmidlebranches[(GR_D(native,7)&0xffff)==1]++;
    if(gp->gp_flags&GPF_AWATCH)uin_pmidlewatchedcalls++;
    if(!uin_pmphaseon)pmphaseseen=0;
    else if(uin_pmphasecount>=0 && uin_pmphasecount<8) {
        phase=uin_pmphase[uin_pmphasecount];
        {long *w=uin_pmwatch[uin_pmphasecount];struct uitask *t=ui_task(gp);
         w[0]=AUXP(gp)->ap_mac;w[1]=gp->gp_flags;
         w[2]=((GPF_DONE(NPATCH)-GPF_DONE(0))&~gp->gp_flags)>>16;
         w[3]=ui_forceget();w[4]=t ? t->t_force : -1;
         w[5]=uin_coffdiag && gp==coffdiagowner;
         w[6]=uinter_nalwatch;w[7]=uin_watchskipped;
        }
        phase[0]=pmphaseseen ? pmphasewake : -1;phase[1]=lbolt;
        phase[3]=pmsite;phase[4]=GR_A(native,6);phase[5]=-1;
        fp=(unsigned long)GR_A(native,6);
        if(fp<=0xffffffffUL-4 && ui_macrange(gp,fp+4,4) &&
            !copyin((caddr_t)(fp+4),(caddr_t)b,4))phase[5]=G32(b);
        phase[6]=GR_USP(native);phase[7]=GR_A(native,5);
        phase[8]=u.u_procp->p_cursig;
        phase[12]=copyin((caddr_t)0x16a,(caddr_t)b,4) ? -1 : G32(b);
        phase[14]=gp->gp_vsr;phase[15]=u.u_procp->p_hold;
    }
    uin_pmidlecalls++;ui_pmidlewait(gp);
    if(phase) {
        phase[2]=pmphasewake=lbolt;pmphaseseen=1;
        phase[9]=u.u_procp->p_cursig;phase[10]=u.u_procp->p_sig;
        phase[11]=gp->gp_flags;
        phase[13]=copyin((caddr_t)0x16a,(caddr_t)b,4) ? -1 : G32(b);
        uin_pmphasecount++;
    }
    return 0;
}

long uin_pmidleclearerror;

int
ui_pmidleclear(gp)
struct guest_proc *gp;
{
    unsigned char code[60],redirect[16],b[24];int error=0;
    if(pmowner!=gp)return 0;
    if(pmstate==2) {
        if(pmproc!=gp->gp_proc || pmproc!=u.u_procp)error=1;
        else {
            pmexpected(code,redirect);
            if(copyin((caddr_t)(pmsite-16),(caddr_t)b,24))error=2;
            else if(bcmp((caddr_t)b+16,(caddr_t)pmnative,8))error=3;
            else if(!bcmp((caddr_t)b,(caddr_t)pmprefix,16)) {
                /* The native entry can outlive its allocated forwarding stub. */
            } else if(bcmp((caddr_t)b,(caddr_t)redirect,16))error=3;
            else if(copyout((caddr_t)pmprefix,(caddr_t)(pmsite-16),16) ||
                copyin((caddr_t)(pmsite-16),(caddr_t)b,16) || bcmp((caddr_t)b,(caddr_t)pmprefix,16))error=4;
            else uin_pmidlerestores++;
        }
    }
    uin_pmidleclearerror=error;
    if(error)return error;
    dlm_cacheflush();
    pmowner=0;pmproc=0;pmblock=0;pmsite=0;pmsys=0;pmstate=0;
    return 0;
}

int
ui_pmidleclose(gp)
struct guest_proc *gp;
{
    int error;
    if(pmowner!=gp)return 0;
    if((error=ui_pmidleclear(gp))!=0)return error;
    pmowner=gp;pmproc=gp->gp_proc;pmstate=3;
    return 0;
}

void
ui_pmidleforget(gp)
struct guest_proc *gp;
{
    if(pmowner!=gp)return;
    pmowner=0;pmproc=0;pmblock=0;pmsite=0;pmsys=0;pmstate=0;
}

#define MCOFFSPACE 26
#define SPACEENT 0x594
#define SPACETRAP 0xaffb
static long spaceblock,spacepred;
static struct guest_proc *spacesession,*spacepending;
static struct proc *spaceproc,*spacependingproc;
static int spacestate;
long uin_coffspacecalls,uin_coffspaceforward,uin_coffspacealloc,uin_coffspacerefused;

static void
spacecode(b)
unsigned char *b;
{
    P16(b,SPACETRAP);P16(b+2,0x4e75);P16(b+4,0x4ef9);P32(b+6,spacepred);
}

static int
spacevalid()
{
    unsigned char b[10],expected[10];
    if(spacestate!=2 || spacesession!=ui.l_gp || spaceproc!=ui.l_proc ||
        !ui_macrange(GUESTP(u.u_procp),(unsigned long)spaceblock,10))return 0;
    spacecode(expected);
    return !copyin((caddr_t)spaceblock,(caddr_t)b,10) &&
        !bcmp((caddr_t)b,(caddr_t)expected,10);
}

static int
spacewatch(gp)
struct guest_proc *gp;
{
    struct uitask *t=ui_task(gp);
    return t && gp->gp_proc==u.u_procp && t->t_proc==gp->gp_proc && spacevalid();
}

/* A growable Unix stack is bounded by its process limit, not the Mac heap. */
static int
spacesize(p,sp,limit,available)
struct proc *p;
unsigned long sp,limit,*available;
{
    unsigned long bottom=(unsigned long)p->p_stkbase,bytes,top;
    if(p->p_stksize<=0)return 0;
    bytes=(unsigned long)p->p_stksize;
    if(bottom>0xffffffffUL-bytes)return 0;
    top=bottom+bytes;
    if(sp<bottom || sp>=top)return 0;
    if(limit<bytes)limit=bytes;
    if(limit>0x7fffffffUL || limit>top)return 0;
    bottom=top-limit;
    *available=sp-bottom>32 ? sp-bottom-32 : 0;
    return 1;
}

static int
spacestart(gp,r)
struct guest_proc *gp;
char *r;
{
    unsigned short call[]={0x203c,0,10,0xa71e,0x4e71,0x4e71};
    unsigned char b[4];
    if(spacevalid())return 0;
    if(spacestate || (gp->gp_flags&GPF_STUB) ||
        copyin((caddr_t)SPACEENT,(caddr_t)b,4))return -1;
    spacepred=G32(b);
    if((spacepred&1) || !(ui_macrange(gp,(unsigned long)spacepred,2) ||
        (spacepred>=0x40800000L && spacepred<0x40900000L)))return -1;
    spacesession=ui.l_gp;spaceproc=ui.l_proc;spacepending=gp;
    spacependingproc=gp->gp_proc;spacestate=1;
    if(!ui_auxcall(r,0,0L,call,MCOFFSPACE,0)) {spacestate=3;return -1;}
    gp->gp_flags|=GPF_STUB;return 1;
}

static void
spacefinish(gp,block,error)
struct guest_proc *gp;
long block;
int error;
{
    unsigned char b[10],expected[10],world[16];struct uitask *t;
    if(spacestate!=1 || gp!=spacepending || gp->gp_proc!=spacependingproc ||
        u.u_procp!=spacependingproc || spacesession!=ui.l_gp ||
        spaceproc!=ui.l_proc || !ui_coffworld(gp,world) || error ||
        !ui_macrange(gp,(unsigned long)block,10) ||
        copyin((caddr_t)SPACEENT,(caddr_t)b,4) || G32(b)!=spacepred)goto refused;
    spacecode(expected);
    if(copyout((caddr_t)expected,(caddr_t)block,10) ||
        copyin((caddr_t)block,(caddr_t)b,10) || bcmp((caddr_t)b,(caddr_t)expected,10))goto refused;
    dlm_cacheflush();P32(b,block);
    if(copyout((caddr_t)b,(caddr_t)SPACEENT,4) ||
        copyin((caddr_t)SPACEENT,(caddr_t)b,4) || G32(b)!=block) {
        if(!copyin((caddr_t)SPACEENT,(caddr_t)b,4) && G32(b)==block) {
            P32(b,spacepred);
            if(copyout((caddr_t)b,(caddr_t)SPACEENT,4) ||
                copyin((caddr_t)SPACEENT,(caddr_t)b,4) || G32(b)!=spacepred)
                psignal(spaceproc,SIGKILL);
        }
        goto refused;
    }
    spaceblock=block;spacestate=2;spacepending=0;spacependingproc=0;
    for(t=ui.l_task;t<ui.l_task+UI_NTASK;t++)
        if(t->t_gp && t->t_proc==t->t_gp->gp_proc)t->t_gp->gp_flags|=GPF_AWATCH;
    uin_coffspacealloc++;dlm_cacheflush();return;
refused:
    spacestate=3;uin_coffspacerefused++;psignal(gp->gp_proc,SIGSEGV);
}

static int
spacecall(gp,r,bound)
struct guest_proc *gp;
char *r;
int bound;
{
    unsigned long available;unsigned char memerr[2]={0,0};
    if(GR_PC(r)!=spaceblock || !spacevalid())return 0;
    if(bound && gp->gp_proc==u.u_procp && ui_task(gp) &&
        (AUXP(gp)->ap_mac&APM_COFF) &&
        spacesize(u.u_procp,(unsigned long)GR_USP(r),
            (unsigned long)u.u_rlimit[RLIMIT_STACK].rlim_cur,&available) &&
        !copyout((caddr_t)memerr,(caddr_t)0x220,2)) {
        GR_D(r,0)=available;GR_SR(r)=(GR_SR(r)&~0xf)|4;
        GR_PC(r)=spaceblock+2;uin_coffspacecalls++;
    } else {GR_PC(r)=spaceblock+4;uin_coffspaceforward++;}
    return 1;
}

/* Later native wrappers may retain the block until the session heap is reclaimed. */
int
ui_stackspaceclear(gp)
struct guest_proc *gp;
{
    unsigned char b[4];int error=0;
    if(gp==spacepending && gp!=spacesession && spacestate!=2) {
        spaceblock=spacepred=0;spacestate=0;
        spacesession=spacepending=0;spaceproc=spacependingproc=0;return 0;
    }
    if(gp!=spacesession)return 0;
    if(spacestate==2 && copyin((caddr_t)SPACEENT,(caddr_t)b,4))error=1;
    else if(spacestate==2 && G32(b)==spaceblock) {
        if(!spacevalid())error=1;
        else {P32(b,spacepred);if(copyout((caddr_t)b,(caddr_t)SPACEENT,4) ||
            copyin((caddr_t)SPACEENT,(caddr_t)b,4) || G32(b)!=spacepred)error=1;}
    }
    dlm_cacheflush();spaceblock=spacepred=0;spacestate=0;
    spacesession=spacepending=0;spaceproc=spacependingproc=0;
    return error;
}

#define MCOFFFRONT 21
#define MCOFFAUXFRONT 23
long uin_cofflaunch,uin_cofffront,uin_coffexit,uin_coffrefused;

static void
coff47targets(offset)
int offset;
{
    static long slots[4]={0x11f4L,0x14d0L,0x15a8L,0xeb8L};
    unsigned char b[4];int i;
    for(i=0;i<4;i++)uin_coff47[offset+i]=
        copyin((caddr_t)slots[i],(caddr_t)b,4)?-1:G32(b);
}

/* The joined child uses the carrier's registered native world. */
static int
coffauxadapt(gp,r,world,version)
struct guest_proc *gp;
char *r,*world;
unsigned short version;
{
    static unsigned char auxlaunch[] = {0x2f,0x42,0x00,0x04,0x2e,0x8d,0x59,0x8f,0x3f,0x3c,0x00,0x2f,0x42,0xa7,0x2a,0x78,0x09,0x04,0xab,0xf9,0x24,0x2f,0x00,0x08,0x2a,0x6f,0x00,0x04,0x20,0x1f};
    static unsigned char auxfront[] = {0x2f,0x42,0x00,0x04,0x2e,0x8d,0x59,0x8f,0x3f,0x3c,0x00,0x2e,0x42,0xa7,0x2a,0x78,0x09,0x04,0xab,0xf9,0x20,0x1f};
    static unsigned short foreground[] = {0x4267,0x486f,0x0062,0x3f3c,59,0xa88f};
    unsigned char code[30],frame[10],low[4],entry[8],result[4];
    long pc=GR_PC(r),sp=GR_USP(r),at,base;
    int front=pc==0x47e03c0eL,n=front ? sizeof auxfront : sizeof auxlaunch,error=0;
    at=front ? 0x47e03bfcL : 0x47e03bdeL;
    if(copyin((caddr_t)at,(caddr_t)code,n) ||
        bcmp((char *)code,(char *)(front ? auxfront : auxlaunch),n) ||
        copyin((caddr_t)sp,(caddr_t)frame,sizeof frame) ||
        G16(frame+4)!=(front ? 46 : 47)) {uin_coffrefused++;return 0;}
    if(G32(frame) || copyin((caddr_t)0x904,(caddr_t)low,4) ||
        G32(low)!=G32(world+12) || GR_A(r,5)!=G32(world+12))error=-50;
    if(!error && uinter_coffnative47) {
        if(!front) {
            uin_coff47[0]=pc;uin_coff47[1]=sp;uin_coff47[2]=GR_A(r,5);
            uin_coff47[3]++;coff47targets(4);uin_cofflaunch++;
            coffsyserrparent=ui.l_gp;coffsyserrproc=ui.l_proc;
            uin_coffsyserrparentpid=ui.l_proc->p_pid;coffsyserrversion=version;
            return 0;
        }
        coff47targets(8);uin_coff47[12]=pc;uin_coff47[13]=sp;
        uin_coff47[14]=G32(frame+6);
    }
    if(front && !error && uinter_coffnative47) {
        int started=spacestart(gp,r);
        if(started>0)return 1;
        if(started<0) {uin_coffspacerefused++;psignal(gp->gp_proc,SIGSEGV);return 1;}
    }
    if(front && !error) {
        if(copyin((caddr_t)0xb7c,(caddr_t)low,4))error=-50;
        else {
            base=G32(low)-(version==0x0761 ? 3330 : 3308);
            if(!ui_macrange(gp,(unsigned long)base,60*8) ||
                copyin((caddr_t)(base+59*8),(caddr_t)entry,8) ||
                !G32(entry) || (G32(entry)&1) || G16(entry+4)!=4 || G16(entry+6)!=2)
                error=-50;
        }
    }
    if(!front || error) {
        P32(result,error);
        if(copyout((caddr_t)result,(caddr_t)(sp+6),4)) {
            uin_coffrefused++;psignal(gp->gp_proc,SIGSEGV);return 1;
        }
        GR_PC(r)=pc+2;GR_USP(r)=sp+6;
        if(error)uin_coffrefused++;else uin_cofflaunch++;
        return 1;
    }
    if(!ui_auxcall(r,0,0L,foreground,MCOFFAUXFRONT,8) ||
        copyout(world+4,(caddr_t)(GR_USP(r)+FSIZE),8)) {
        GR_PC(r)=pc;GR_USP(r)=sp;uin_coffrefused++;
        psignal(gp->gp_proc,SIGSEGV);return 1;
    }
    gp->gp_flags|=GPF_STUB;uin_cofffront++;return 1;
}

/* Legacy bridges consume their own frames while the carrier owns native state. */
static int
coffadapt(gp,r,tw,world)
struct guest_proc *gp;
char *r,*world;
unsigned short tw;
{
    static unsigned char launch[] = {
        0x48,0xe7,0x20,0x04,0x2a,0x79,0,0,9,4,0x42,0x67,
        0x2f,0x2f,0,0x0e,0x2f,0x2f,0,0x16,0x2f,0x2f,0,0x1e,
        0x3f,0x2f,0,0x28,0x2f,0x2f,0,0x2c,0x3f,0x2f,0,0x36,
        0xa8,0x8f,0x30,0x1f,0x48,0xc0,0x4c,0xdf,0x20,0x04,0x4e,0x75
    };
    static unsigned char front[] = {
        0x48,0xe7,0x20,0x04,0x2a,0x79,0,0,9,4,0x42,0xa7,
        0x3f,0x2f,0,0x12,0x3f,0x3c,0,0x11,0xa8,0x8f,
        0x20,0x1f,0x4c,0xdf,0x20,0x04,0x4e,0x75
    };
    static unsigned short foreground[] = {0x4267,0x486f,0x0062,0x3f3c,59,0xa88f};
    static unsigned short unixexit[] = {0x42a7,0x42a7,0x7001,0x4e40,0x60fe,0x4e71};
    unsigned char code[48],frame[22],low[4],entry[8],version[2],result[4];
    long pc=GR_PC(r),sp=GR_USP(r),at,base;
    int which,n,error=0;
    if(copyin((caddr_t)0x15a,(caddr_t)version,2) ||
        (G16(version)!=0x0761 && G16(version)!=0x0810)) return 0;
    if(tw==0xa9f4) {
        if(copyin((caddr_t)0x904,(caddr_t)low,4) || G32(low)!=G32(world+12)) {
            uin_coffrefused++;return 0;
        }
        if(!ui_auxcall(r,0,0L,unixexit,0,0)) {
            uin_coffrefused++;psignal(gp->gp_proc,SIGSEGV);
        } else uin_coffexit++;
        return 1;
    }
    if(tw==0xabf9 && (pc==0x47e03bf0L || pc==0x47e03c0eL))
        return coffauxadapt(gp,r,world,G16(version));
    if(tw!=0xa88f || (pc!=0x47e061beL && pc!=0x47e095f2L)) return 0;
    which=pc==0x47e095f2L;at=which ? 0x47e095deL : 0x47e0619aL;
    n=which ? sizeof front : sizeof launch;
    if(copyin((caddr_t)at,(caddr_t)code,n) ||
        bcmp((char *)code,(char *)(which ? front : launch),n) ||
        copyin((caddr_t)sp,(caddr_t)frame,which ? 8 : 22) ||
        G16(frame)!=(which ? 17 : 55)) {uin_coffrefused++;return 0;}
    if(copyin((caddr_t)0x904,(caddr_t)low,4) || G32(low)!=G32(world+12) ||
        GR_A(r,5)!=G32(world+12)) error=-50;
    if(!which) {
        if(G32(frame+2) || G32(frame+12) || G32(frame+16)) error=-50;
        P16(result,error ? error : G32(world));
        if(copyout((caddr_t)result,(caddr_t)(sp+20),2)) {
            uin_coffrefused++;psignal(gp->gp_proc,SIGSEGV);return 1;
        }
        GR_PC(r)=pc+2;GR_USP(r)=sp+20;
        if(error)uin_coffrefused++;else uin_cofflaunch++;
        return 1;
    }
    if(G16(frame+2)!=G32(world)) error=-50;
    if(!error) {
        if(copyin((caddr_t)0xb7c,(caddr_t)low,4))error=-50;
        else {
            base=G32(low)-(G16(version)==0x0761 ? 3330 : 3308);
            if(!ui_macrange(gp,(unsigned long)base,60*8) ||
                copyin((caddr_t)(base+59*8),(caddr_t)entry,8) ||
                !G32(entry) || (G32(entry)&1) || G16(entry+4)!=4 || G16(entry+6)!=2)
                error=-50;
        }
    }
    if(error) {
        P32(result,error);
        if(copyout((caddr_t)result,(caddr_t)(sp+4),4))psignal(gp->gp_proc,SIGSEGV);
        GR_PC(r)=pc+2;GR_USP(r)=sp+4;uin_coffrefused++;return 1;
    }
    if(!ui_auxcall(r,0,0L,foreground,MCOFFFRONT,8) ||
        copyout(world+4,(caddr_t)(GR_USP(r)+FSIZE),8)) {
        GR_PC(r)=pc;GR_USP(r)=sp;uin_coffrefused++;
        psignal(gp->gp_proc,SIGSEGV);return 1;
    }
    gp->gp_flags|=GPF_STUB;uin_cofffront++;return 1;
}

static unsigned char aslmreader[] = {
    0x59,0x4f,0x2f,0x03,0x3f,0x2e,0x00,0x16,
    0xa8,0x0e,0x20,0x1f,0x2d,0x40,0xfe,0xf8
};
static unsigned char aslmloader[] = {
    0x59,0x4f,0x2f,0x2e,0x00,0x16,0x3f,0x2e,0x00,0x14,0xa8,0x1f,0x26,0x5f
};
static unsigned short aslmcall[] = { 0xa80e,0x4e71,0x4e71,0x4e71,0x4e71,0x4e71 };
static int
aslmwrap(r)
    char *r;
{
    unsigned char code[sizeof aslmreader],args[10];
    unsigned short call[6],trap;
    long pc=GR_PC(r),sp=GR_USP(r),f,type;
    int i,match=1;
    if(!uinter_aslmtrace || pc<10 ||
        copyin((caddr_t)pc,(caddr_t)code,2) ||
        copyin((caddr_t)sp,(caddr_t)args,sizeof args))return 0;
    trap=G16(code);type=G32(args+2);
    if(type==0x6c696272L) {
        if(trap!=0xa80e || uin_aslmreads>=256)return 0;
    } else if(type==0x61583031L) {
        if(trap!=0xa81f || uin_aslmcode>=64)return 0;
    } else return 0;
    if(copyin((caddr_t)(pc-8),(caddr_t)code,sizeof aslmreader))return 0;
    for(i=0;i<sizeof aslmreader;i++)
        if(code[i]!=(i==9 ? (trap&255) : aslmreader[i]))match=0;
    if(!match) {
        if(trap!=0xa81f || copyin((caddr_t)(pc-10),(caddr_t)code,sizeof aslmloader))return 0;
        for(i=0;i<sizeof aslmloader;i++)if(code[i]!=aslmloader[i])return 0;
    }
    for(i=0;i<6;i++)call[i]=aslmcall[i];call[0]=trap;
    if(copyout((caddr_t)args+6,(caddr_t)sp+6,4) ||
        !ui_auxcall(r,0,0L,call,MASLM,0))return 0;
    f=GR_USP(r);
    if(copyout((caddr_t)args,(caddr_t)(f-6),6)) {
        GR_USP(r)=sp;GR_PC(r)=pc;return 0;
    }
    GR_USP(r)=f-6;
    return 1;
}
static unsigned char aslmclass[] = {
    0xdd,0xae,0xff,0xc4,0x7c,0x00,0x20,0x6e,0xff,0xc4,0x28,0x10,0x10,0x2e,0xff,0xbc
};
static unsigned char aslmexit[] = {
    0x30,0x2e,0xff,0x72,0x4c,0xdf,0x1c,0xf8,0x4e,0x5e,0x20,0x5f,0x58,0x4f,0x4e,0xd0
};
static unsigned char aslmcaller[] = {
    0x4f,0xef,0x00,0x10,0x2d,0x40,0xff,0x60,0x0c,0x6e,0xf3,0xbb,0xff,0x72,0x66,0x00
};
static unsigned char aslmload[] = {
    0x76,0x00,0x2d,0x43,0xff,0xf8,0x2d,0x43,0xff,0xfc,0x20,0x2e,0x00,0x0c,0x66,0x08
};
static unsigned char aslmloadexit[] = {
    0x30,0x04,0x4c,0xdf,0x1c,0x18,0x4e,0x5e,0x4e,0x75,0x4e,0x56,0xff,0xf8,0x48,0xe7
};
static unsigned char aslmget[] = {
    0x28,0x6e,0x00,0x08,0x42,0x45,0x70,0x00,0x24,0x40,0x70,0x00,0x2d,0x40,0xff,0xb4
};
static unsigned char aslmgetexit[] = {
    0x20,0x0a,0x4c,0xdf,0x1c,0x28,0x4e,0x5e,0x20,0x5f,0x4f,0xef,0x00,0x14,0x4e,0xd0
};
static unsigned char aslmmp[] = {
    0x24,0x6e,0x00,0x18,0x20,0x0a,0x67,0x06,0x26,0x6a,0x00,0x08,0x60,0x04,0x26,0x6e
};
static unsigned char aslmmpcall[] = {
    0x24,0x40,0x2f,0x0a,0x20,0x52,0x20,0x68,0x00,0x4c,0x4e,0x90,0x4f,0xef,0x00,0x10
};
static unsigned char aslmmpreturn[] = {
    0x4f,0xef,0x00,0x10,0x24,0x5f,0x4e,0x5e,0x4e,0x75,0x4e,0x56,0x00,0x00,0x2f,0x0a
};
static long aslmclasspc,aslmexitpc,aslmloadpc,aslmloadexitpc,aslmgetpc,aslmgetexitpc;
static unsigned char aslmmpinstall[] = {
    0x24,0x40,0x20,0x0a,0x4c,0xdf,0x0c,0x00,0x4e,0x5e,0x4e,0x74,0x00,0x08,0x9c,0x47
};
struct aslmmpcopy {long base,pc,loadpc,exitpc,manager,installpc;};
static struct aslmmpcopy aslmmpcopies[4];
static int aslmmpcount;
static long aslmgets,aslmauxgets,aslmmpcalls,aslmsetupreturns,aslminstallreturns;
static long aslmdiagptr[3],aslmdiagsamples;
static unsigned char aslmdiaglast[3][896];
static struct guest_proc *aslmowner;
static long aslmclasses,aslmexits,aslmloads,aslmloadexits;
static long aslmparentpc;
static unsigned int aslmfailseen;

static struct guest_proc *otprobeowner;
static struct proc *otprobeproc;
static unsigned int otprobepid,otprobeentry,otprobedesc,otprobectx,otprobea5;
long uin_otprobe[2][192];
int uin_otprobecount;

static int
aslmsite(pc,signature,patched)
    long pc;
    unsigned char *signature;
    int patched;
{
    unsigned char b[16];int i;
    if(pc<2 || pc>0x7fffffefL || copyin((caddr_t)pc,(caddr_t)b,sizeof b))return 0;
    if(G16(b)!=(patched ? RESUME : G16(signature)))return 0;
    for(i=2;i<sizeof b;i++)if(b[i]!=signature[i])return 0;
    return 1;
}

static int
aslmrestore(pc,signature)
    long pc;
    unsigned char *signature;
{
    if(!pc || aslmsite(pc,signature,0))return 1;
    return aslmsite(pc,signature,1) &&
        !copyout((caddr_t)signature,(caddr_t)pc,2);
}

void
ui_aslmclear(gp)
    struct guest_proc *gp;
{
    int restored,i;
    if(gp==otprobeowner) {otprobeowner=0;otprobeproc=0;}
    if(gp!=aslmowner)return;
    restored=gp->gp_proc==u.u_procp;
    if(restored) {
        if(!aslmrestore(aslmclasspc,aslmclass))restored=0;
        if(!aslmrestore(aslmexitpc,aslmexit))restored=0;
        if(!aslmrestore(aslmloadpc,aslmload))restored=0;
        if(!aslmrestore(aslmloadexitpc,aslmloadexit))restored=0;
        if(!aslmrestore(aslmgetpc,aslmget))restored=0;
        if(!aslmrestore(aslmgetexitpc,aslmgetexit))restored=0;
        for(i=0;i<aslmmpcount;i++) {
            if(!aslmrestore(aslmmpcopies[i].pc,aslmmp))restored=0;
            if(!aslmrestore(aslmmpcopies[i].installpc,aslmmpinstall))restored=0;
            if(!aslmrestore(aslmmpcopies[i].loadpc,aslmload))restored=0;
            if(!aslmrestore(aslmmpcopies[i].exitpc,aslmloadexit))restored=0;
        }
        dlm_cacheflush();
    }
    aslmclasspc=aslmexitpc=aslmloadpc=aslmloadexitpc=aslmgetpc=aslmgetexitpc=0;aslmmpcount=0;aslmowner=0;
    if(restored) {
        uin_aslmreads=uin_aslmcode=aslmclasses=aslmexits=aslmparentpc=0;
        aslmloads=aslmloadexits=aslmgets=aslmauxgets=aslmmpcalls=aslmsetupreturns=aslminstallreturns=0;
        aslmdiagptr[0]=aslmdiagptr[1]=aslmdiagptr[2]=aslmdiagsamples=0;
        aslmfailseen=0;
    }
}

static void
aslminstall(caller)
    long caller;
{
    unsigned char b[16],word[2];long c,e,l,x,g,y;int i,ok=1;
    if(!uinter_aslmtrace || aslmowner || !ui.l_gp || caller<0x12c28 || caller>0x7fffff9fL ||
        copyin((caddr_t)caller,(caddr_t)b,sizeof b))return;
    for(i=0;i<sizeof b;i++)if(b[i]!=aslmcaller[i])return;
    c=caller-0x12c28+0x12912;e=caller-0x12c28+0x12c78;
    l=caller-0x12c28+0xf572;x=caller-0x12c28+0xf646;g=caller-0x12c28+0xf958;y=caller-0x12c28+0xfaa8;
    if(!aslmsite(c,aslmclass,0) || !aslmsite(e,aslmexit,0) ||
        !aslmsite(l,aslmload,0) || !aslmsite(x,aslmloadexit,0) || !aslmsite(g,aslmget,0) || !aslmsite(y,aslmgetexit,0))return;
    P16(word,RESUME);
    /* Retain ownership until guarded words are restored or their RAM is freed. */
    aslmowner=ui.l_gp;aslmclasspc=c;aslmexitpc=e;aslmloadpc=l;aslmloadexitpc=x;aslmgetpc=g;aslmgetexitpc=y;
    if(copyout((caddr_t)word,(caddr_t)c,2) || copyout((caddr_t)word,(caddr_t)e,2) ||
        copyout((caddr_t)word,(caddr_t)l,2) || copyout((caddr_t)word,(caddr_t)x,2) ||
        copyout((caddr_t)word,(caddr_t)g,2) || copyout((caddr_t)word,(caddr_t)y,2)) {
        if(!aslmrestore(c,aslmclass))ok=0;
        if(!aslmrestore(e,aslmexit))ok=0;
        if(!aslmrestore(l,aslmload))ok=0;
        if(!aslmrestore(x,aslmloadexit))ok=0;
        if(!aslmrestore(g,aslmget))ok=0;
        if(!aslmrestore(y,aslmgetexit))ok=0;
        if(ok) {aslmclasspc=aslmexitpc=aslmloadpc=aslmloadexitpc=aslmgetpc=aslmgetexitpc=0;aslmowner=0;}
        else psignal(u.u_procp,SIGSEGV);
        dlm_cacheflush();return;
    }
    uin_aslmreads=uin_aslmcode=aslmclasses=aslmexits=aslmparentpc=0;
    aslmloads=aslmloadexits=aslmgets=aslmauxgets=aslmmpcalls=aslmsetupreturns=aslminstallreturns=0;
        aslmdiagptr[0]=aslmdiagptr[1]=aslmdiagptr[2]=aslmdiagsamples=0;aslmfailseen=0;
    dlm_cacheflush();
    printf("uinter: ASLM parser class %x exit %x load %x return %x\n",(int)c,(int)e,(int)l,(int)x);
}

static int
aslmstep(gp,r)
    struct guest_proc *gp;
    char *r;
{
    unsigned short call[6];unsigned char previous[2];
    unsigned char *signature=0;long pc=GR_PC(r),sp=GR_USP(r),f;int i,mark=0;
    if(!aslmowner || aslmowner!=ui.l_gp)return 0;
    if(pc==aslmclasspc) {signature=aslmclass;mark=MCLASS;}
    else if(pc==aslmexitpc) {signature=aslmexit;mark=MREGEXIT;}
    else if(pc==aslmloadpc) {signature=aslmload;mark=MLOAD;}
    else if(pc==aslmloadexitpc) {signature=aslmloadexit;mark=MLOADEXIT;}
    else if(pc==aslmgetpc) {signature=aslmget;mark=MGET;}
    else if(pc==aslmgetexitpc) {signature=aslmgetexit;mark=MGETEXIT;}
    else {
        for(i=0;i<aslmmpcount;i++) {
            if(pc==aslmmpcopies[i].pc) {signature=aslmmp;mark=MMPSLOAD;break;}
            if(pc==aslmmpcopies[i].installpc) {signature=aslmmpinstall;mark=MGINSTALL;break;}
            if(pc==aslmmpcopies[i].loadpc) {signature=aslmload;mark=MLOAD;break;}
            if(pc==aslmmpcopies[i].exitpc) {signature=aslmloadexit;mark=MLOADEXIT;break;}
        }
        if(i==aslmmpcount)return 0;
    }
    if(!aslmsite(pc,signature,1)) {
        psignal(u.u_procp,SIGSEGV);return 1;
    }
    call[0]=G16(signature);call[1]=(mark==MLOAD || mark==MLOADEXIT || mark==MGETEXIT || mark==MGINSTALL) ? 0x4e71 : G16(signature+2);
    for(i=2;i<6;i++)call[i]=0x4e71;
    P16(previous,(gp->gp_flags&GPF_STUB)!=0);
    if(ui_auxcall(r,0,0L,call,mark,0)) {
        f=GR_USP(r);
        if(!copyout((caddr_t)previous,(caddr_t)(f+90),2)) {
            gp->gp_flags|=GPF_STUB;return 1;
        }
        GR_USP(r)=sp;GR_PC(r)=pc;
    }
    /* Execute the native word if the continuation cannot be copied. */
    if(copyout((caddr_t)signature,(caddr_t)pc,2))psignal(u.u_procp,SIGSEGV);
    else dlm_cacheflush();
    return 1;
}

static long
aslmpointer(address)
    long address;
{
    unsigned char b[4];
    if(address<2 || address>0x7ffffffbL || copyin((caddr_t)address,(caddr_t)b,4))return 0;
    return G32(b);
}

static long
aslmfield(address,offset)
    long address,offset;
{
    if(offset<0 || address<2 || address>0x7ffffffbL-offset)return 0;
    return aslmpointer(address+offset);
}

static long
aslmmpsmanager(base)
    long base;
{
    unsigned char b[16];long slot;
    if(base<2 || base>0x7ffda85bL ||
        copyin((caddr_t)(base+0x25794),(caddr_t)b,sizeof b))return 0;
    if(G16(b)!=0x2039 || G16(b+6)!=0x4e75 || G16(b+8)!=0x2039 || G16(b+14)!=0x660e)return 0;
    slot=G32(b+2);
    return aslmpointer(slot);
}

static void
aslmmpsinstall(r)
    char *r;
{
    unsigned char b[16],word[2];long fp=GR_A(r,6),parent,caller,base,manager,table,target,l,x,p;
    struct aslmmpcopy *copy;int i,ok=1,newclass;
    if(fp<2 || fp>0x7fffffcbL || aslmpointer(fp)!=fp+28)return;
    parent=fp+28;caller=aslmpointer(parent+4);
    if((caller&1) || caller<0x25724 || caller>0x7fffffebL || copyin((caddr_t)(caller-12),(caddr_t)b,16))return;
    for(i=0;i<16;i++)if(b[i]!=aslmmpcall[i])return;
    if(copyin((caddr_t)caller,(caddr_t)b,16))return;
    for(i=0;i<16;i++)if(b[i]!=aslmmpreturn[i])return;
    base=caller-0x25722;
    for(i=0;i<aslmmpcount;i++)if(aslmmpcopies[i].base==base)return;
    if(aslmmpcount>=4)return;
    manager=aslmmpsmanager(base);table=aslmpointer(manager);target=aslmfield(table,64);
    l=x=0;p=base+0x6d8;
    if(target>=2 && target<=0x7fffff0fL) {l=target+12;x=target+0xe0;}
    newclass=l && l!=aslmloadpc;
    for(i=0;i<aslmmpcount;i++)if(l==aslmmpcopies[i].loadpc)newclass=0;
    if(!aslmsite(p,aslmmp,0) || !aslmsite(base+0x9d8,aslmmpinstall,0))return;
    if(newclass && (!aslmsite(l,aslmload,0) || !aslmsite(x,aslmloadexit,0)))newclass=0;
    copy=&aslmmpcopies[aslmmpcount++];copy->base=base;copy->pc=p;
    copy->installpc=base+0x9d8;copy->loadpc=newclass ? l : 0;copy->exitpc=newclass ? x : 0;copy->manager=manager;
    P16(word,RESUME);
    if(copyout((caddr_t)word,(caddr_t)p,2) ||
        copyout((caddr_t)word,(caddr_t)copy->installpc,2) ||
        (newclass && (copyout((caddr_t)word,(caddr_t)l,2) || copyout((caddr_t)word,(caddr_t)x,2)))) {
        if(!aslmrestore(p,aslmmp))ok=0;
        if(!aslmrestore(copy->installpc,aslmmpinstall))ok=0;
        if(newclass && !aslmrestore(l,aslmload))ok=0;
        if(newclass && !aslmrestore(x,aslmloadexit))ok=0;
        if(ok)aslmmpcount--;
        else psignal(u.u_procp,SIGSEGV);
        dlm_cacheflush();return;
    }
    dlm_cacheflush();
    printf("uinter: ASLM MPS copy %x module %x manager %x classload %x new %d\n",
        (int)base,(int)p,(int)manager,(int)target,newclass);
}

static void
aslmowncache(slot,address)
    int slot;
    long address;
{
    unsigned char b[12];int i;
    if(slot<0 || slot>=3 || address<2 || address>0x7fffffbfL ||
        copyin((caddr_t)address,(caddr_t)b,sizeof b) ||
        G32(b)!=0x4f544447L || G32(b+4)!=1 || G32(b+8)!=(slot==0 ? 3 : slot))return;
    if(aslmdiagptr[slot]!=address) {
        aslmdiagptr[slot]=address;
        for(i=0;i<sizeof aslmdiaglast[slot];i++)aslmdiaglast[slot][i]=0;
    }
}
static void
aslmupstream(b,words)
    unsigned char *b;
    int words;
{
    unsigned char q[24],entry[4],code[32],state[112];long queue,info,put,private;int i;
    if(words!=44 || G32(b+164)==0 || G32(b+172)!=0 || G32(b+24)!=8)return;
    queue=G32(b+160);
    if(queue<2 || queue>0x7fffffe7L || copyin((caddr_t)queue,(caddr_t)q,sizeof q))return;
    info=G32(q);put=0;
    if(info>=2 && info<=0x7ffffffbL && !copyin((caddr_t)info,(caddr_t)entry,4))put=G32(entry);
    printf("uinter: ASLM upstream queue %x info %x next %x link %x private %x put %x\n",
        (int)queue,(int)info,(int)G32(q+12),(int)G32(q+16),(int)G32(q+20),(int)put);
    if(put<2 || put>0x7fffffdfL || copyin((caddr_t)put,(caddr_t)code,sizeof code))return;
    for(i=0;i<8;i+=4)printf("uinter: ASLM upstream put %x +%x %x %x %x %x\n",
        (int)put,i*4,(int)G32(code+i*4),(int)G32(code+i*4+4),
        (int)G32(code+i*4+8),(int)G32(code+i*4+12));
    if(G32(code)!=0x4e56ffdcL || G32(code+4)!=0x48e71c38L ||
        G32(code+8)!=0x206e0008L || G32(code+12)!=0x20280014L ||
        G32(code+16)!=0x2d40ffdcL || G32(code+20)!=0x246e000cL ||
        G32(code+24)!=0x206a0014L || G32(code+28)!=0x1428000dL)return;
    private=G32(q+20);
    if(private<2 || private>0x7fffff8fL ||
        copyin((caddr_t)private,(caddr_t)state,sizeof state))return;
    printf("uinter: ASLM upstream head %x +0 %x %x %x %x\n",(int)private,
        (int)G32(state),(int)G32(state+4),(int)G32(state+8),(int)G32(state+12));
    printf("uinter: ASLM upstream head %x +20 %x %x %x %x\n",(int)private,
        (int)G32(state+32),(int)G32(state+36),(int)G32(state+40),(int)G32(state+44));
    printf("uinter: ASLM upstream head %x +60 %x %x %x %x\n",(int)private,
        (int)G32(state+96),(int)G32(state+100),(int)G32(state+104),(int)G32(state+108));
}

static void
aslmowndiags()
{
    unsigned char b[896];static char *names[]={"config","tcp","udp"};int i,n,words;long address;
    if(aslmdiagsamples>=64)return;
    for(i=0;i<3;i++) {
        address=aslmdiagptr[i];
        for(n=0;n<sizeof b;n++)b[n]=0;
        if(address<2 || address>0x7fffffbfL || copyin((caddr_t)address,(caddr_t)b,64) ||
            G32(b)!=0x4f544447L || G32(b+4)!=1 || G32(b+8)!=(i==0 ? 3 : i))continue;
        words=16;
        if(i && address<=0x7fffff8fL &&
            !copyin((caddr_t)(address+64),(caddr_t)(b+64),8) &&
            G32(b+64)==0x4f545450L) {
            if(G32(b+68)==0x0001001cL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),40))words=28;
            else if(G32(b+68)==0x0002002cL && address<=0x7fffff4fL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),104))words=44;
            else if(G32(b+68)==0x00030058L && address<=0x7ffffe9fL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),280))words=88;
            else if(G32(b+68)==0x00040078L && address<=0x7ffffe1fL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),408))words=120;
            else if(G32(b+68)==0x000500a0L && address<=0x7ffffd7fL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),568))words=160;
            else if(G32(b+68)==0x000600b0L && address<=0x7ffffd3fL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),632))words=176;
            else if(G32(b+68)==0x000700c8L && address<=0x7ffffcdfL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),728))words=200;
            else if((G32(b+68)==0x000800e0L || G32(b+68)==0x000900e0L || G32(b+68)==0x000a00e0L) && address<=0x7ffffc7fL &&
                !copyin((caddr_t)(address+72),(caddr_t)(b+72),824))words=224;
        }
        for(n=words*4;n<sizeof b;n++)b[n]=0;
        for(n=0;n<sizeof b && b[n]==aslmdiaglast[i][n];n++);
        if(n==sizeof b)continue;
        for(n=0;n<sizeof b;n++)aslmdiaglast[i][n]=b[n];
        aslmdiagsamples++;
        for(n=0;n<16;n+=4)printf("uinter: ASLM own diagnostics %s %x +%x %x %x %x %x\n",
            names[i],(int)address,n*4,(int)G32(b+n*4),(int)G32(b+n*4+4),
            (int)G32(b+n*4+8),(int)G32(b+n*4+12));
        if(words>=28)for(n=16;n<words;n+=4)printf("uinter: ASLM own TPI %s %x +%x %x %x %x %x\n",
            names[i],(int)address,n*4,(int)G32(b+n*4),(int)G32(b+n*4+4),
            (int)G32(b+n*4+8),(int)G32(b+n*4+12));
        aslmupstream(b,words);
        if(aslmdiagsamples>=64)return;
    }
}

static void
aslmsteplog(r,mark,nativepc)
    char *r;
    int mark;
    long nativepc;
{
    unsigned char b[80],source[4],pointer[4],index[2];long fp=GR_A(r,6),address,src=0;int i;
    if(!copyin((caddr_t)(fp+8),(caddr_t)source,4))src=G32(source);
    if(mark==MGETEXIT) {
        unsigned char classname[80],procedure[80],code[32];long cp,pp,result=GR_D(r,0);int n,ownsetup,ownmodule;
        aslmowndiags();
        if(aslmsetupreturns>=16 || fp<2 || fp>0x7fffffe3L)return;
        cp=aslmpointer(fp+24);pp=aslmpointer(fp+20);
        if(cp<2 || cp>0x7fffffafL || pp<2 || pp>0x7fffffafL ||
            copyin((caddr_t)cp,(caddr_t)classname,sizeof classname) ||
            copyin((caddr_t)pp,(caddr_t)procedure,sizeof procedure))return;
        for(n=0;n<sizeof classname && classname[n];n++);
        if(n==sizeof classname)return;
        for(n=0;n<sizeof procedure && procedure[n];n++);
        if(n==sizeof procedure)return;
        ownsetup=!strcmp((char *)classname,"OTClnt$auxCfig") &&
            !strcmp((char *)procedure,"OTSetupConfigurator");
        ownmodule=(!strcmp((char *)classname,"OTModl$auxtcp") &&
            (!strcmp((char *)procedure,"GetOTInstallInfo") ||
             !strcmp((char *)procedure,"GetOTauxtcpInstallInfo"))) ||
            (!strcmp((char *)classname,"OTModl$auxudp") &&
            (!strcmp((char *)procedure,"GetOTInstallInfo") ||
             !strcmp((char *)procedure,"GetOTauxudpInstallInfo")));
        if(!ownsetup && !ownmodule)return;
        aslmsetupreturns++;
        printf("uinter: ASLM own procedure %s %s returned %x error %d\n",
            classname,procedure,(int)result,(short)GR_D(r,5));
        if(result<2 || result>0x7fffffdfL || copyin((caddr_t)result,(caddr_t)code,sizeof code))return;
        if(ownsetup && G16(code)==0x41f9 && G16(code+6)==0x4ef9) {
            aslmowncache(0,G32(code+2));aslmowndiags();
        }
        for(n=0;n<8;n+=4)printf("uinter: ASLM own setup code %x %x %x %x %x\n",
            (int)(result+4*n),(int)G32(code+4*n),(int)G32(code+4*n+4),
            (int)G32(code+4*n+8),(int)G32(code+4*n+12));
        return;
    }
    if(mark==MGET) {
        unsigned char classname[80],procedure[80];long cp,pp,manager;int n,observe,custom;
        if(fp<2 || fp>0x7fffffe3L)return;
        observe=aslmgets<16;
        if(aslmgets<128) {aslmgets++;aslmmpsinstall(r);}
        manager=aslmpointer(fp+8);cp=aslmpointer(fp+24);pp=aslmpointer(fp+20);
        classname[0]=procedure[0]=0;
        if(cp>=2 && cp<=0x7fffffafL && !copyin((caddr_t)cp,(caddr_t)classname,sizeof classname)) {
            for(n=0;n<sizeof classname && classname[n];n++);
            if(n==sizeof classname)classname[0]=0;
        } else classname[0]=0;
        if(pp>=2 && pp<=0x7fffffafL && !copyin((caddr_t)pp,(caddr_t)procedure,sizeof procedure)) {
            for(n=0;n<sizeof procedure && procedure[n];n++);
            if(n==sizeof procedure)procedure[0]=0;
        } else procedure[0]=0;
        custom=!strcmp((char *)classname,"OTModl$auxtcp") || !strcmp((char *)classname,"OTModl$auxudp") ||
            !strcmp((char *)procedure,"GetOTauxtcpInstallInfo") || !strcmp((char *)procedure,"GetOTauxudpInstallInfo");
        if(custom) {
            if(aslmauxgets>=32)return;
            aslmauxgets++;aslmmpsinstall(r);
        } else if(!observe)return;
        printf("uinter: ASLM lookup frame %x manager %x class %s\n",(int)fp,(int)manager,classname);
        printf("uinter: ASLM lookup procedure frame %x name %s ordinal %x errorptr %x\n",
            (int)fp,procedure,(int)aslmpointer(fp+16),(int)aslmpointer(fp+12));
        return;
    }
    if(mark==MGINSTALL) {
        unsigned char name[16],data[24];long np,result=GR_D(r,0);int n;
        aslmowndiags();
        if(aslminstallreturns>=16 || fp<2 || fp>0x7ffffff3L)return;
        np=aslmpointer(fp+8);
        if(np<2 || np>0x7fffffefL || copyin((caddr_t)np,(caddr_t)name,sizeof name))return;
        for(n=0;n<sizeof name && name[n];n++);
        if(n==sizeof name || (strcmp((char *)name,"auxtcp") && strcmp((char *)name,"auxudp")))return;
        aslminstallreturns++;
        printf("uinter: ASLM MPS install %s returned %x caller %x\n",name,(int)result,(int)aslmpointer(fp+4));
        if(result<2 || result>0x7fffffe7L || copyin((caddr_t)result,(caddr_t)data,sizeof data))return;
        if(result<=0x7fffffa7L) {aslmowncache(!strcmp((char *)name,"auxtcp") ? 1 : 2,result+24);aslmowndiags();}
        printf("uinter: ASLM MPS descriptor %x %x %x %x %x %x\n",(int)G32(data),(int)G32(data+4),
            (int)G32(data+8),(int)G32(data+12),(int)G32(data+16),(int)G32(data+20));
        return;
    }
    if(mark==MMPSLOAD) {
        unsigned char name[80];long pointer,record,manager=0,table,target;int slot;
        if(aslmmpcalls++>=128 || fp<2 || fp>0x7fffffe3L)return;
        record=aslmpointer(fp+24);pointer=record ? aslmfield(record,8) : aslmpointer(fp+12);
        for(slot=0;slot<aslmmpcount;slot++)if(aslmmpcopies[slot].pc==nativepc)break;
        if(slot<aslmmpcount)manager=aslmmpsmanager(aslmmpcopies[slot].base);
        table=aslmpointer(manager);target=aslmfield(table,64);
        if(pointer<2 || pointer>0x7fffffafL || copyin((caddr_t)pointer,(caddr_t)name,sizeof name)) {
            printf("uinter: ASLM MPS module frame %x record %x nameptr %x manager %x v64 %x inaccessible\n",
                (int)fp,(int)record,(int)pointer,(int)manager,(int)target);return;
        }
        for(i=0;i<sizeof name && name[i];i++);
        if(i>=sizeof name)return;
        printf("uinter: ASLM MPS module frame %x record %x name %s manager %x v64 %x\n",
            (int)fp,(int)record,name,(int)manager,(int)target);return;
    }
    if(mark==MLOAD || mark==MLOADEXIT) {
        unsigned char args[12],name[80];long manager,pointer;
        if((mark==MLOAD ? aslmloads++ : aslmloadexits++)>=128)return;
        if(fp<2 || fp>0x7fffffecL || copyin((caddr_t)(fp+8),(caddr_t)args,sizeof args))return;
        manager=G32(args);pointer=G32(args+4);
        for(i=0;i<sizeof name;i++)name[i]=0;
        if(pointer<2 || pointer>0x7fffffafL ||
            copyin((caddr_t)pointer,(caddr_t)name,sizeof name)) {
            printf("uinter: ASLM load %s frame %x manager %x classptr %x inaccessible error %d\n",
                mark==MLOAD ? "request" : "return",(int)fp,(int)manager,(int)pointer,
                mark==MLOADEXIT ? (short)GR_D(r,0) : 0);return;
        }
        for(i=0;i<sizeof name && name[i];i++);
        if(i>=sizeof name) {printf("uinter: ASLM load unterminated frame %x\n",(int)fp);return;}
        printf("uinter: ASLM load %s frame %x manager %x class %s flag %d error %d\n",
            mark==MLOAD ? "request" : "return",(int)fp,(int)manager,name,(int)args[11],
            mark==MLOADEXIT ? (short)GR_D(r,0) : 0);return;
    }
    if(mark==MREGEXIT) {
        if(aslmexits++<128)printf("uinter: ASLM registration source %x error %d\n",(int)src,(short)GR_D(r,0));
        return;
    }
    if(aslmclasses++>=256 || copyin((caddr_t)(fp-60),(caddr_t)pointer,4) ||
        copyin((caddr_t)(fp-56),(caddr_t)index,2) || !(address=G32(pointer)) ||
        copyin((caddr_t)address,(caddr_t)b,sizeof b))return;
    for(i=8;i<sizeof b && b[i];i++);
    if(i>=sizeof b)return;
    printf("uinter: ASLM class source %x index %d record %x kind %x version %x minimum %x name %s\n",
        (int)src,(int)G16(index),(int)address,(int)G32(b),(int)G16(b+4),(int)G16(b+6),b+8);
}

static unsigned char aslmindexedreturn[] = {
    0x4e,0x5e,0x4e,0x75,0x4e,0x56,0x00,0x00,0x2f,0x2e,0x00,0x0c,0x42,0x67,0x2f,0x2e
};
static long
aslmparent(fp,caller,source)
    long fp,caller,source;
{
    unsigned char b[16],link[4],parent[8];
    long previous=0,pc=0,actualsource=0;int i,stage;
    for(i=0;i<sizeof b;i++)b[i]=0;
    stage=1;
    if(fp<2 || fp>0x7fffffd7L || caller<0xdaae || caller>0x7fffac55L)goto rejected;
    stage=2;
    if(copyin((caddr_t)caller,(caddr_t)b,sizeof b))goto rejected;
    stage=3;
    for(i=0;i<sizeof b;i++)if(b[i]!=aslmindexedreturn[i])goto rejected;
    stage=4;
    if(copyin((caddr_t)fp,(caddr_t)link,4))goto rejected;
    previous=G32(link);stage=5;
    if(previous!=fp+28)goto rejected;
    stage=6;
    if(copyin((caddr_t)(previous+4),(caddr_t)parent,sizeof parent))goto rejected;
    pc=G32(parent);actualsource=G32(parent+4);stage=7;
    if(pc!=caller - 0xdaae + 0x12c28 || actualsource!=source)goto rejected;
    stage=8;
    if(copyin((caddr_t)pc,(caddr_t)b,sizeof b))goto rejected;
    stage=9;
    for(i=0;i<sizeof b;i++)if(b[i]!=aslmcaller[i])goto rejected;
    if(pc!=aslmparentpc) {
        aslmparentpc=pc;
        printf("uinter: ASLM parser parent wrapper %x caller %x frame %x\n",(int)caller,(int)pc,(int)previous);
    }
    return pc;
rejected:
    if(!(aslmfailseen&(1UL<<stage))) {
        aslmfailseen|=1UL<<stage;
        printf("uinter: ASLM chain reject stage %d fp %x wrapper %x previous %x parent %x source %x/%x\n",
            stage,(int)fp,(int)caller,(int)previous,(int)pc,(int)source,(int)actualsource);
        printf("uinter: ASLM chain words stage %d %x %x %x %x\n",
            stage,(int)G32(b),(int)G32(b+4),(int)G32(b+8),(int)G32(b+12));
    }
    return 0;
}

static void
aslmresult(handle,index,fp,type)
    long handle,fp,type;
    int index;
{
    unsigned char ptr[4],data[96],status[2],global[4],expand[4],caller[8];
    long address,returnpc=0,source=0;int i,o,system=-1,error=-1;
    if(!copyin((caddr_t)(fp+4),(caddr_t)caller,sizeof caller)) {
        returnpc=G32(caller);source=G32(caller+4);
    }
    if(!copyin((caddr_t)0xa60,(caddr_t)status,2))error=(short)G16(status);
    if(type==0x61583031L) {
        uin_aslmcode++;
        printf("uinter: ASLM aX01 ID %d handle %x error %d caller %x source %x\n",
            index,(int)handle,error,(int)returnpc,(int)source);
        return;
    }
    uin_aslmreads++;
    if(!handle || copyin((caddr_t)handle,(caddr_t)ptr,4) || !(address=G32(ptr)) ||
        copyin((caddr_t)address,(caddr_t)data,sizeof data)) {
        if(index==1)printf("uinter: ASLM libr index %d handle %x error %d inaccessible\n",index,(int)handle,error);
        return;
    }
    for(i=0;i<64 && data[i];i++);
    if(i>=64 || i<5 || data[0]!='O' || data[1]!='T')return;
    o=(i+2)&~1;
    if(o+24>sizeof data)return;
    if(!copyin((caddr_t)0x2b6,(caddr_t)expand,4) && G32(expand) &&
        !copyin((caddr_t)(G32(expand)+0x10c),(caddr_t)global,4) && G32(global) &&
        !copyin((caddr_t)(G32(global)+110),(caddr_t)status,2))system=G16(status);
    address=aslmparent(fp,returnpc,source);
    if(address)aslminstall(address);
    printf("uinter: ASLM libr %s index %d handle %x error %d fmt %x flags %x classes %d system %x caller %x source %x\n",
        data,index,(int)handle,error,(int)G16(data+o+4),(int)G32(data+o+18),
        (int)G16(data+o+22),system,(int)returnpc,(int)source);
}

static int
otprobeidentity(gp)
struct guest_proc *gp;
{
    static unsigned char prefix[]={0x4e,0x56,0,0,0x48,0xe7,0x3f,0x3c,0x20,0x6e,0,0x14,
        0x21,0x7c,0,0,0,1,0,0x2c,0x2a,0x50,0x2f,0x2e,0,8};
    unsigned char b[32],code[sizeof prefix];int i;
    if(!uinter_aslmtrace || !otprobeowner || gp!=otprobeowner || gp!=aslmowner ||
        ui.l_state!=LS_INUSE || ui.l_gp!=gp || ui.l_proc!=otprobeproc ||
        gp->gp_proc!=otprobeproc || u.u_procp!=otprobeproc ||
        prfind(otprobepid)!=otprobeproc || !ui_task(gp))return 0;
    if(copyin((caddr_t)0x15a,(caddr_t)b,2) || G16(b)!=0x0810 ||
        copyin((caddr_t)0x904,(caddr_t)b,4) || G32(b)!=otprobea5 ||
        copyin((caddr_t)otprobeentry,(caddr_t)code,sizeof code))return 0;
    for(i=0;i<sizeof code;i++)if(code[i]!=prefix[i])return 0;
    if(copyin((caddr_t)otprobedesc,(caddr_t)b,sizeof b))return 0;
    return G16(b)==0xaafe && b[2]==7 && b[17]==0 &&
        G32(b+12)==0x3fc0 && G32(b+20)==otprobeentry;
}

int
ui_otprobearm(gp,args)
struct guest_proc *gp;
unsigned char *args;
{
    unsigned char b[60];
    unsigned long entry=G32(args),desc=G32(args+4),ctx=G32(args+8);
    if(!uinter_aslmtrace || gp!=aslmowner || gp!=ui.l_gp ||
        !entry || entry>0x7fffffdfUL || !desc || desc>0x7fffffdfUL ||
        !ctx || ctx>0x7fffffc3UL || (entry|desc|ctx)&1 ||
        copyin((caddr_t)ctx,(caddr_t)b,sizeof b))return EINVAL;
    otprobeowner=gp;otprobeproc=u.u_procp;otprobepid=u.u_procp->p_pid;
    otprobeentry=entry;otprobedesc=desc;otprobectx=ctx;otprobea5=G32(b);
    if(!otprobeidentity(gp)) {otprobeowner=0;otprobeproc=0;return EINVAL;}
    uin_otprobecount=0;bzero((caddr_t)uin_otprobe,sizeof uin_otprobe);
    return 0;
}

int
ui_otacceptdiag(gp,b)
struct guest_proc *gp;
unsigned char *b;
{
    unsigned char h[32];long p;unsigned long protocol=G32(b);
    if(!uinter_aslmtrace || gp!=ui.l_gp || gp!=aslmowner || !ui_task(gp))return EACCES;
    if(protocol!=1 && protocol!=2)return EINVAL;
    p=aslmdiagptr[protocol];
    if(p<2 || p>0x7ffffbdfL || copyin((caddr_t)p,(caddr_t)h,12) ||
        G32(h)!=0x4f544447L || G32(h+4)!=1 || G32(h+8)!=protocol ||
        copyin((caddr_t)(p+896),(caddr_t)h,sizeof h) ||
        G32(h)!=0x4f544143L || G32(h+4)!=1 || G32(h+24)>16)return EINVAL;
    P32(b+4,p+896);P32(b+8,160);P32(b+12,G32(h+24));return 0;
}

static void
otproberecord(gp,r,tw)
struct guest_proc *gp;
char *r;
unsigned short tw;
{
    unsigned char b[60];long *sample,sp=GR_USP(r);int i,j,error;
    if(uin_otprobecount<0 || uin_otprobecount>=2 ||
        (tw!=0xa9c9 && tw!=0xabf7) || (unsigned)sp>0xfffffdffU ||
        !otprobeidentity(gp))return;
    error=copyin((caddr_t)0xaf0,(caddr_t)b,2)?-1:G16(b);
    if(tw==0xabf7 && error<=0)return;
    sample=uin_otprobe[uin_otprobecount++];
    sample[0]=GR_PC(r);sample[1]=sp;sample[2]=tw;sample[3]=error;
    sample[4]=GR_A(r,6);sample[5]=GR_A(r,5);sample[6]=GR_D(r,0);
    sample[7]=GR_A(r,0);sample[8]=GR_SR(r);sample[9]=otprobea5;
    sample[10]=otprobeentry;sample[11]=otprobedesc;sample[13]=otprobectx;
    for(i=0;i<15;i++)sample[16+i]=G32(r+4+4*i);
    for(i=0;i<512;i+=32) {
        if(copyin((caddr_t)(sp+i),(caddr_t)b,32))break;
        for(j=0;j<8;j++)sample[32+i/4+j]=G32(b+4*j);
        sample[12]+=32;
    }
    if(!copyin((caddr_t)otprobectx,(caddr_t)b,60))
        for(i=0;i<15;i++)sample[160+i]=G32(b+4*i);
}

/* Compare the current and front PSNs before entering the forced-quit dialog. */
static unsigned char forcecode[] = {
	0x4e, 0x56, 0xff, 0xf0, 0x42, 0x67, 0x48, 0x6e, 0xff, 0xf8, 0x70, 0xff,
	0x2f, 0x00, 0x3f, 0x3c, 0x00, 0x39, 0xa8, 0x8f, 0x30, 0x1f, 0x66, 0x00,
	0x00, 0x38, 0x42, 0x67, 0x48, 0x6e, 0xff, 0xf0, 0x3f, 0x3c, 0x00, 0x37,
	0xa8, 0x8f, 0x30, 0x1f, 0x66, 0x00, 0x00, 0x26, 0x20, 0x2e, 0xff, 0xf8,
	0xb0, 0xae, 0xff, 0xf0, 0x66, 0x00, 0x00, 0x1a, 0x20, 0x2e, 0xff, 0xfc,
	0xb0, 0xae, 0xff, 0xf4, 0x66, 0x00, 0x00, 0x0e, 0x30, 0x3c, 0x4e, 0x22,
	0xa9, 0xc9, 0x70, 0x01, 0x60, 0x00, 0x00, 0x04, 0x70, 0x00, 0x4e, 0x5e,
	0x4e, 0x75, 0x00, 0x00,
};

static int
ui_forcecall(r)
	char *r;
{
	static unsigned short call[] = { 0x4eba, FSIZE - 6, 0x4e71, 0x4e71, 0x4e71, 0x4e71 };
	long pc = GR_PC(r), sp = GR_USP(r);

	if (!ui_auxcall(r, 0, 0L, call, MFORCE, sizeof forcecode))
		return 0;
	if (copyout((caddr_t)forcecode, (caddr_t)(GR_USP(r) + FSIZE), sizeof forcecode)) {
		GR_PC(r) = pc;
		GR_USP(r) = sp;
		return 0;
	}
	dlm_cacheflush();
	ui_forceconsume();
	return 1;
}

/* hfstramp at a, over the previous _HFSDispatch */
static void
hfsinstall(a)
	long a;
{
	unsigned short t[sizeof hfstramp / 2];
	unsigned char b[4];

	if (copyin((caddr_t)HFSENT, (caddr_t)b, 4))
		return;
	bcopy((char *)hfstramp, (char *)t, sizeof t);
	t[3] = G32(b) >> 16;
	t[4] = G32(b);
	P32(b, a);
	if (copyout((caddr_t)t, (caddr_t)a, sizeof t) || copyout((caddr_t)b, (caddr_t)HFSENT, 4))
		return;
	dlm_cacheflush();
	hfst = a;
	ui_forcereserve(a + WNEOFF);
	if (uinter_trace)
		printf("uinter: _HFSDispatch at %x\n", (int)a);
}

/* RESUME at pc: back to the trap the stub was made at; 2 if another stub follows */
static int
ui_resume(r, pc)
	char *r;
	long pc;
{
	unsigned char b[FSIZE];
	long f = pc - 16, d0 = GR_D(r, 0);
	int i;

	if (copyin((caddr_t)f, (caddr_t)b, FSIZE) || G32(b + 20) != FMAGIC ||
	    G32(b + 92) < f + FSIZE)
		return 0;
	P32(b + 20, 0);
	(void)copyout((caddr_t)b + 20, (caddr_t)f + 20, 4);
	if (G16(b + 18) == MHFS && GR_D(r, 0) == 0 && GR_A(r, 0))
		hfsinstall(GR_A(r, 0));
    if(G16(b+18)==MCOFFSPACE) {
        long block=GR_A(r,0);int error=GR_D(r,0);
        bcopy((char *)b+24,r+4,60);GR_PC(r)=G32(b+84);
        GR_USP(r)=G32(b+92);GR_SR(r)=(GR_SR(r)&~0x1f)|(G16(b+88)&0x1f);
        spacefinish(GUESTP(u.u_procp),block,error);return 1;
    }
    if(G16(b+18)==MPMIDLE) {
        long block=GR_A(r,0);int error=GR_D(r,0);
        bcopy((char *)b+24,r+4,60);GR_PC(r)=G32(b+84);
        GR_USP(r)=G32(b+92);GR_SR(r)=(GR_SR(r)&~0x1f)|(G16(b+88)&0x1f);
        pmstubfinish(GUESTP(u.u_procp),r,error ? 0L : block);
        return 1;
    }
    if(G16(b+18)==MCLASS || G16(b+18)==MREGEXIT ||
        G16(b+18)==MLOAD || G16(b+18)==MLOADEXIT ||
        G16(b+18)==MGET || G16(b+18)==MMPSLOAD || G16(b+18)==MGETEXIT || G16(b+18)==MGINSTALL) {
        aslmsteplog(r,G16(b+18),G32(b+84));
        GR_PC(r)=G32(b+84)+((G16(b+18)==MLOAD || G16(b+18)==MLOADEXIT || G16(b+18)==MGETEXIT || G16(b+18)==MGINSTALL) ? 2 : 4);GR_USP(r)=G32(b+92);
        return G16(b+90) ? 2 : 1;
    }
    if(G16(b+18)==MCOFFFRONT || G16(b+18)==MCOFFAUXFRONT) {
        unsigned char result[4],native[2];
        char world[16];
        long sp=G32(b+92),used=G16(b+18)==MCOFFAUXFRONT ? 6 : 4;
        if(!ui_coffworld(GUESTP(u.u_procp),world) ||
            copyin((caddr_t)GR_USP(r),(caddr_t)native,2)) {
            uin_coffrefused++;psignal(u.u_procp,SIGSEGV);return 1;
        }
        P32(result,(short)G16(native));
        if(copyout((caddr_t)result,(caddr_t)(sp+used),4)) {
            uin_coffrefused++;psignal(u.u_procp,SIGSEGV);return 1;
        }
        bcopy((char *)b+24,r+4,60);
        GR_PC(r)=G32(b+84)+2;GR_USP(r)=sp+used;
        return 1;
    }
    if(G16(b+18)==MASLM) {
        unsigned char result[4],args[6];
        long sp=G32(b+92);
        if(copyin((caddr_t)f,(caddr_t)result,4) ||
            copyin((caddr_t)sp,(caddr_t)args,6) ||
            copyout((caddr_t)result,(caddr_t)(sp+6),4)) {
            GR_PC(r)=G32(b+84);GR_USP(r)=sp;
            psignal(u.u_procp,SIGSEGV);return 1;
        }
        aslmresult(G32(result),G16(args),GR_A(r,6),G32(args+2));
        GR_PC(r)=G32(b+84)+2;GR_USP(r)=sp+6;
        return 1;
    }
	bcopy((char *)b + 24, r + 4, 60);
	GR_PC(r) = G32(b + 84);
	GR_SR(r) = (GR_SR(r) & ~0x1f) | (G16(b + 88) & 0x1f);
	GR_USP(r) = G32(b + 92);
	if (G16(b + 18) == MFORCE && d0 != 0) {
        unsigned char error[2];
        if(copyin((caddr_t)0xaf0,(caddr_t)error,2)) {
            psignal(u.u_procp,SIGSEGV);return 3;
        }
        if(G16(error)==20002) {
            P16(error,0);
            if(copyout((caddr_t)error,(caddr_t)0xaf0,2)) {
                psignal(u.u_procp,SIGSEGV);return 3;
            }
        }
    }
	if (G16(b + 18) == MFORCE && d0 == 0)
		ui_forceretry();
	if (G16(b + 18) == MSPEC)
		mkspec(r, f + FSIZE);
	if (G16(b + 18) == MPATH)
		return pathnext(r, f + FSIZE);
	if (G16(b + 18) == MPARMS) {
		if ((short)d0 == PARAMERR && extcall(r))
			return 2;
		if ((short)d0 == PARAMERR && (GR_D(r, 0) & 0xffff) == 0x30)
			d0 = mkparms(GR_A(r, 0));
	}
	if (G16(b + 18) == MEXTFS) {
		P16(b, d0);
		(void)copyout((caddr_t)b, (caddr_t)(GR_A(r, 0) + 0x10), 2);
	}
	if (G16(b + 18) >= MCAT && G16(b + 18) <= MTRASH) {
		if ((i = trashnext(r, G16(b + 18), f, &d0)) != 0)
			return i + 1;
		trashbusy = 0;
	}
	if (G16(b + 18) == MTPAR && d0 == 0)
		trashroot(GR_A(r, 0));
	if (G16(b + 18) == MXVOL && d0 == 0)
		xvolbytes(GR_A(r, 0));
	if (G16(b + 18) == MOPEN || G16(b + 18) == MPARMS || G16(b + 18) == MEXTFS ||
	    (G16(b + 18) >= MCAT && G16(b + 18) <= MXVOL)) {
		GR_D(r, 0) = d0;
		GR_SR(r) = (GR_SR(r) & ~0xf) | ((short)d0 < 0 ? 8 : d0 ? 0 : 4);
		GR_PC(r) += 2;
	}
	return G16(b + 18) == MFORCE ? 3 : 1;
}

/*
 * Hosted FSMakeFSSpec uses catalog calls, including each colon-path component.
 * Directory IDs and missing-file parents come from the external filesystem.
 */
struct uispecpath {
	unsigned char path[256];
	unsigned short pos, stage;
	short vref;
	long dir;
};
#define PATHPB 160
#define PATHX ((PATHPB + 256 + sizeof(struct uispecpath) + 3) & ~3)
#define P_VOLUME 1
#define P_DEFAULT 2
#define P_PARENT 3
#define P_COMPONENT 4
#define P_FINAL 5
#define P_DIRECTORY 6
#define P_START 7
#define P_WORKING 8

static unsigned short volcall[] = { 0x41fa, FSIZE - 6, 0xa207, 0x4e71, 0x4e71, 0x4e71 };
static unsigned short defcall[] = { 0x41fa, FSIZE - 6, 0xa214, 0x4e71, 0x4e71, 0x4e71 };
static unsigned short wdcall[] = { 0x41fa, FSIZE - 6, 0x7007, 0xa260, 0x4e71, 0x4e71 };
static unsigned short speccall[] = { 0x701b, 0xa260, 0x4e71, 0x4e71, 0x4e71, 0x4e71 };

static int
pathdone(r, s, err, name, parent)
	char *r;
	struct uispecpath *s;
	int err;
	unsigned char *name;
	long parent;
{
	unsigned char pb[0x34], out[70], b[2];
	long a0 = GR_A(r, 0);

	if (copyin((caddr_t)a0, (caddr_t)pb, sizeof pb))
		err = PARAMERR;
	else if (name && (!err || err == FNFERR)) {
		if (name[0] > 63)
			err = -37;
		else {
			P16(out, s->vref);
			P32(out + 2, parent);
			bcopy((char *)name, (char *)out + 6, name[0] + 1);
			if (copyout((caddr_t)out, (caddr_t)G32(pb + 0x1c), 7 + name[0]))
				err = PARAMERR;
		}
	}
	P16(b, err);
	(void)copyout((caddr_t)b, (caddr_t)(a0 + 0x10), 2);
	GR_D(r, 0) = err;
	GR_SR(r) = (GR_SR(r) & ~0xf) | (err < 0 ? 8 : err ? 0 : 4);
	GR_PC(r) += 2;
	return 1;
}

static int
pathissue(r, s, name, index, code)
	char *r;
	struct uispecpath *s;
	unsigned char *name;
	int index;
	unsigned short *code;
{
	unsigned char c[PATHPB], nm[256];
	long q;

	bzero((caddr_t)c, sizeof c);
	bzero((caddr_t)nm, sizeof nm);
	if (name)
		bcopy((char *)name, (char *)nm, name[0] + 1);
	/* Prepare the complete catalog request before changing guest execution. */
	q = ((GR_USP(r) - FSIZE - PATHX) & ~3L) + FSIZE;
	P32(c + 0x12, q + PATHPB);
	P16(c + 0x16, s->vref);
	P16(c + 0x1c, index);
	P32(c + 0x30, s->dir);
	if (copyout((caddr_t)c, (caddr_t)q, sizeof c) ||
	    copyout((caddr_t)nm, (caddr_t)(q + PATHPB), sizeof nm) ||
	    copyout((caddr_t)s, (caddr_t)(q + PATHPB + 256), sizeof *s) ||
	    !ui_auxcall(r, 0, 0L, code, MPATH, PATHX))
		return 0;
	return 2;
}

static int
pathstep(r, s)
	char *r;
	struct uispecpath *s;
{
	unsigned char nm[64];
	int start, n, i;

	if (s->pos > s->path[0]) {
		s->stage = P_DIRECTORY;
		i = pathissue(r, s, (unsigned char *)0, -1, catcall);
		return i ? i : pathdone(r, s, PARAMERR, (unsigned char *)0, 0L);
	}
	start = s->pos;
	while (s->pos <= s->path[0] && s->path[s->pos] != ':')
		s->pos++;
	n = s->pos - start;
	if (n > 63)
		return pathdone(r, s, -37, (unsigned char *)0, 0L);
	nm[0] = n;
	bcopy((char *)s->path + start, (char *)nm + 1, n);
	s->stage = !n ? P_PARENT : s->pos <= s->path[0] ? P_COMPONENT : P_FINAL;
	if (s->pos <= s->path[0])
		s->pos++;
	i = pathissue(r, s, nm, n ? 0 : -1, catcall);
	return i ? i : pathdone(r, s, PARAMERR, (unsigned char *)0, 0L);
}

static int
pathnext(r, q)
	char *r;
	long q;
{
	struct uispecpath s;
	unsigned char c[PATHPB], nm[256];
	long parent;
	int err, i;

	if (copyin((caddr_t)q, (caddr_t)c, sizeof c) ||
	    copyin((caddr_t)(q + PATHPB), (caddr_t)nm, sizeof nm) ||
	    copyin((caddr_t)(q + PATHPB + 256), (caddr_t)&s, sizeof s)) {
		bzero((caddr_t)&s, sizeof s);
		return pathdone(r, &s, PARAMERR, (unsigned char *)0, 0L);
	}
	if (s.pos < 1 || s.pos > s.path[0] + 1 || s.stage < P_VOLUME || s.stage > P_WORKING)
		return pathdone(r, &s, PARAMERR, (unsigned char *)0, 0L);
	err = (short)G16(c + 0x10);
	if (s.stage == P_VOLUME || s.stage == P_DEFAULT || s.stage == P_WORKING) {
		if (err)
			return pathdone(r, &s, err, (unsigned char *)0, 0L);
		s.vref = G16(c + (s.stage == P_VOLUME ? 0x16 : 0x20));
		P16(c + 0x16, s.vref);
		if (copyout((caddr_t)c + 0x16, (caddr_t)(q + 0x16), 2))
			return pathdone(r, &s, PARAMERR, (unsigned char *)0, 0L);
		if (!extvcb(q, 0))
			return ui_auxcall(r, 0, 0L, speccall, MOPEN, 0) ? 2 :
			    pathdone(r, &s, PARAMERR, (unsigned char *)0, 0L);
		if (s.stage != P_VOLUME && !s.dir)
			s.dir = G32(c + 0x30);
		s.stage = P_START;
		i = pathissue(r, &s, (unsigned char *)0, -1, catcall);
		return i ? i : pathdone(r, &s, PARAMERR, (unsigned char *)0, 0L);
	}
	if (s.stage == P_FINAL || s.stage == P_DIRECTORY) {
		parent = !err ? G32(c + 0x64) : s.dir;
		if (!err && trashdir && G32(c + 0x30) == trashdir &&
		    parent == trashpar && s.vref == trashvref)
			parent = 2;
		return pathdone(r, &s, err,
		    !err || (err == FNFERR && s.stage == P_FINAL) ? nm : (unsigned char *)0, parent);
	}
	if (err)
		return pathdone(r, &s, err, (unsigned char *)0, 0L);
	if (!(c[0x1e] & 0x10))
		return pathdone(r, &s, -120, (unsigned char *)0, 0L);
	s.dir = G32(c + (s.stage == P_PARENT ? 0x64 : 0x30));
	return pathstep(r, &s);
}

static int
pathspec(r, pb, name)
	char *r;
	unsigned char *pb, *name;
{
	struct uispecpath s;
	unsigned char nm[256];
	int n;

	bzero((caddr_t)&s, sizeof s);
	bcopy((char *)name, (char *)s.path, name[0] + 1);
	s.vref = G16(pb + 0x16);
	s.dir = G32(pb + 0x30);
	s.pos = 2;
	if (name[1] != ':') {
		for (n = 1; n <= name[0] && name[n] != ':'; n++)
			;
		nm[0] = n;
		bcopy((char *)name + 1, (char *)nm + 1, n);
		s.pos = n + 1;
		s.dir = 2;
		s.vref = -32768;
		s.stage = P_VOLUME;
		return pathissue(r, &s, nm, -1, volcall) != 0;
	}
	if (s.vref > 0) {
		s.stage = P_VOLUME;
		if (!s.dir)
			s.dir = 2;
		return pathissue(r, &s, (unsigned char *)0, 0, volcall) != 0;
	}
	if (!s.vref) {
		s.stage = P_DEFAULT;
		return pathissue(r, &s, (unsigned char *)0, 0, defcall) != 0;
	}
	s.stage = P_WORKING;
	return pathissue(r, &s, (unsigned char *)0, 0, wdcall) != 0;
}

static int
catspec(r)
	char *r;
{
	unsigned char pb[0x34], nm[256], q[CINFO];
	long np, f;
	int n = 0, i;
	short vref;

	if (copyin((caddr_t)GR_A(r, 0), (caddr_t)pb, sizeof pb))
		return 0;
	np = G32(pb + 0x12);
	vref = G16(pb + 0x16);
	nm[0] = 0;
	if (np && (copyin((caddr_t)np, (caddr_t)nm, 1) ||
	    copyin((caddr_t)(np + 1), (caddr_t)(nm + 1), nm[0])))
		return 0;
	for (n = nm[0], i = 1; i <= n; i++)
		if (nm[i] == ':')
			return pathspec(r, pb, nm);
	if (n > 63 || vref >= 0)
		return 0;
	if (!ui_auxcall(r, 0, 0L, catcall, MSPEC, SPECX))
		return 0;
	f = GR_USP(r) + FSIZE;
	bzero((caddr_t)q, sizeof q);
	P32(q + 0x12, f + CINFO);
	P16(q + 0x16, vref);
	P16(q + 0x1c, n ? 0 : -1);
	bcopy((char *)pb + 0x30, (char *)q + 0x30, 4);
	(void)(copyout((caddr_t)q, (caddr_t)f, sizeof q) ||
	    copyout((caddr_t)nm, (caddr_t)(f + CINFO), n + 1));
	return 1;
}

/*
 * PBHOpenDF ($1A), which the ROM refuses for Patch.067C's volumes: PBHOpen
 * (PBOpen without the HFS bit) instead, as Apple's glue does on paramErr;
 * not for driver names.
 */
static int
ui_opendf(r, tw)
	char *r;
	int tw;
{
	unsigned char b[4], c[2];

	if (copyin((caddr_t)(GR_A(r, 0) + 0x12), (caddr_t)b, 4) ||
	    (G32(b) && (copyin((caddr_t)G32(b), (caddr_t)c, 2) || (c[0] && c[1] == '.'))))
		return 0;
	return ui_auxcall(r, 0, 0L, (tw & 0x200) ? hopen : popen, MOPEN, 0);
}

/* _HFSDispatch selectors Patch.067C's external file system serves */
static int
extsel(s)
	int s;
{
	return (s >= 0x14 && s <= 0x18) || (s >= 0x20 && s <= 0x30) ||
	    (s >= 0x32 && s <= 0x35) || s == 0x38 || s == 0x39 || s == 0x60 || s == 0x61;
}

/*
 * The VCB of an external file system's volume: vref's (0 the default, or
 * a working directory); for a Desktop Manager refnum, the first such.
 */
static long
extvcb(a0, sel)
	long a0;
	int sel;
{
	unsigned char b[8];
	long v;
	short vref;
	int n;

	if (copyin((caddr_t)(a0 + 0x16), (caddr_t)b, 2))
		return 0;
	vref = G16(b);
	if (sel > 0x20 && sel < 0x30)
		vref = 1;
	if (vref == 0) {
		if (copyin((caddr_t)DEFVCB, (caddr_t)b, 4))
			return 0;
		v = G32(b);
	} else if (vref <= -32767) {
		if (copyin((caddr_t)WDCBS, (caddr_t)b, 4) ||
		    copyin((caddr_t)(G32(b) + vref + 32767), (caddr_t)b, 4))
			return 0;
		v = G32(b);
	} else {
		if (copyin((caddr_t)VCBQ, (caddr_t)b, 4))
			return 0;
		for (v = G32(b), n = 0; v; v = G32(b), n++) {
			if (n == 256 || (v & 1) || (unsigned long)v > 0x7fffff80)
				return 0;
			if (copyin((caddr_t)(v + 0x4c), (caddr_t)b, 4))
				return 0;
			if (vref > 0 ? G16(b) != 0 : (short)G16(b + 2) == vref)
				break;
			if (copyin((caddr_t)v, (caddr_t)b, 4))
				return 0;
		}
	}
	if (!v || (v & 1) || (unsigned long)v > 0x7fffff80 ||
	    copyin((caddr_t)(v + 0x4c), (caddr_t)b, 2) || !G16(b))
		return 0;
	return v;
}

/* the call at a0 to ToExtFS, through a stub: movea.l #vcb,a1; movea.l ToExtFS,a2; jsr (a2) */
static int
extcall(r)
	char *r;
{
	unsigned short code[6];
	unsigned char b[4];
	long a0 = GR_A(r, 0), v;
	int sel = GR_D(r, 0) & 0xffff;

	if (!extsel(sel) || !(v = extvcb(a0, sel)) ||
	    copyin((caddr_t)TOEXTFS, (caddr_t)b, 4) || !G32(b))
		return 0;
	P32(b, v);
	if (copyout((caddr_t)b, (caddr_t)REQSTVOL, 4))
		return 0;
	P16(b, 0xa260);
	if (copyout((caddr_t)b, (caddr_t)(a0 + 6), 2))
		return 0;
	code[0] = 0x227c;
	code[1] = v >> 16;
	code[2] = v;
	code[3] = 0x2478;
	code[4] = TOEXTFS;
	code[5] = 0x4e92;
	if (!ui_auxcall(r, 0, 0L, code, MEXTFS, 0))
		return 0;
	return 1;
}

/*
 * GetVolParms ($30) and the other calls of extsel, which the ROM refuses
 * for Patch.067C's volumes: the call runs through a stub and, on paramErr,
 * goes to extcall.  Without a ToExtFS answer, GetVolParms gets a version 1
 * buffer with no attributes, as a plain local volume would; 8.1's
 * FindFolder gives up on a volume without one.
 */
static int
ui_volparms(r, tw)
	char *r;
	int tw;
{
	unsigned short code[6];
	int i;

	code[0] = tw;
	for (i = 1; i < 6; i++)
		code[i] = 0x4e71;
	return ui_auxcall(r, 0, 0L, code, MPARMS, 0);
}

/* the buffer for ui_volparms: noErr, or paramErr if it does not fit */
static long
mkparms(a0)
	long a0;
{
	unsigned char pb[0x2c], v[14];
	long n;

	if (copyin((caddr_t)a0, (caddr_t)pb, sizeof pb))
		return PARAMERR;
	n = G32(pb + 0x24);
	if (n < 6 || !G32(pb + 0x20))
		return PARAMERR;
	if (n > sizeof v)
		n = sizeof v;
	bzero((caddr_t)v, sizeof v);
	P16(v, 1);
	P32(pb + 0x28, n);
	P16(pb + 0x10, 0);
	if (copyout((caddr_t)v, (caddr_t)G32(pb + 0x20), n) ||
	    copyout((caddr_t)pb + 0x10, (caddr_t)(a0 + 0x10), 2) ||
	    copyout((caddr_t)pb + 0x28, (caddr_t)(a0 + 0x28), 4))
		return PARAMERR;
	if (uinter_trace)
		printf("uinter: GetVolParms\n");
	return 0;
}

/* RESUME of catspec's stub: the FSSpec, ioResult and d0, past the trap */
static void
mkspec(r, q)
	char *r;
	long q;
{
	unsigned char c[CINFO], nm[64], pb[0x34], sp[70], w[8];
	long a0 = GR_A(r, 0), dir;
	short err, vref;

	if (copyin((caddr_t)q, (caddr_t)c, sizeof c) ||
	    copyin((caddr_t)(q + CINFO), (caddr_t)nm, sizeof nm) ||
	    copyin((caddr_t)a0, (caddr_t)pb, sizeof pb) || nm[0] > 63)
		return;
	err = G16(c + 0x10);
	vref = G16(pb + 0x16);
	dir = G32(pb + 0x30);
	if (vref <= -32767) {
		/* a working directory: its volume's refnum and its directory */
		if (copyin((caddr_t)WDCBS, (caddr_t)w, 4) ||
		    copyin((caddr_t)(G32(w) + vref + 32767), (caddr_t)w, 8))
			return;
		if (!dir)
			dir = G32(w + 4);
		if (copyin((caddr_t)(G32(w) + 0x4e), (caddr_t)w, 2))
			return;
		vref = G16(w);
	} else if (!dir)
		dir = 2;
	if (!err)
		dir = G32(c + 0x64);
	if (!err && trashdir && G32(c + 0x30) == trashdir && dir == trashpar && vref == trashvref)
		dir = 2;
	if (!err || (err == FNFERR && nm[0])) {
		P16(sp, vref);
		P32(sp + 2, dir);
		bcopy((char *)nm, (char *)sp + 6, nm[0] + 1);
		if (copyout((caddr_t)sp, (caddr_t)G32(pb + 0x1c), 7 + nm[0]))
			return;
	}
	P16(w, err);
	if (copyout((caddr_t)w, (caddr_t)(a0 + 0x10), 2))
		return;
	GR_D(r, 0) = err;
	GR_SR(r) = (GR_SR(r) & ~0xf) | (err < 0 ? 8 : err ? 0 : 4);
	GR_PC(r) += 2;
	if (uinter_trace)
		printf("uinter: FSMakeFSSpec %d\n", err);
}

/* 1 if the long at low memory a points into the system heap */
static int
ui_insys(a)
	long a;
{
	unsigned char b[4];
	long v, z, lim;

	if (copyin((caddr_t)a, (caddr_t)b, 4))
		return 0;
	v = G32(b);
	if (copyin((caddr_t)0x2a6, (caddr_t)b, 4))
		return 0;
	z = G32(b);
	if (copyin((caddr_t)z, (caddr_t)b, 4))
		return 0;
	lim = G32(b);
	return v > z && v < lim;
}

long uinter_soundnative, uinter_soundrefused, uinter_soundrollback;

static unsigned char sndentry[] = {0x20,0x78,0x02,0xb6,0x20,0x68,0x01,0x10,0x20,0x68,0x00,0x2a,0x20,0x70,0x00,0x00,0x48,0x40,0x4e,0xd0};
static unsigned char sndadd[] = {0x30,0x2f,0x00,0x08,0x0c,0x40,0x00,0x24,0x6c,0x1a,0x22,0x78,0x02,0xb6,0x22,0x69,0x01,0x10,0x22,0x69,0x00,0x2a,0x20,0x6f,0x00,0x04,0x23,0x88,0x00,0x00,0x42,0x6f,0x00,0x0a,0x60,0x06,0x3f,0x7c,0xff,0xce,0x00,0x0a,0x20,0x57,0xde,0xfc,0x00,0x0a,0x4e,0xd0};
static unsigned char sndmanager[] = {0x42,0x41,0x12,0x00,0x0c,0x01,0x00,0x24,0x64,0x04,0x4e,0xfb,0x10,0xd0,0x20,0x5f,0x32,0x00,0xee,0x49,0xde,0xc1,0x3e,0xbc,0xff,0xce,0x4e,0xd0};
static unsigned char sndversion[] = {0x4e,0x56,0x00,0x00,0x2d,0x7c,0x03,0x21,0x80,0x00,0x00,0x08,0x4e,0x5e,0x4e,0x75};
static unsigned char sndlegacy[] = {0x48,0x78,0x00,0x40,0x61,0xff,0x00,0x00,0x69,0xf6,0x23,0xc8,0x00,0x05,0x66,0x3c,0x20,0x79,0x00,0x00,0x02,0xb6,0x21,0x79,0x00,0x05,0x66,0x3c,0x01,0x10,0x58,0x4f,0x66,0x14};
static unsigned char sndcall[] = {0x61,0xff,0x00,0x02,0xaa,0x90,0x70,0x00};
static unsigned char sndnop[] = {0x4e,0x71,0x4e,0x71,0x4e,0x71};

static int
soundmatch(gp,a,code,n)
struct guest_proc *gp;
unsigned long a;
unsigned char *code;
int n;
{
    unsigned char b[64];
    return n<=sizeof b && ui_macrange(gp,a,n) &&
        !copyin((caddr_t)a,(caddr_t)b,n) && !bcmp((caddr_t)b,(caddr_t)code,n);
}

/* Preserve native sound globals and dispatch slots during late initialization. */
static int
soundnative(gp)
struct guest_proc *gp;
{
    unsigned char b[102],call[8],v[4];
    unsigned long expand,globals,table,entry,manager,version;
    int installed;
    if(gp!=ui.l_gp || gp!=ui.l_active || gp->gp_proc!=u.u_procp ||
        ui.l_proc!=u.u_procp || ui.l_state!=LS_INUSE ||
        (AUXP(gp)->ap_mac&APM_COFF) || (gp->gp_vsr&0x700) ||
        copyin((caddr_t)0x15a,(caddr_t)v,2) || G16(v)!=0x0810 ||
        copyin((caddr_t)TOOLTAB,(caddr_t)v,4) ||
        !soundmatch(gp,entry=G32(v),sndentry,sizeof sndentry) ||
        copyin((caddr_t)0x2b6,(caddr_t)v,4) ||
        !ui_macrange(gp,expand=G32(v),0x114) ||
        copyin((caddr_t)(expand+0x110),(caddr_t)v,4) ||
        !ui_macrange(gp,globals=G32(v),sizeof b) ||
        copyin((caddr_t)globals,(caddr_t)b,sizeof b) ||
        G32(b+0x12)!=102 || G16(b+0x18)!=1 ||
        !ui_macrange(gp,G32(b+0x26),56) ||
        !ui_macrange(gp,table=G32(b+0x2a),52) ||
        copyin((caddr_t)(table+0x28),(caddr_t)v,4) ||
        !soundmatch(gp,G32(v),sndadd,sizeof sndadd) ||
        copyin((caddr_t)(table+8),(caddr_t)v,4) ||
        !soundmatch(gp,manager=G32(v),sndmanager,sizeof sndmanager) ||
        manager<24 || !ui_macrange(gp,manager-24,4) ||
        copyin((caddr_t)(manager-24),(caddr_t)v,4) || G16(v)!=0x4efa)
        goto refused;
    version=manager-22+(short)G16(v+2);
    if(!soundmatch(gp,version,sndversion,sizeof sndversion) ||
        !soundmatch(gp,0x33108,sndlegacy,sizeof sndlegacy) ||
        !ui_macrange(gp,0x8676,sizeof call) ||
        copyin((caddr_t)0x8676,(caddr_t)call,sizeof call))goto refused;
    installed=!bcmp((caddr_t)call,(caddr_t)sndnop,sizeof sndnop);
    if(G16(call+6)!=0x7000 || (!installed &&
        bcmp((caddr_t)call,(caddr_t)sndcall,sizeof sndcall)))goto refused;
    if(installed)return 1;
    if(as_checkprot(u.u_procp->p_as,(caddr_t)0x8676,6,PROT_READ|PROT_WRITE|PROT_EXEC))goto refused;
    if(copyout((caddr_t)sndnop,(caddr_t)0x8676,6) ||
        !soundmatch(gp,0x8676,sndnop,sizeof sndnop)) {
        uinter_soundrollback++;
        if(copyout((caddr_t)call,(caddr_t)0x8676,6) ||
            !soundmatch(gp,0x8676,call,6)) {
            uinter_soundrefused++;dlm_cacheflush();
            psignal(u.u_procp,SIGSEGV);return -1;
        }
        dlm_cacheflush();return 0;
    }
    dlm_cacheflush();uinter_soundnative++;return 1;
refused:
    uinter_soundrefused++;return 0;
}

int uin_soundopendiag;
long uin_soundopencount, uin_soundopenrefused, uin_soundopenearly;
unsigned long uin_soundopen[16][22], uin_soundopenreject[8], uin_soundopenlast[10];

/* Record native component opens without changing the guest call. */
static void
soundopenrecord(gp,r,tw)
struct guest_proc *gp;
char *r;
unsigned short tw;
{
    unsigned char v[4],stack[20],bytes[32];
    unsigned long entry=0,expand=0,globals=0,table=0,manager=0,version=0,size=0,sp,pc,*row;
    int i,stage=0;
    if(!uin_soundopendiag || tw!=0xa82a)return;
    if(uin_soundopencount>=16){uin_soundopenreject[0]++;return;}
    if(GR_D(r,0)!=7){uin_soundopenreject[1]++;return;}
    if(gp!=ui.l_gp || gp!=ui.l_active){uin_soundopenreject[2]++;return;}
    if(gp->gp_proc!=u.u_procp || ui.l_proc!=u.u_procp){uin_soundopenreject[3]++;return;}
    if(ui.l_state!=LS_INUSE){uin_soundopenreject[4]++;return;}
    if(AUXP(gp)->ap_mac&APM_COFF){uin_soundopenreject[5]++;return;}
    stage=1;
    if(copyin((caddr_t)0x15a,(caddr_t)v,2) || G16(v)!=0x0810)goto refused;
    stage=2;
    if(copyin((caddr_t)TOOLTAB,(caddr_t)v,4) ||
        !soundmatch(gp,entry=G32(v),sndentry,sizeof sndentry))goto refused;
    stage=3;
    if(copyin((caddr_t)0x2b6,(caddr_t)v,4))goto refused;
    expand=G32(v);stage=4;
    if(!ui_macrange(gp,expand,0x114) ||
        copyin((caddr_t)(expand+0x110),(caddr_t)v,4))goto refused;
    globals=G32(v);stage=5;
    if(!ui_macrange(gp,globals,102) ||
        copyin((caddr_t)(globals+0x12),(caddr_t)v,4))goto refused;
    size=G32(v);stage=6;
    if(size!=102)goto refused;
    stage=7;
    if(copyin((caddr_t)(globals+0x2a),(caddr_t)v,4))goto refused;
    table=G32(v);
    if(!ui_macrange(gp,table,52) || copyin((caddr_t)(table+8),(caddr_t)v,4))goto refused;
    manager=G32(v);stage=8;
    if(!soundmatch(gp,manager,sndmanager,sizeof sndmanager))goto refused;
    stage=9;
    if(manager<24 || !ui_macrange(gp,manager-24,4) ||
        copyin((caddr_t)(manager-24),(caddr_t)v,4) || G16(v)!=0x4efa)goto refused;
    version=manager-22+(short)G16(v+2);stage=10;
    if(!soundmatch(gp,version,sndversion,sizeof sndversion))goto refused;
    stage=11;
    if(copyin((caddr_t)(globals+0x40),(caddr_t)v,4))goto refused;
    if(!G32(v)){uin_soundopenearly++;uin_soundopenreject[7]++;return;}
    row=uin_soundopen[uin_soundopencount];
    for(i=0;i<22;i++)row[i]=0xffffffffUL;
    row[0]=pc=GR_PC(r);row[1]=GR_D(r,0);row[2]=GR_A(r,0);
    row[3]=sp=GR_USP(r);row[10]=GR_A(r,5);row[11]=globals;row[13]=(unsigned long)(gp->gp_vsr&0xffff)<<16;
    if(ui_macrange(gp,sp,sizeof stack) &&
        !copyin((caddr_t)sp,(caddr_t)stack,sizeof stack)) {
        for(i=0;i<5;i++)row[4+i]=G32(stack+4*i);
        row[13]|=1;
    }
    if(!copyin((caddr_t)0xb7c,(caddr_t)v,4)){row[9]=G32(v);row[13]|=2;}
    if(!copyin((caddr_t)(globals+0x40),(caddr_t)v,4)){row[12]=G32(v);row[13]|=4;}
    if(pc>=16 && ui_macrange(gp,pc-16,sizeof bytes) &&
        !copyin((caddr_t)(pc-16),(caddr_t)bytes,sizeof bytes)) {
        for(i=0;i<8;i++)row[14+i]=G32(bytes+4*i);
        row[13]|=8;
    }
    uin_soundopencount++;
    return;
refused:
    uin_soundopenrefused++;uin_soundopenreject[6]++;
    uin_soundopenlast[0]=GR_PC(r);uin_soundopenlast[1]=gp->gp_vsr;
    uin_soundopenlast[2]=stage;uin_soundopenlast[3]=expand;
    uin_soundopenlast[4]=globals;uin_soundopenlast[5]=size;
    uin_soundopenlast[6]=table;uin_soundopenlast[7]=manager;
    uin_soundopenlast[8]=version;uin_soundopenlast[9]=GR_D(r,0);
}

/* 1 if _SoundDispatch is _Unimplemented; -1 if the table is unreadable. */
static int
ui_unimpl()
{
	unsigned char b[4], u[4];

	if (copyin((caddr_t)TOOLTAB, (caddr_t)b, 4) ||
	    copyin((caddr_t)(TOOLTAB + 4 * 0x9f), (caddr_t)u, 4))
		return -1;
	return G32(b) == G32(u);
}

/* Keep the public slot available to the native Sound Manager installer. */
static int
soundearly(r, tw)
	char *r;
	int tw;
{
	unsigned char b[510];
	unsigned long d0 = GR_D(r, 0), sp = GR_USP(r), pc = GR_PC(r) + 2;
	unsigned long used = ((d0 >> 24) & 0xff) * 2, result;
	int size = 0, flags = 4;
	unsigned long selector, diff;

	if (tw & 0x400) {
		if (sp > 0xfffffffbUL || copyin((caddr_t)sp, (caddr_t)b, 4))
			return 0;
		pc = G32(b);
		sp += 4;
	}
	if (sp > 0xffffffffUL - used)
		return 0;
	result = sp + used;
	if (used && copyin((caddr_t)sp, (caddr_t)b, used))
		return 0;
	if (!used) {
		P32(b, 0);
		size = 4;
	} else if ((d0 & 0xffff) != 0x10 && d0 != 0x02180008UL) {
		selector = d0 & 0xffff;
		P16(b, selector == 0x14 ? -220 : -201);
		/* Match the final word comparison flags. */
		diff = (selector - 0x14) & 0xffff;
		flags = selector == 0x14 ? 8 :
		    ((diff & 0x8000) ? 8 : 0) | (selector < 0x14 ? 1 : 0) |
		    (((selector ^ 0x14) & (selector ^ diff) & 0x8000) ? 2 : 0);
		size = 2;
	}
	if (size && (result > 0xffffffffUL - (size - 1) ||
	    copyout((caddr_t)b, (caddr_t)result, size)))
		return 0;
	GR_D(r, 1) = (((d0 << 8) | (d0 >> 24)) & 0xffff0000UL) | used;
	GR_A(r, 0) = pc;
	GR_SR(r) = (GR_SR(r) & ~0x1f) | flags;
	GR_USP(r) = result;
	GR_PC(r) = pc;
	return 1;
}

/* 1 if _FSMDispatch is _Unimplemented */
static int
fsmunimpl()
{
	unsigned char b[4], u[4];

	if (copyin((caddr_t)(0x400 + 4 * 0xac), (caddr_t)b, 4) ||
	    copyin((caddr_t)(TOOLTAB + 4 * 0x9f), (caddr_t)u, 4))
		return 0;
	return G32(b) == G32(u);
}

/*
 * Between the loader's passes: walks its table (a condition, then entries
 * of a delta byte or $FF and a delta word, and a trap word; $FE starts the
 * next group) to the File Manager's groups and makes each of them "not
 * under A/UX".  Pass 1 has loaded their code, so jump table entries into
 * it stay good; pass 2 then installs none of their traps, vectors or
 * routines.  1 if it found and marked them.
 */
static int
lpgroup(r)
	char *r;
{
	unsigned char f[4], t[LODRSCAN];
	long sp = GR_USP(r), tab;
	int p = 3, i;

	if (copyin((caddr_t)(sp + LODRTAB), (caddr_t)f, 4))
		return 0;
	tab = G32(f);
	if (copyin((caddr_t)tab, (caddr_t)t, sizeof t))
		return 0;
	while (p + sizeof fmgroups <= sizeof t) {
		if (t[p] == 0xfe) {
			if (!bcmp((char *)t + p, (char *)fmgroups, sizeof fmgroups))
				break;
			p += 4;
		} else if (t[p] == 0xff)
			p += 5;
		else
			p += 3;
	}
	if (p + sizeof fmgroups > sizeof t)
		return 0;
	for (i = 0; i < sizeof fmgroups; i += 3)
		if (t[p + i] == 0xfe) {
			t[p + i + 2] |= NOTAUX;
			i++;
		}
	return !copyout((caddr_t)t + p, (caddr_t)(tab + p), sizeof fmgroups);
}

static long rzpc;		/* the _SetApplBase whose _RsrcZoneInit ran */

/*
 * The Finder's _HFSDispatch calls by selector (async << 16 | sync); its
 * last 64 A-lines (pc, trap, d0) up to its first open of "Desktop", and
 * then the first VCB.
 */

/*
 * The Finder's stack at its first PBDTGetPath (1) and _ExitToShell (2): 15
 * usp longs, the pc, then per a6 frame its return pc, first argument, the
 * argument's longs at +$140, and 28 code bytes before the return pc.
 */

/* 1: the System file's resource map is open (SysMapHndl) */
static int
sysmap()
{
	long h;

	return !copyin((caddr_t)0xa54, (caddr_t)&h, 4) && h != 0 && h != -1;
}

/*
 * The Finder looks for each volume's Trash by name in its root, where HFS
 * keeps it; Patch.067C's FindFolder puts the startup volume's elsewhere.
 * A root lookup that fails with fnfErr asks FindFolder for the volume's
 * Trash and, if that folder has the name looked for, answers with it as a
 * folder of the root.  Three stubs: the lookup, FindFolder, then
 * PBGetCatInfo of the Trash itself.  Their data past the frame:
 */
#define	TNAME	0		/* the name looked for, 64 bytes */
#define	TVREF	64		/* ioVRefNum << 16 | the volume's vRefNum */
#define	TDIR	68		/* the Trash's vRefNum << 16, then its dirID */
#define	TEXT	76

static unsigned short hgetcat[] = { 0xa260, 0x4e71, 0x4e71, 0x4e71, 0x4e71, 0x4e71 };

static int
lower(c)
	int c;
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

/* the lookup, through a stub; 1 if it runs */
static int
trashcat(r, tw)
	char *r;
	int tw;
{
	unsigned char pb[0x34];
	unsigned short code[6];
	int i;

	if (trashbusy || copyin((caddr_t)GR_A(r, 0), (caddr_t)pb, sizeof pb) ||
	    G16(pb + 0x1c) || G32(pb + 0x30) != 2 || !G32(pb + 0x12) ||
	    !extvcb(GR_A(r, 0), 9))
		return 0;
	code[0] = tw;
	for (i = 1; i < 6; i++)
		code[i] = 0x4e71;
	return ui_auxcall(r, 0, 0L, code, MCAT, 0);
}

/*
 * RESUME of a Trash stub (mark m, frame f): 1 if the next one runs, 2 if
 * that is FindFolder (which may take stubs of its own), else 0 with the
 * result in *d0
 */
static int
trashnext(r, m, f, d0)
	char *r;
	int m;
	long f, *d0;
{
	static unsigned short fold[] = {	/* lea f-18,sp; moveq #0,d0; _FindFolder */
		0x4ffa, 0xffe8, 0x7000, 0xa823, 0x4e71, 0x4e71
	};
	unsigned char pb[0x34], t[TEXT], nm[64], a[18];
	long a0 = GR_A(r, 0), v, g;
	int i;

	if (copyin((caddr_t)a0, (caddr_t)pb, sizeof pb)) {
		if (m != MCAT)
			*d0 = FNFERR;
		return 0;
	}
	if (m == MCAT) {
		if ((short)*d0 != FNFERR || !(v = extvcb(a0, 9)) ||
		    copyin((caddr_t)G32(pb + 0x12), (caddr_t)nm, sizeof nm) || !nm[0] || nm[0] > 63)
			return 0;
		bzero((char *)t, sizeof t);
		bcopy((char *)nm, (char *)t + TNAME, nm[0] + 1);
		bcopy((char *)pb + 0x16, (char *)t + TVREF, 2);
		(void)copyin((caddr_t)(v + 0x4e), (caddr_t)t + TVREF + 2, 2);
		/* FindFolder(vRefNum, 'trsh', false, &vRefNum, &dirID) below the frame */
		g = (GR_USP(r) - FSIZE - TEXT) & ~3L;
		P32(a, g + FSIZE + TDIR + 4);
		P32(a + 4, g + FSIZE + TDIR);
		P16(a + 8, 0);
		P32(a + 10, 0x74727368);
		bcopy((char *)t + TVREF + 2, (char *)a + 14, 2);
		P16(a + 16, 0);
		if (copyout((caddr_t)a, (caddr_t)(g - 18), sizeof a) ||
		    !ui_auxcall(r, 0, 0L, fold, MFOLD, TEXT))
			return 0;
		(void)copyout((caddr_t)t, (caddr_t)(g + FSIZE), sizeof t);
		trashbusy = 1;
		return 2;
	}
	if (copyin((caddr_t)(f + FSIZE), (caddr_t)t, sizeof t) || t[TNAME] > 63) {
		*d0 = FNFERR;
		return 0;
	}
	if (m == MFOLD) {
		if (copyin((caddr_t)(f - 2), (caddr_t)a, 2) || G16(a) || !G32(t + TDIR + 4))
			return trashundo(r, t, d0);
		P16(pb + 0x16, G16(t + TDIR));
		P16(pb + 0x1c, -1);
		bcopy((char *)t + TDIR + 4, (char *)pb + 0x30, 4);
		if (copyout((caddr_t)pb, (caddr_t)a0, sizeof pb) ||
		    !ui_auxcall(r, 0, 0L, hgetcat, MTRASH, TEXT) ||
		    copyout((caddr_t)t, (caddr_t)(GR_USP(r) + FSIZE), sizeof t))
			return trashundo(r, t, d0);
		return 1;
	}
	/* MTRASH: the Trash, if it has the name looked for */
	if ((short)*d0 || copyin((caddr_t)G32(pb + 0x12), (caddr_t)nm, sizeof nm) ||
	    nm[0] > 63 || nm[0] != t[TNAME])
		return trashundo(r, t, d0);
	for (i = 1; i <= nm[0]; i++)
		if (lower(nm[i]) != lower(t[TNAME + i]))
			return trashundo(r, t, d0);
	if (!copyin((caddr_t)(a0 + 0x64), (caddr_t)a, 4)) {
		trashpar = G32(a);
		trashdir = G32(t + TDIR + 4);
		trashvref = G16(t + TVREF + 2);
	}
	bcopy((char *)t + TVREF, (char *)pb + 0x16, 2);
	P16(pb + 0x1c, 0);
	P32(a, 2);
	(void)copyout((caddr_t)pb, (caddr_t)a0, sizeof pb);
	(void)copyout((caddr_t)a, (caddr_t)(a0 + 0x64), 4);
	return 0;
}

/* the lookup's own result: fnfErr, its name and fields back */
static int
trashundo(r, t, d0)
	char *r;
	unsigned char *t;
	long *d0;
{
	unsigned char pb[0x34];
	long a0 = GR_A(r, 0);

	*d0 = FNFERR;
	if (t[TNAME] > 63 || copyin((caddr_t)a0, (caddr_t)pb, sizeof pb))
		return 0;
	bcopy((char *)t + TVREF, (char *)pb + 0x16, 2);
	P16(pb + 0x1c, 0);
	P32(pb + 0x30, 2);
	P16(pb + 0x10, FNFERR);
	if (!copyout((caddr_t)pb, (caddr_t)a0, sizeof pb))
		(void)copyout((caddr_t)t + TNAME, (caddr_t)G32(pb + 0x12), t[TNAME] + 1);
	return 0;
}

/*
 * The folder Patch.067C keeps the startup volume's Trash and Temporary
 * Items in, from its table of special folders (name, dirID, type for
 * 'temp', 'trsh', 'empt', 'desk'); 0 until Patch.067C has filled it in.
 */
static long
dotmac()
{
	unsigned char b[512 + 48];
	long a;
	int i;

	for (a = 0x4000; !spectab && a < 0x100000; a += 512) {
		if (copyin((caddr_t)a, (caddr_t)b, sizeof b))
			continue;
		for (i = 4; i < 512; i += 2)
			if (G32(b + i) == 0x74656d70 && G32(b + i + 12) == 0x74727368 &&
			    G32(b + i + 24) == 0x656d7074 && G32(b + i + 36) == 0x6465736b) {
				spectab = a + i;
				break;
			}
	}
	if (!spectab)
		spectab = -1;
	if (spectab < 0 || copyin((caddr_t)(spectab - 4), (caddr_t)b, 4))
		return 0;
	return G32(b);
}

/* the startup volume's VCB: the first of an external file system */
static long
uxvcb()
{
	unsigned char b[4];
	long v;
	int n;

	if (copyin((caddr_t)VCBQ, (caddr_t)b, 4))
		return 0;
	for (v = G32(b), n = 0; v; v = G32(b), n++) {
		if (n == 256 || (v & 1) || (unsigned long)v > 0x7fffff80 ||
		    copyin((caddr_t)(v + 0x4c), (caddr_t)b, 2))
			return 0;
		if (G16(b))
			return v;
		if (copyin((caddr_t)v, (caddr_t)b, 4))
			return 0;
	}
	return 0;
}

/*
 * A folder by its dirID (ioFDirIndex -1, or 0 without a name): the Trash
 * trashcat answered for, or any on the startup volume, through a stub, so
 * that the Trash and its neighbours have the root as their parent.
 */
static int
trashid(r, tw)
	char *r;
	int tw;
{
	unsigned char pb[0x34], b[2];
	unsigned short code[6];
	long v, d, m;
	int i;

	if (trashbusy || copyin((caddr_t)GR_A(r, 0), (caddr_t)pb, sizeof pb) ||
	    (short)G16(pb + 0x1c) > 0)
		return 0;
	if (!G16(pb + 0x1c) && G32(pb + 0x12) &&
	    (copyin((caddr_t)G32(pb + 0x12), (caddr_t)b, 1) || b[0]))
		return 0;
	d = G32(pb + 0x30);
	if (!(v = extvcb(GR_A(r, 0), 9)) || copyin((caddr_t)(v + 0x4e), (caddr_t)b, 2))
		return 0;
	if (!(trashdir && d == trashdir && (short)G16(b) == trashvref) &&
	    !(d > 2 && v == uxvcb() && (m = dotmac()) && d != m))
		return 0;
	code[0] = tw;
	for (i = 1; i < 6; i++)
		code[i] = 0x4e71;
	return ui_auxcall(r, 0, 0L, code, MTPAR, 0);
}

/* trashid's answer: the root as the parent */
static void
trashroot(a0)
	long a0;
{
	unsigned char pb[0x68];
	long p;

	if (copyin((caddr_t)a0, (caddr_t)pb, sizeof pb) || !(pb[0x1e] & 0x10))
		return;
	p = G32(pb + 0x64);
	if (!(trashdir && G32(pb + 0x30) == trashdir && p == trashpar) && (!p || p != dotmac()))
		return;
	P32(pb + 0x64, 2);
	(void)copyout((caddr_t)pb + 0x64, (caddr_t)(a0 + 0x64), 4);
}

/*
 * PBXGetVolInfo ($12), which only 8.1's File Manager knows: PBHGetVInfo,
 * then the byte counts from its block counts (xvolbytes).
 */
static int
xvolinfo(r, tw)
	char *r;
	int tw;
{
	static unsigned short code[] = { 0xa207, 0x4e71, 0x4e71, 0x4e71, 0x4e71, 0x4e71 };

	return !(tw & 0x400) && ui_auxcall(r, 0, 0L, code, MXVOL, 0);
}

/* n blocks of bs bytes as a 64-bit count at b */
static void
bytes64(b, n, bs)
	unsigned char *b;
	unsigned long n, bs;
{
	unsigned long lo = n * (bs & 0xffff), mid = n * (bs >> 16);

	P32(b, (mid >> 16) + ((lo + (mid << 16)) < lo));
	P32(b + 4, lo + (mid << 16));
}

static void
xvolbytes(a0)
	long a0;
{
	unsigned char pb[0x8a];

	if (copyin((caddr_t)a0, (caddr_t)pb, sizeof pb))
		return;
	bytes64(pb + 0x7a, (unsigned long)G16(pb + 0x2e), (unsigned long)G32(pb + 0x30));
	bytes64(pb + 0x82, (unsigned long)G16(pb + 0x3e), (unsigned long)G32(pb + 0x30));
	(void)copyout((caddr_t)pb + 0x7a, (caddr_t)(a0 + 0x7a), 16);
}

/* a sync _HFSDispatch that a stub serves: 1 if one runs */
static int
hfsspec(r, tw)
	char *r;
	int tw;
{
	int sel = GR_D(r, 0) & 0xffff;

	return sel == 9 ? trashcat(r, tw) || trashid(r, tw) : sel == 0x1b ? (tw & 0x200) && catspec(r) :
	    sel == 0x1a ? ui_opendf(r, tw) : sel == 0x12 ? xvolinfo(r, tw) :
	    extsel(sel) && ui_volparms(r, tw);
}

long uin_watchskipped;

/* Unrelated traps need neither native state reads nor redirect matching. */
static int
ui_trapinterest(gp,tw)
struct guest_proc *gp;
unsigned short tw;
{
    struct uipatch *p;int i;
    if(uin_soundopendiag && uin_soundopencount<16 && tw==0xa82a)return 1;
    if((gp->gp_flags&GPF_STUB) || !uinter_patches ||
        (uin_coffdiag && gp==coffdiagowner) ||
        (AUXP(gp)->ap_mac&APM_COFF) || ui_forceget() || ui_forcepollowned(gp))return 1;
    if(tw==RESUME || tw==HFSTRAP || tw==SPACETRAP || tw==0xa9f4 ||
        tw==0xa9c9 || tw==0xabf7 || tw==0xa80e || tw==0xa81f)return 1;
    if((gp->gp_flags&(GPF_DONE(NPATCH)-GPF_DONE(0)))==
        (GPF_DONE(NPATCH)-GPF_DONE(0)))return 1;
    for(i=0,p=uipatches;i<NPATCH;i++,p++) {
        if(p->p_kind==PSPEC && (tw&~0x200)==p->p_trap)return 1;
        if(p->p_kind==PRZONE && tw==p->p_trap)return 1;
        if(p->p_kind==PFSM && (tw&0xf8ff)==p->p_trap)return 1;
        if(gp->gp_flags&GPF_DONE(i))continue;
        if(p->p_kind==PSLEEPQ && (tw&0xf800)==0xa800)return 1;
        if(p->p_kind==PSOUND && ((tw&~0x400)==0xa800 || tw==0xa02e))return 1;
        if(p->p_kind==PHFS && (tw&~0x200)==p->p_trap)return 1;
        if(tw==p->p_trap)return 1;
    }
    return 0;
}

/* Temporary watchers release only their own claim on trap interception. */
int
ui_watchneeded(gp)
struct guest_proc *gp;
{
    unsigned char b[2];char world[16];int i,sv;
    struct uitask *t=ui_task(gp);
    if(!t || t->t_proc!=gp->gp_proc)return 0;
    if(gp->gp_flags&GPF_STUB)return 1;
    /* A foreign task's address space cannot be inspected by this caller. */
    if(gp->gp_proc!=u.u_procp)return 1;
    if(spacewatch(gp) || ui_forcepollowned(gp) || ui_forceget() || ui_coffworld(gp,world))return 1;
    if(uin_coffdiag && gp==coffdiagowner && (long)(lbolt-coffdiaguntil)<0 &&
        coffdiagpolls<=65536UL)return 1;
    if(pmwatchneeded(gp))return 1;
    if(!uinter_patches)return 0;
    if(copyin((caddr_t)0x15a,(caddr_t)b,2))return 1;
    sv=G16(b);
    if(uin_soundopendiag && uin_soundopencount<16 && sv==0x0810 &&
        gp==ui.l_gp && gp==ui.l_active && gp->gp_proc==u.u_procp)return 1;
    if(sv==0x0710 || (sv>>8)<6 || (sv>>8)>9 || (sv&0xf0)>0x90 || (sv&0xf)>9)return 1;
    for(i=0;i<NPATCH;i++)
        if(uipatches[i].p_sysver==sv && !(gp->gp_flags&GPF_DONE(i)))return 1;
    return 0;
}

/* an A-line of a watched task: 1 if a patch took it */
int
ui_alwatch(gp, r)
	struct guest_proc *gp;
	char *r;
{
	struct uipatch *p;
	unsigned char b[2], code[64];
	unsigned short sv, tw;
	long pc = GR_PC(r);
	int i, coffwatch, coffbound, early = 0, todo = gp->gp_flags & GPF_STUB;
    char coffworld[16];

	uinter_nalwatch++;
	if (copyin((caddr_t)pc, (caddr_t)b, 2))
		return 0;
    soundopenrecord(gp,r,G16(b));
    if(!ui_trapinterest(gp,G16(b))) {uin_watchskipped++;return 0;}
    coffwatch=coffdiagwatch(gp,r);
    coffbound=ui_coffworld(gp,coffworld);
    if((G16(b)==0xa9c9 || G16(b)==0xabf7) &&
        (coffbound || coffsyserrowned(gp)))coffsyserrrecord(r,G16(b));
    if(G16(b)==0xa9c9 || G16(b)==0xabf7)otproberecord(gp,r,G16(b));
    if(G16(b)==SPACETRAP && spacecall(gp,r,coffbound))return 1;
    if(coffbound) {coffviswriterecord(gp,r,pc,G16(b));coffconvertrecord(gp,r,pc,G16(b));cofffontafter(gp,r,pc);coffnatdrawrecord(r,pc,G16(b));cofftoolrecord(r,pc,G16(b));}
    if(coffwatch && coffbound) {
        long *sample=uin_cofftrace[uin_cofftracecount++ & 63];
        unsigned char top[4];
        coffalertrecord(r,pc,G16(b));
        sample[0]=pc;sample[1]=GR_USP(r);sample[2]=G16(b);
        sample[3]=GR_D(r,0);sample[4]=GR_A(r,0);sample[5]=GR_A(r,5);
        sample[6]=copyin((caddr_t)sample[1],(caddr_t)top,4) ? -1 : G32(top);
        sample[7]=copyin((caddr_t)0xaf0,(caddr_t)top,2) ? -1 : G16(top);
    }
	if (G16(b) == RESUME && aslmstep(gp,r))return 1;
	if (G16(b) == RESUME && (i = ui_resume(r, pc)) != 0) {
		if (i != 2)
			gp->gp_flags &= ~GPF_STUB;
		if (i == 3) ui_forceend(gp);
		return 1;
	}
	if (pc == hfst && G16(b) == HFSTRAP) {
		/* the trap word in d1 when the dispatcher calls; sync for glue */
		tw = (GR_D(r, 1) & 0xf9ff) == 0xa060 ? GR_D(r, 1) : 0xa260;
		if (!(gp->gp_flags & GPF_STUB) && !(tw & 0x400) && hfsspec(r, tw)) {
			gp->gp_flags |= GPF_STUB;
			return 1;
		}
		GR_PC(r) = pc + 4;
		return 1;
	}
	tw = G16(b);
    if(tw==0xabf7 && uin_pmidle) {
        if(pmidleowned(gp,r)) {
            if(uin_pmidlestub && !pmstate && pmstubstart(gp,r)) {gp->gp_flags|=GPF_STUB;return 1;}
            if(uin_pmidlestub && (pmstate==2 || pmstate==3))return 0;
            uin_pmidlecalls++;ui_pmidlewait(gp);return 0;
        }
        uin_pmidlerefused++;
    }
    if(coffbound && coffadapt(gp,r,tw,coffworld))return 1;
	if (tw == 0xa9f4) {
        if(ui_forcepollowned(gp) && ui_forceexit(gp)) {
            psignal(gp->gp_proc,SIGSEGV);
            return 1;
        }
		ui_forceend(gp);
		todo = gp->gp_flags & GPF_STUB;
	}
	if (copyin((caddr_t)0x15a, (caddr_t)b, 2))
		return 0;
	sv = G16(b);
    if(sv==0x0810 && (tw==0xa80e || tw==0xa81f) && !(gp->gp_flags & GPF_STUB) && aslmwrap(r)) {
        gp->gp_flags|=GPF_STUB;return 1;
    }

	ui_forceenable(uinter_patches && sv == 0x0810);
	if (ui_forceget()) {
		uin_forcealine++; uin_forceop = tw; uin_forcepc = pc;
		if ((tw & ~0x400) == 0xa860) uin_forcewne++;
	}
	/* Keep the resume path usable if forced exit discards the caller's stack. */
	if (uinter_patches && sv == 0x0810 && tw == 0xa860 &&
	    !(gp->gp_flags & GPF_STUB) && ui_forceget() && ui_forcecall(r)) {
		ui_forcestub(gp);
		return 1;
	}
	/*
	 * Until 'boot' 3 sets it, SysVersion is 0 (the ROM clears it) or the
	 * $0710 that 'PTCH' 0 of System 7.1 and later sets while it installs.
	 */
	if (sv == 0x0710 || (sv >> 8) < 6 || (sv >> 8) > 9 || (sv & 0xf0) > 0x90 ||
	    (sv & 0xf) > 9)
		todo = early = 1;
	for (i = 0; i < NPATCH; i++)
		if (uipatches[i].p_sysver == sv && !(gp->gp_flags & GPF_DONE(i)))
			todo = 1;
	if ((!uinter_patches && !spacewatch(gp)) || (!todo && !coffwatch && !coffbound && !spacewatch(gp) && !ui_forceget() && !ui_forcepollowned(gp) && !(pmwatchneeded(gp)) &&
        !(uin_soundopendiag && uin_soundopencount<16 && sv==0x0810))) {
		gp->gp_flags &= ~GPF_AWATCH;
		if (uinter_trace)
			printf("uinter: watch off after %d A-lines, SysVersion %x\n",
			    (int)uinter_nalwatch, sv);
		return 0;
	}
	if (!uinter_patches)
		return 0;
	for (i = 0, p = uipatches; p < uipatches + NPATCH; p++, i++) {
		if (p->p_kind == PRZONE && (p->p_sysver == sv || early) && tw == p->p_trap &&
		    !(gp->gp_flags & GPF_STUB) && sysmap()) {
			if (rzpc == pc) {
				rzpc = 0;
				continue;
			}
			if (!ui_auxcall(r, 0, 0L, rzinit, 0, 0))
				return 0;
			rzpc = pc;
			gp->gp_flags |= GPF_STUB;
			uinter_patched |= 1L << i;
			return 1;
		}
		if (p->p_kind == PSLEEPQ && p->p_sysver == sv && !(gp->gp_flags & GPF_DONE(i)) &&
		    (tw & 0xf800) == 0xa800 && !(gp->gp_vsr & 0x700)) {
			gp->gp_flags |= GPF_DONE(i);
			if (ui_insys(0xbbc))
				continue;
			if (!ui_auxcall(r, 0, 0L, sleepq, 0, 0))
				return 0;
			gp->gp_flags |= GPF_STUB;
			uinter_patched |= 1L << i;
			if (uinter_trace)
				printf("uinter: patch %d at %x\n", i, (int)pc);
			return 1;
		}
		if (p->p_kind == PSOUND && p->p_sysver == sv && !(gp->gp_flags & GPF_DONE(i)) &&
		    ((tw & ~0x400) == 0xa800 || tw == 0xa02e) && !(gp->gp_vsr & 0x700)) {
			int state = ui_unimpl();
			if (state < 0)
				return 0;
			if (!state) {
                int native = soundnative(gp);
                if(native<0)return 1;
                if(native)gp->gp_flags |= GPF_DONE(i);
				if (uinter_trace)
					printf("uinter: _SoundDispatch there at %x\n", (int)pc);
				continue;
			}
            if ((tw & ~0x400) != 0xa800)continue;
			if (!soundearly(r, tw))
				return 0;
			uinter_patched |= 1L << i;
			return 1;
		}
		if (p->p_kind == PSPEC && p->p_sysver == sv && (tw & ~0x200) == p->p_trap &&
		    !(gp->gp_flags & GPF_STUB) && hfsspec(r, tw)) {
			gp->gp_flags |= GPF_STUB;
			uinter_patched |= 1L << i;
			return 1;
		}
		if (p->p_kind == PFSM && p->p_sysver == sv && (tw & 0xf8ff) == p->p_trap &&
		    fsmunimpl()) {
			GR_D(r, 0) = FSMNOFFS;
			GR_SR(r) = (GR_SR(r) & ~0xf) | 8;
			GR_PC(r) = pc + 2;
			uinter_patched |= 1L << i;
			return 1;
		}
		if (p->p_kind == PHFS && p->p_sysver == sv && !(gp->gp_flags & GPF_DONE(i)) &&
		    (tw & ~0x200) == p->p_trap && !(gp->gp_flags & GPF_STUB) &&
		    !(gp->gp_vsr & 0x700)) {
			gp->gp_flags |= GPF_DONE(i);
			if (!ui_auxcall(r, 0, 0L, hfsalloc, MHFS, 0))
				return 0;
			gp->gp_flags |= GPF_STUB;
			uinter_patched |= 1L << i;
			return 1;
		}
		if (p->p_kind == PSLEEPQ || p->p_kind == PSOUND || p->p_kind == PSPEC ||
		    p->p_kind == PRZONE || p->p_kind == PHFS || p->p_kind == PFSM ||
		    (p->p_sysver != sv && !(early && p->p_kind == PGROUP)) ||
		    (gp->gp_flags & GPF_DONE(i)) ||
		    tw != p->p_trap || p->p_len > sizeof code ||
		    copyin((caddr_t)(pc - p->p_at), (caddr_t)code, p->p_len) ||
		    bcmp((char *)code, (char *)p->p_code, p->p_len))
			continue;
		if (p->p_kind == PHFSINIT) {
			long walk = pc - p->p_at + HFSWALK;
			int volumes;

			if (copyin((caddr_t)walk, (caddr_t)code, sizeof hfswalk) ||
			    bcmp((char *)code, (char *)hfswalk, sizeof hfswalk))
				continue;
			volumes = hfsvolumes();
			if (volumes < 0)
				continue;
			if (volumes) {
				GR_PC(r) = pc - p->p_at + 0x20;
				gp->gp_flags |= GPF_DONE(i);
				uinter_patched |= 1L << i;
				return 1;
			}
			/* Skipped VCBs still advance through the queue. */
			code[15] = 0x0e;
			code[23] = 0x06;
			if (copyout((caddr_t)code, (caddr_t)walk, sizeof hfswalk))
				continue;
			dlm_cacheflush();
			gp->gp_flags |= GPF_DONE(i);
			uinter_patched |= 1L << i;
			return 0;
		} else if (p->p_kind == PGROUP) {
			if (!lpgroup(r))
				continue;
			/* either trap; both count as one entry, the first */
			for (i = NPATCH - 1; i >= 0; i--)
				if (uipatches[i].p_kind == PGROUP) {
					gp->gp_flags |= GPF_DONE(i);
					tw = i;
				}
			uinter_patched |= 1L << tw;
			if (uinter_trace)
				printf("uinter: patch %d at %x\n", (int)(p - uipatches), (int)pc);
			return 0;
		} else if (p->p_kind == PFIX) {
			if (copyout((caddr_t)tmhead, (caddr_t)(pc - p->p_at), sizeof tmhead))
				continue;
			dlm_cacheflush();
			gp->gp_flags |= GPF_DONE(i);
			uinter_patched |= 1L << i;
			if (uinter_trace)
				printf("uinter: patch %d at %x\n", i, (int)pc);
			return 0;
		} else if (p->p_kind == PCALL) {
			if (!ui_auxcall(r, (int)(p->p_to >> 16), p->p_to & 0xffff, (unsigned short *)0, 0, 0))
				return 0;
			gp->gp_flags |= GPF_STUB;
		} else
			GR_PC(r) = pc - p->p_at + p->p_to;
		gp->gp_flags |= GPF_DONE(i);
		uinter_patched |= 1L << i;
		if (uinter_trace)
			printf("uinter: patch %d at %x\n", i, (int)pc);
		return 1;
	}
	return 0;
}
