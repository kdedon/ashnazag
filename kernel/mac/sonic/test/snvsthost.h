/* snvsthost.h -- host build of snvst.c: the driver pieces it uses */
#include <stdio.h>
#include <string.h>
#include <errno.h>

struct sn_softc {
	unsigned char	cam[16][6];
};

extern unsigned char *sn_txbuf();
extern int sn_txroom();
extern void sn_txstart();
