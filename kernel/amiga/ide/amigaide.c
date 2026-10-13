/*
 * SCSI host adapter selection with the A4000's IDE port as one more
 * adapter.  Replaces the kernel's own table walk, which knows no IDE and
 * on an A4000 registers an A3000 SCSI controller that is not there.
 * Adapters are numbered by address, so the IDE port is card 0 on an
 * A4000 and its master, answering as unit 6, is c6d0.
 */
#include "sys/types.h"
#include "sys/errno.h"
#include "rico.h"
#include "sd.h"

#define A3000SCSI	0x0202F003
#define VPOSR		((volatile ushort *)0xDFF004)

extern bool a3091queue(), a2090queue(), a2091queue();
extern int idepresent(), idequeue(), autocon();
extern void ideintr(), ide_cpush();
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

static int inited;
static void (*int2_prev)();

/* PIO goes through the uncached identity window; keep the cache out of it */
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
	ideintr();
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
		ide_hook();
		insert(ideq, 0, a, "A4000 IDE");
	}
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
