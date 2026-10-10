#include "../mps.h"
#include "../configurator.h"
void *pointer_alloc(unsigned long n) { return OTAllocMem(n); }
void *pointer_block(unsigned long n) { return allocb(n,2); }
void *pointer_get(void *q) { return getq(q); }
void *pointer_read(void *q) { return RD(q); }
void *pointer_write(void *q) { return WR(q); }
void *pointer_timer(void *q) { return mi_timer_alloc(q,0); }
void *pointer_name(void *q) { return (void *)OTCfigGetProviderNameD0(q); }
void *pointer_configurator(void) { return (void *)OTNewConfiguratorD0(0,0,0,0); }
void *pointer_state(void) { return (void *)OTCreateStateMachineD0(0,0,0,0,0); }
