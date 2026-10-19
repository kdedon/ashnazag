/*
 * The base's SCSI controllers on the disk interface, and boot modules
 * from the loader.  The A4000's IDE port holds card 0 for its module,
 * so a CyberStorm's SCSI at 0xF40000 is card 1 either way.  The kernel
 * finds no A3000 SCSI on an A4000 although autoconfig lists one.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "rico.h"
#include "sd.h"
#include "sys/bootmod.h"

#define A3000SCSI	0x0202F003
#define VPOSR		((__volatile__ ushort *)0xDFF004)
#define G32(p)		(*(unsigned long *)(p))
#define BI_BMOD		0x66C		/* bootinfo.reserved */

extern bool a3091queue(), a2090queue(), a2091queue();
extern int autocon();

char	*sd_modnames[] = { "amigaide", "ncr770", 0 };

static struct {
	uint	pn;
	bool	(*f)();
	char	*name;
} cards[] = {
	A3000SCSI,	a3091queue,	"A3000 Internal SCSI",
	0x02020001,	a2090queue,	"A2090 SCSI",
	0x02020003,	a2091queue,	"A2091 SCSI",
};

void
sd_hostinit()
{
	char *a, *o;
	int i, c, aga;

	aga = (*VPOSR >> 8 & 0x0F) >= 2;
	for (i = 0; i < nel(cards); i++) {
		if (cards[i].pn == A3000SCSI && aga)
			continue;
		for (c = 0; autocon(cards[i].pn, c, &a, &o); c++)
			(void)sd_attach((int (*)())0, cards[i].f, c, a,
			    cards[i].name);
	}
	if (aga)
		(void)sd_attach((int (*)())0, (bool (*)())0, 0,
		    (char *)0xDA0000, "A4000 IDE");
}

/* bootinfo.reserved: 'BMOD' and the loader's table */
void
amiga_boottab(bi)
char *bi;
{
	if (G32(bi + BI_BMOD) == BM_MAGIC)
		dlm_boottab((char *)G32(bi + BI_BMOD + 4));
}
