#ifndef SIMSONIC_H
#define SIMSONIC_H

#include "../sonic.h"

#define SIMMEMSZ	SN_POOLALLOC
#define SIMWIRE		512

enum { SIM_OK, SIM_OFF, SIM_FILTERED, SIM_NORBA, SIM_NORDA, SIM_TOOBIG };

struct sim {
	unsigned short	reg[SN_NREGS];
	int		rst, rxen, txp, at_eol;
	int		rbe_stall, rde_stall, have_rba;
	sn_u32		crba, rbwc, llfa;
	int		rbaseq, pktseq;
	unsigned char	cam[16][6];
	int		camloads;
	unsigned char	wire[SIMWIRE][1536];
	int		wirelen[SIMWIRE], nwire;
	int		received, missed, errors;
	int		lose_txp, tx_fail;	/* fault injection */
};

extern unsigned char simmem[];
extern struct sim sim;

unsigned short	sim_rd(int);
void		sim_wr(int, int);
sn_u32		sim_pa(unsigned char *);
unsigned char	*sim_va(sn_u32);
int		sim_txstep(void);
int		sim_rx(const unsigned char *, int);

#endif
