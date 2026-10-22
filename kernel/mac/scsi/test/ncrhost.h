/*
 * Host build of ncr96.c for the state-machine test: kernel types and
 * services come from the simulator (ncrsim.c).
 */
#ifndef NCRHOST_H
#define NCRHOST_H
#include <stdio.h>
#include <string.h>

typedef char *caddr_t;
#include "rico.h"
#include "sd.h"

#define printf	sim_printf
extern int	sim_printf(const char *, ...);
extern int	splscsi();
extern void	splrestore();
extern int	sim_sr, sim_t1hi();
#define SR_GET(s)	((s) = sim_sr)
#define SR_SET(s)	(sim_sr = (s))
#endif
