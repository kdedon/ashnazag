/*
 * Host build of ncr96.c for the state-machine test: kernel types and
 * services come from the simulator (ncrsim.c).
 */
#include <stdio.h>
#include <string.h>

typedef char *caddr_t;
#include "rico.h"
#include "sd.h"

#define printf	sim_printf
extern int	sim_printf(const char *, ...);
extern int	splscsi();
extern void	splrestore();
