/* The disk driver's SCSI command block. */
#ifndef _AMIGA_SD_H
#define _AMIGA_SD_H

#define sdspl	spl2
#define	SDCARDS	2
#define	SDUNITS	8

struct sdcom {
	volatile struct sdcom	*next;
	char		reading;
	char		okay;	/* the command ran; status is valid */
	unsigned char	status;
	unsigned char	cdb[12];
	caddr_t		addr;
	unsigned	nbyte;
	unsigned	card;
	unsigned	unit;
	void		(*intr)();	/* (cp) on completion */
};

#endif
