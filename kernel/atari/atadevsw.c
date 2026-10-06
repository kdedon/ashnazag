/*
 * Falcon device switches.  Same majors and sizes as the base kernel
 * (shadowcsw/shadowbsw and the counts are sized from it); the rows of
 * Amiga hardware drivers are empty.  Rows 18 and 40 are the disk driver
 * over the IDE layer.  Block row 20 is the RAM disk; row 42 (raw RAM
 * disk) is filled when the RAM disk is configured.  Character row 18,
 * the Amiga Ethernet's major, is the cartridge-port Ethernet.
 */

#include "sys/types.h"
#include "sys/param.h"
#include "sys/conf.h"
#include "sys/stream.h"

extern int nodev(), nulldev();
#define ND	nodev
#define notty	(struct tty *)0
#define nostr	(struct streamtab *)0

static int oldflag[1] = { D_OLD };
static int nullflag[1] = { 0 };

extern int prfopen(), prfclose(), prfread(), prfwrite(), prfioctl();
extern int syopen(), syread(), sywrite(), syioctl();
extern int mmopen(), mmclose(), mmread(), mmwrite(), mmioctl(), mmmmap(),
	mmsegmap();
extern int ddopen(), ddclose(), ddstrategy(), ddprint(), ddsize(),
	ddread(), ddwrite(), ddioctl();
extern int ramopen(), ramclose(), ramstrategy(), ramprint(), ramsize();
extern int rtcread(), rtcwrite();
extern int ds_fbopen(), ds_fbclose(), ds_fbread(), ds_fbioctl(), ds_fbmmap(),
	ds_segmap(), ds_fbpoll();
extern int ds_evopen(), ds_evclose(), ds_evread(), ds_evioctl(), ds_evpoll();

extern struct streamtab coinfo, nxtinfo, nsxtinfo, ptsinfo, ptminfo;
extern struct streamtab sldinfo, loopinfo, timinfo, trwinfo, loginfo;
extern struct streamtab spinfo, clninfo, tcoinfo, tcooinfo, tclinfo;
extern struct streamtab ipinfo, tcpinfo, udpinfo, ripinfo, icmpinfo;
extern struct streamtab arpinfo, sadinfo, nuinfo;

#define B_NONE	{ ND, ND, ND, ND, ND, ND, ND, nullflag }
#define C_NONE	{ ND, ND, ND, ND, ND, ND, ND, ND, ND, ND, notty, nostr, nullflag }
#define C_STR(s, f)	{ ND, ND, ND, ND, ND, ND, ND, ND, ND, ND, notty, s, f }

struct bdevsw bdevsw[32] = {
/* open close strategy print size xpoll xhalt flag */
	B_NONE, B_NONE, B_NONE, B_NONE,			/* 0-3 */
	B_NONE, B_NONE, B_NONE, B_NONE,			/* 4-7 */
	B_NONE, B_NONE, B_NONE, B_NONE,			/* 8-11 */
	B_NONE, B_NONE, B_NONE, B_NONE,			/* 12-15 */
	B_NONE,						/* 16 Amiga floppy */
	B_NONE,						/* 17 A2090 disk */
	{ ddopen, ddclose, ddstrategy, ddprint, ddsize, ND, ND, nullflag },
	B_NONE,						/* 19 */
	{ ramopen, ramclose, ramstrategy, ramprint, ramsize, ND, ND, nullflag },
	B_NONE, B_NONE, B_NONE,				/* 21-23 */
	B_NONE, B_NONE, B_NONE, B_NONE,			/* 24-27 */
	B_NONE, B_NONE, B_NONE, B_NONE,			/* 28-31 */
};

