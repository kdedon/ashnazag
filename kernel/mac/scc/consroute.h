#ifndef _SCC_CONSROUTE_H
#define _SCC_CONSROUTE_H
extern int scc_conroute();
extern void scc_conunroute(), scc_conresume();
extern int scc_confeed();
#define AUXCROUTE (('c' << 8) | 2)
#define AUXCRDSIG (('c' << 8) | 1)
#endif
