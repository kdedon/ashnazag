/*
 * SCSI host adapter selection with the A4000's IDE port as one more
 * adapter.  Replaces the kernel's own table walk, which knows no IDE and
 * on an A4000 registers an A3000 SCSI controller that is not there.
 * Adapters are numbered by address, so the IDE port is card 0 on an
 * A4000 and its master, answering as unit 6, is c6d0.  A CyberStorm's
 * SCSI, at 0xF40000, is always card 1: its unit 6 is ced0.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "rico.h"
#include "sd.h"

#define A3000SCSI	0x0202F003
#define VPOSR		((__volatile__ ushort *)0xDFF004)

extern bool a3091queue(), a2090queue(), a2091queue();
extern int idepresent(), idequeue(), autocon();
extern bool csppcqueue();
extern int csppcpresent();
extern void ideintr(), ide_cpush(), csppcintr();
extern void (*int2_tbl[])();

/* ide.c runs interrupt driven only when it finds itself in this table */
void (*ide_int2_tbl[])() = { ideintr, 0 };

static struct {
	uint	pn;
	bool	(*f)();
	char	*name;
} cards[] = {
	A3000SCSI,	a3091queue,	"A3000 Internal SCSI",
	0x02020001,	a2090queue,	"A2090 SCSI",
	0x02020003,	a2091queue,	"A2091 SCSI",
};

static struct {
	bool	(*f)();
	uint	c;
	char	*a;
	char	*name;
} q[SDCARDS];

static int inited, ide, csppc;
static void (*int2_prev)();

/* PIO goes through the uncached identity window; keep the cache out of it */
/* an A4000 IDE port that did not answer still takes card 0 */
static bool
noq(c, cp)
struct sdcom *cp;
{
	cp->okay = FALSE;
	(*cp->intr)(cp);
	return TRUE;
}

static bool
ideq(c, cp)
struct sdcom *cp;
{
	ide_cpush(cp->addr, cp->nbyte);
	return idequeue(c, cp);
}

/* takes the level 2 table's last slot, as the PCI hook does */
static void
ide_int2(pcb)
char *pcb;
{
	(*int2_prev)(pcb);
	if (ide)
		ideintr();
	if (csppc)
		csppcintr();
}

static void
ide_hook()
{
	int i, s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (s) : : "memory");
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s | 0x700) : "memory");
	for (i = 0; int2_tbl[i + 1]; i++)
		;
	int2_prev = int2_tbl[i];
	int2_tbl[i] = ide_int2;
	__asm__ __volatile__("mov.w %0,%%sr" : : "d" (s) : "memory");
}

static void
insert(f, c, a, name)
bool (*f)();
char *a, *name;
{
	int i;

	if (q[SDCARDS - 1].f) {
		printf("sd: too many controllers\n");
		return;
	}
	for (i = SDCARDS - 1; i > 0 && (!q[i - 1].f || q[i - 1].a > a); i--)
		q[i] = q[i - 1];
	q[i].f = f;
	q[i].c = c;
	q[i].a = a;
	q[i].name = name;
}

static void
init()
{
	char *a, *o;
	int i, c, aga;

	if (inited)
		return;
	inited = 1;
	aga = (*VPOSR >> 8 & 0x0F) >= 2;
	for (i = 0; i < nel(cards); i++) {
		if (cards[i].pn == A3000SCSI && aga)
			continue;
		for (c = 0; autocon(cards[i].pn, c, &a, &o); c++)
			insert(cards[i].f, c, a, cards[i].name);
	}
	if (idepresent(&a)) {
		ide = 1;
		insert(ideq, 0, a, "A4000 IDE");
	} else if (aga)
		insert(noq, 0, (char *)0xDA0000, "A4000 IDE");
	if (csppcpresent()) {
		csppc = 1;
		insert(csppcqueue, 0, (char *)0xF40000, "CyberStorm SCSI");
	}
	if (ide || csppc)
		ide_hook();
}

int
sdopen(card)
uint card;
{
	init();
	if (card >= SDCARDS || !q[card].f)
		return ENXIO;
	return 0;
}

void
sdqueue(cp)
struct sdcom *cp;
{
	(*q[cp->card].f)(q[cp->card].c, cp);
}

char *
sdhardwarename(card)
uint card;
{
	return q[card].name;
}
