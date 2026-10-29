/*
 * The custom chip and CIA-A registers paula.c uses.
 */
#ifndef _AMIGAHR_H
#define _AMIGAHR_H

typedef struct pcb pcb_t;

struct amiga_audio {
	__signed__ char	*buf;		/* AUDxLC */
	unsigned short	size, period, loud, data;
	unsigned short	_pad[2];
};

struct amiga {
	unsigned short	_r0[2];
	unsigned short	vposr, vhposr;	/* 0x004 */
	unsigned short	_r1[71];
	unsigned short	dmacon;		/* 0x096 */
	unsigned short	_r2;
	unsigned short	intena, intreq;	/* 0x09a */
	unsigned short	_r3;
	struct amiga_audio audio[4];	/* 0x0a0 */
};

/* one byte every 256 */
struct acia {
	unsigned char	pra, _p0[255];
	unsigned char	prb, _p1[255];
	unsigned char	ddra, _p2[255];
	unsigned char	ddrb, _p3[255];
	unsigned char	talo, _p4[255];
	unsigned char	tahi, _p5[255];
	unsigned char	tblo, _p6[255];
	unsigned char	tbhi, _p7[255];
	unsigned char	todlo, _p8[255];
	unsigned char	todmid, _p9[255];
	unsigned char	todhi, _p10[255];
	unsigned char	_rb, _p11[255];
	unsigned char	sdr, _p12[255];
	unsigned char	icr, _p13[255];
	unsigned char	cra, _p14[255];
	unsigned char	crb;
};

#define AMIGA	((__volatile__ struct amiga *)0xdff000)
#define ACIAA	((__volatile__ struct acia *)0xbfe001)

/* Agnus or Alice ID bit 4: NTSC */
#define PAL	((AMIGA->vposr & 0x1000) == 0)

#define DMASET	0x8000
#define DMACLR	0
#define DMAAUD0	0x0001
#define DMAAUD1	0x0002
#define DMAAUD2	0x0004
#define DMAAUD3	0x0008
#define AINTCLR	0
#define AIEAUD0	0x0080
#define AIEAUD1	0x0100
#define AIEAUD2	0x0200
#define AIEAUD3	0x0400

#define ICR_TA	0x01
#define ICR_TB	0x02
#define ICR_SP	0x08
#define ICR_FLG	0x10
#define ICR_SET	0x80
#define ICR_CLR	0

#endif