struct cdevsw cdevsw[70] = {
/* open close read write ioctl mmap segmap poll xpoll xhalt ttys stream flag */
	C_STR(&coinfo, nullflag),			/* 0 console */
	{ prfopen, prfclose, prfread, prfwrite, prfioctl,
		ND, ND, ND, ND, ND, notty, nostr, nullflag },	/* 1 prf */
	{ syopen, nulldev, syread, sywrite, syioctl,
		ND, ND, ND, ND, ND, notty, nostr, nullflag },	/* 2 tty */
	{ mmopen, mmclose, mmread, mmwrite, mmioctl,
		mmmmap, mmsegmap, ND, ND, ND, notty, nostr, nullflag }, /* 3 mem */
	C_NONE,						/* 4 bb */
	C_NONE,						/* 5 sl */
	C_NONE,						/* 6 amiga */
	C_NONE,						/* 7 */
	C_STR(&nxtinfo, oldflag),			/* 8 xt */
	C_STR(&nsxtinfo, oldflag),			/* 9 sxt */
	C_NONE,						/* 10 screen */
	C_NONE,						/* 11 scsi */
	C_NONE,						/* 12 machid */
	C_NONE,						/* 13 ql */
	C_STR(&ptsinfo, oldflag),			/* 14 pts */
	C_STR(&ptminfo, oldflag),			/* 15 ptmx */
	C_NONE,						/* 16 ct */
	C_NONE,						/* 17 fd */
	C_STR(&nuinfo, nullflag),			/* 18 aen */
	C_STR(&sldinfo, nullflag),			/* 19 slip */
	C_STR(&loopinfo, oldflag),			/* 20 loop */
	C_NONE,						/* 21 par */
	C_NONE,						/* 22 tiga */
	C_STR(&timinfo, nullflag),			/* 23 timod */
	C_STR(&trwinfo, oldflag),			/* 24 tirdwr */
	C_STR(&loginfo, nullflag),			/* 25 log */
	C_STR(&spinfo, oldflag),			/* 26 sp */
	C_STR(&clninfo, nullflag),			/* 27 clone */
	C_STR(&tcoinfo, oldflag),			/* 28 ticots */
	C_STR(&tcooinfo, oldflag),			/* 29 ticotsord */
	C_STR(&tclinfo, oldflag),			/* 30 ticlts */
	C_NONE,						/* 31 res */
	C_STR(&ipinfo, oldflag),			/* 32 ip */
	C_STR(&tcpinfo, oldflag),			/* 33 tcp */
	C_STR(&udpinfo, oldflag),			/* 34 udp */
	C_STR(&ripinfo, oldflag),			/* 35 rawip */
	C_STR(&icmpinfo, oldflag),			/* 36 icmp */
	C_STR(&arpinfo, oldflag),			/* 37 arp */
	C_NONE, C_NONE,					/* 38-39 */
	{ ddopen, ddclose, ddread, ddwrite, ddioctl,
		ND, ND, ND, ND, ND, notty, nostr, nullflag },	/* 40 dd */
	{ nulldev, nulldev, rtcread, rtcwrite, ND,
		ND, ND, ND, ND, ND, notty, nostr, nullflag },	/* 41 clock */
	C_NONE,						/* 42 raw RAM disk */
	C_NONE, C_NONE, C_NONE,				/* 43-45 */
	C_NONE,						/* 46 audio */
	C_NONE, C_NONE, C_NONE,				/* 47-49 */
	C_STR(&sadinfo, nullflag),			/* 50 sad */
	{ ds_fbopen, ds_fbclose, ds_fbread, ND, ds_fbioctl,
		ds_fbmmap, ds_segmap, ds_fbpoll, ND, ND, notty, nostr, nullflag }, /* 51 fb */
	{ ds_evopen, ds_evclose, ds_evread, ND, ds_evioctl,
		ND, ND, ds_evpoll, ND, ND, notty, nostr, nullflag },	/* 52 kbd */
	{ ds_evopen, ds_evclose, ds_evread, ND, ds_evioctl,
		ND, ND, ds_evpoll, ND, ND, notty, nostr, nullflag },	/* 53 mouse */
	C_NONE,						/* 54 */
	C_NONE, C_NONE, C_NONE, C_NONE, C_NONE,		/* 55-59 */
	C_NONE, C_NONE, C_NONE, C_NONE, C_NONE,		/* 60-64 */
	C_NONE, C_NONE, C_NONE, C_NONE, C_NONE,		/* 65-69 */
};
