/* sim.h -- simulated VIA1 + ADB transceiver + devices */
#define SIM_NDEV	8
#define SIM_KBD		1
#define SIM_MOUSE	2

struct sdev {
	int	kind, orig, addr, handler, handler0, handler2;
	int	srqen, collided;
	unsigned char q[64];
	int	nq;
	int	leds;
	int	mdx, mdy, mbtn, mlast;
};

extern struct sdev sim_dev[];
extern int sim_ndev, sim_dead, sim_stall, sim_end2, sim_nbytes;
extern unsigned long sim_ncmd;
void sim_reset(), sim_idle(), sim_key();
int sim_add(), sim_pending(), sim_state();
