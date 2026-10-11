/*
 * cpm.h -- CP/M-68K C programs: BDOS numbers, FCB and base page.
 * Built with 16-bit ints; pointers and LONG parameters are 32 bits.
 */

#define	BDOS_WBOOT	0
#define	BDOS_CONIN	1
#define	BDOS_CONOUT	2
#define	BDOS_PRINTSTR	9
#define	BDOS_RDCONBUF	10
#define	BDOS_CONST	11
#define	BDOS_RESET	13
#define	BDOS_SELDSK	14
#define	BDOS_OPEN	15
#define	BDOS_CLOSE	16
#define	BDOS_SFIRST	17
#define	BDOS_SNEXT	18
#define	BDOS_DELETE	19
#define	BDOS_READSEQ	20
#define	BDOS_WRITESEQ	21
#define	BDOS_MAKE	22
#define	BDOS_RENAME	23
#define	BDOS_CURDSK	25
#define	BDOS_SETDMA	26
#define	BDOS_ROVEC	29
#define	BDOS_SETATTR	30
#define	BDOS_GETDPB	31
#define	BDOS_READRAN	33
#define	BDOS_WRITERAN	34
#define	BDOS_SETRAN	36
#define	BDOS_SETMULTI	44
#define	BDOS_ERRMODE	45
#define	BDOS_FREESP	46
#define	BDOS_BIOSCALL	50
#define	BDOS_FREEBLK	98
#define	BDOS_SETLABEL	100
#define	BDOS_GETLABEL	101
#define	BDOS_RDSTAMPS	102
#define	BDOS_SETTIME	104
#define	BDOS_GETTIME	105
#define	BDOS_SERIAL	107
#define	BDOS_RETCODE	108
#define	BDOS_CONMODE	109
#define	BDOS_OUTDELIM	110
#define	BDOS_PRTBLK	111
#define	BDOS_LSTBLK	112

/* function 101's label mode bits */
#define	DL_PASSWD	0x80
#define	DL_ACCESS	0x40
#define	DL_UPDATE	0x20
#define	DL_CREATE	0x10
#define	DL_EXISTS	0x01

/* function 50: a BIOS function and its two parameters */
struct biospb {
	int	code;
	long	p1;
	long	p2;
};

#define	BIOS_REFUSED	0xffffffffL
#define	BIOS_CONOUT	4

/* function 45 */
#define	ERRMODE_DEFAULT	0x00
#define	ERRMODE_RETURN	0xff
#define	ERRMODE_DISPRET	0xfe

/* functions 111 and 112 */
struct ccb {
	long	cbaddr;
	unsigned cblen;
};

#define	SECLEN	128

struct fcb {
	char	drvcode;	/* 0 default, 1..16 A..P */
	char	fname[8];
	char	ftype[3];
	char	extent;
	char	s1, s2;
	char	rcdcnt;
	char	dskmap[16];
	char	cur_rec;
	char	ran0, ran1, ran2;
};

struct bpage {
	long	ltpa;
	long	htpa;
	long	lcode;
	long	codelen;
	long	ldata;
	long	datalen;
	long	lbss;
	long	bsslen;
	long	freelen;
	char	resvd1[20];
	struct fcb fcb2;
	struct fcb fcb1;
	char	buff[128];	/* command tail, then the default DMA */
};

#ifndef	VOID
#define	VOID	int
#endif

extern struct bpage *_base;

extern int	__bdos();	/* (fn, long param) */
extern long	__bdosl();
extern long	__bios();	/* (fn, long p1, long p2), trap #3 */

extern int	conout();
extern int	conin();
extern VOID	cputs();
extern VOID	conputs();
extern VOID	printstr();
extern VOID	putdec();
extern VOID	setdma();
extern VOID	mkfcb();
