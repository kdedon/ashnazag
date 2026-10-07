/*
 * bdos3.h -- the CP/M 3 BDOS for CP/M-68K, over Unix files.
 *
 * Guest memory is reached through b3_mem (0 when CP/M is mapped at 0);
 * every multi-byte field in it is read byte by byte, big-endian as the
 * 68000 keeps it, except the fields CP/M 3 defines little-endian.
 */

#define	B3MEM(a)	(b3_mem + (unsigned long)(a))
#define	NDRV		16
#define	NUSER		16
#define	RECLEN		128
#define	MAXREC		262144L		/* 32 MB of records */

/* console control characters */
#define	CTRLA	0x01
#define	CTRLB	0x02
#define	CTRLC	0x03
#define	CTRLE	0x05
#define	CTRLF	0x06
#define	CTRLG	0x07
#define	BS	0x08
#define	TAB	0x09
#define	LF	0x0a
#define	CTRLK	0x0b
#define	CR	0x0d
#define	CTRLP	0x10
#define	CTRLQ	0x11
#define	CTRLR	0x12
#define	CTRLS	0x13
#define	CTRLU	0x15
#define	CTRLW	0x17
#define	CTRLX	0x18
#define	RUB	0x7f

/* function 109 console mode */
#define	CM_CTLC		0x0001	/* status reports only a waiting ^C */
#define	CM_NOSTOP	0x0002	/* no ^S/^Q */
#define	CM_NOTERM	0x0008	/* ^C does not end the program */
#define	CM_RAW		0x0014	/* no tab expansion, no printer echo */

#define	RC_CTLC		0xfffe	/* return code: ended by ^C */
#define	RC_BDOS		0xfffd	/* return code: ended by a BDOS error */

/* password modes (function 102, 103) */
#define	XP_READ		0x80
#define	XP_WRITE	0x40
#define	XP_DELETE	0x20

/* what a host file looks like to CP/M */
struct hfile {
	char		name[11];	/* 8.3, upper case, blank padded */
	int		user;
	int		sys;		/* hidden: a leading dot on the host */
	int		ro;		/* not writable by us */
	int		arc;		/* archived: others' execute bit */
	int		pwmode;		/* XP_* from the group/other bits */
	long		size;
	long		atime, mtime;
	char		host[256];	/* the name in its directory */
};

/* guest memory and the BIOS below the BDOS */
extern char *b3_mem;
extern int b3_conin(), b3_const();
extern void b3_conout(), b3_list(), b3_wboot();
extern long b3_bios();

/* bdos3.c */
extern long cpm_bdos3();
extern void b3_init();
extern unsigned long b3_get16(), b3_get32();
extern void b3_put16(), b3_put32();
extern int b3_retcode;
extern unsigned long b3_dma;

/* con3.c */
extern int b3_column, b3_conwidth, b3_conpage, b3_conline, b3_pagemode;
extern int b3_conmode, b3_delim, b3_lstecho;
extern int b3_constat(), b3_conin3(), b3_getch(), b3_rawio();
extern void b3_cookdout(), b3_conout3(), b3_prtline(), b3_readline(), b3_prtblk();
extern void b3_chain();

/* hostfs.c */
extern char b3_root[1024];
extern int hf_drives(), hf_dir(), hf_scan(), hf_find(), hf_create();
extern int hf_open(), hf_unlink(), hf_rename(), hf_setattr(), hf_protect();
extern int hf_truncate(), hf_name(), hf_mkdist();
extern long hf_free();
extern void hf_flush(), hf_stamp();
