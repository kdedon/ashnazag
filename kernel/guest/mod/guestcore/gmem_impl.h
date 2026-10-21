#ifndef GMEM_IMPL_H
#define GMEM_IMPL_H
struct guest_ram {
	unsigned long size, n;
	unsigned int shift;
	int held, refs, wt;
	unsigned long softlocks, reserved;
	page_t **page;
	unsigned char *dma;
	void (*invalidate)();
	char *arg;
};
extern void gm_unload();
#endif
