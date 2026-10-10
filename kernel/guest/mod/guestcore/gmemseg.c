#include "kinc.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "vm/hat.h"
#include "vm/seg_dev.h"
#include "vm/page.h"
#include "guest_mem.h"
#include "gmem_impl.h"

extern int segdev_create(), as_map(), as_unmap();
extern void map_addr(), flushmmu(), hat_unlock();
extern struct seg *as_segat();
extern page_t *page_numtookpp();

struct gm_map {
	struct seg *seg;
	struct guest_ram *ram;
	struct gm_map *next;
	unsigned char *locks;
	unsigned long n;
};
static struct gm_map *gm_maps;
static struct seg_ops gm_ops, *gm_devops;

struct gm_args {
	struct segdev_crargs dev;
	struct guest_ram *ram;
};

static struct gm_map *
gm_find(seg)
struct seg *seg;
{
	struct gm_map *m;
	for (m = gm_maps; m; m = m->next)
		if (m->seg == seg)
			return m;
	return 0;
}

void
gm_unload(ram)
struct guest_ram *ram;
{
	struct gm_map *m;
	for (m = gm_maps; m; m = m->next)
		if (m->ram == ram && (ram->shift != 12 ||
		    *(unsigned long **)((char *)m->seg->s_as + 20)))
			hat_unload(m->seg, m->seg->s_base, m->seg->s_size, HAT_NOFLAGS);
}

static struct gm_map *
gm_alloc(seg, ram)
struct seg *seg;
struct guest_ram *ram;
{
	struct gm_map *m = (struct gm_map *)kmem_zalloc(sizeof *m, KM_NOSLEEP);
	if (!m)
		return 0;
	m->n = seg->s_size >> ram->shift;
	m->locks = (unsigned char *)kmem_zalloc(m->n, KM_NOSLEEP);
	if (!m->locks) {
		kmem_free(m, sizeof *m);
		return 0;
	}
	return m;
}

static void
gm_drop(m)
struct gm_map *m;
{
	kmem_free(m->locks, m->n);
	kmem_free(m, sizeof *m);
}

static void
gm_unlock(m, addr, len)
struct gm_map *m;
addr_t addr;
u_int len;
{
	unsigned long va, i, ps = 1UL << m->ram->shift;
	for (va = (unsigned long)addr & ~(ps - 1); va < (unsigned long)addr + len; va += ps) {
		i = (va - (unsigned long)m->seg->s_base) >> m->ram->shift;
		if (i < m->n && m->locks[i]) {
			hat_unlock(m->seg, (addr_t)va);
			m->locks[i] = 0;
			m->ram->softlocks--;
		}
	}
}

static void
gm_add(m, seg, ram)
struct gm_map *m;
struct seg *seg;
struct guest_ram *ram;
{
	m->seg = seg;
	m->ram = ram;
	m->next = gm_maps;
	gm_maps = m;
	ram->refs++;
	seg->s_ops = &gm_ops;
}

static void
gm_free(seg)
struct seg *seg;
{
	struct gm_map **p, *m = 0;

	for (p = &gm_maps; *p; p = &(*p)->next)
		if ((*p)->seg == seg) {
			m = *p;
			*p = m->next;
			break;
		}
	if (m)
		gm_unlock(m, seg->s_base, seg->s_size);
	(*gm_devops->free)(seg);
	if (m) {
		guest_ram_free(m->ram);
		gm_drop(m);
	}
}

static int
gm_dup(seg, nseg)
struct seg *seg, *nseg;
{
	struct gm_map *old = gm_find(seg);
	struct gm_map *m;
	int e;

	if (!old)
		return EFAULT;
	m = gm_alloc(nseg, old->ram);
	if (!m)
		return ENOMEM;
	if ((e = (*gm_devops->dup)(seg, nseg)) == 0)
		gm_add(m, nseg, old->ram);
	else
		gm_drop(m);
	return e;
}

static int
gm_unmap(seg, addr, len)
struct seg *seg;
addr_t addr;
u_int len;
{
	struct as *as = seg->s_as;
	struct seg *n;
	struct gm_map *old = gm_find(seg), *m = 0;
	struct guest_ram *ram;
	int mid = addr > seg->s_base && addr + len < seg->s_base + seg->s_size;
	int e;

	if (!old)
		return EFAULT;
	ram = old->ram;
	if (ram->softlocks)
		return EBUSY;
	if (mid && !(m = gm_alloc(seg, ram)))
		return ENOMEM;
	e = (*gm_devops->unmap)(seg, addr, len);
	if (!e && mid && (n = as_segat(as, addr + len)) != 0 && (n->s_ops == gm_devops || n->s_ops == &gm_ops))
		gm_add(m, n, ram);
	else if (m)
		gm_drop(m);
	return e;
}

/* DMA writes must reach RAM without a cache push from the guest. */
static void
gm_writethrough(seg, va)
struct seg *seg;
unsigned long va;
{
	unsigned long *root = *(unsigned long **)((char *)seg->s_as + 20);
	unsigned long a, b, *leaf;
	if (!root || !((a = root[va >> 25]) & 2))
		return;
	b = ((unsigned long *)(a & 0xFFFFFE00UL))[(va >> 18) & 0x7F];
	if (!(b & 2))
		return;
	leaf = (unsigned long *)(b & 0xFFFFFF00UL) + ((va >> 12) & 0x3F);
	if (*leaf & 1) {
		*leaf &= ~0x60UL;
		flushmmu((addr_t)va, 1);
	}
}

static faultcode_t
gm_fault(seg, addr, len, type, rw)
struct seg *seg;
addr_t addr;
u_int len;
enum fault_type type;
enum seg_rw rw;
{
	struct gm_map *m = gm_find(seg);
	struct segdev_data *sd = (struct segdev_data *)seg->s_data;
	unsigned long va, mask, start, index;
	faultcode_t error = 0;
	u_int prot[1];
	page_t *pp;
	int pf;

	if (!m)
		return FC_MAKE_ERR(EFAULT);
	if (type == F_SOFTUNLOCK) {
		gm_unlock(m, addr, len);
		return 0;
	}
	if (type != F_INVAL && type != F_SOFTLOCK)
		return (*gm_devops->fault)(seg, addr, len, type, rw);
	mask = (1UL << m->ram->shift) - 1;
	start = (unsigned long)addr & ~mask;
	if (type == F_SOFTLOCK)
		for (va = start; va < (unsigned long)addr + len; va += mask + 1) {
			index = (va - (unsigned long)seg->s_base) >> m->ram->shift;
			if (index >= m->n || m->locks[index])
				return FC_MAKE_ERR(EBUSY);
		}
	for (va = start; va < (unsigned long)addr + len; va += mask + 1) {
		prot[0] = 0;
		(void)(*gm_devops->getprot)(seg, (addr_t)va, 0, prot);
		if (!(prot[0] & (PROT_READ | PROT_WRITE | PROT_EXEC)) ||
		    (rw == S_READ && !(prot[0] & PROT_READ)) ||
		    (rw == S_WRITE && !(prot[0] & PROT_WRITE)) ||
		    (rw == S_EXEC && !(prot[0] & PROT_EXEC))) {
			error = FC_PROT;
			break;
		}
		pf = guest_ram_frame(m->ram, sd->offset + va - (unsigned long)seg->s_base);
		if (pf < 0 || !(pp = page_numtookpp((u_int)pf))) {
			error = FC_MAKE_ERR(ENOMEM);
			break;
		}
		hat_memload(seg, (addr_t)va, pp, prot[0], type == F_SOFTLOCK ? HAT_LOCK : HAT_NOFLAGS);
		if (type == F_SOFTLOCK) {
			m->locks[(va - (unsigned long)seg->s_base) >> m->ram->shift] = 1;
			m->ram->softlocks++;
		}
		if (m->ram->shift == 12 &&
		    m->ram->dma[(sd->offset + va - (unsigned long)seg->s_base) >> 12])
			gm_writethrough(seg, va);
	}
	if (error && type == F_SOFTLOCK)
		gm_unlock(m, (addr_t)start, va - start);
	return error;
}

static int
gm_nomap(dev, off, prot)
dev_t dev;
off_t off;
int prot;
{
	return -1;
}

static int
gm_create(seg, args)
struct seg *seg;
struct gm_args *args;
{
	struct gm_map *m = gm_alloc(seg, args->ram);
	int e;

	if (!m)
		return ENOMEM;
	if ((e = segdev_create(seg, (caddr_t)&args->dev)) != 0) {
		gm_drop(m);
		return e;
	}
	if (!gm_devops) {
		gm_devops = seg->s_ops;
		gm_ops = *gm_devops;
		gm_ops.free = gm_free;
		gm_ops.dup = gm_dup;
		gm_ops.unmap = gm_unmap;
		gm_ops.fault = gm_fault;
	}
	gm_add(m, seg, args->ram);
	return 0;
}

int
guest_ram_map(ram, dev, off, as, addrp, len, prot, maxprot, flags)
struct guest_ram *ram;
dev_t dev;
off_t off;
struct as *as;
addr_t *addrp;
u_int len, prot, maxprot, flags;
{
	struct gm_args args;
	unsigned long mask, rlen;
	int e;

	if (!ram)
		return ENXIO;
	mask = (1UL << ram->shift) - 1;
	rlen = (len + mask) & ~mask;
	if ((flags & MAP_TYPE) != MAP_SHARED || off < 0 || (off & mask) ||
	    !rlen || rlen < len || (unsigned long)off >= ram->size || rlen > ram->size - off)
		return EINVAL;
	if (flags & MAP_FIXED) {
		if ((e = as_unmap(as, *addrp, len)) != 0)
			return e;
	}
	else {
		map_addr(addrp, len, off, 0);
		if (!*addrp)
			return ENOMEM;
	}
	args.dev.mapfunc = gm_nomap;
	args.dev.offset = off;
	args.dev.dev = dev;
	args.dev.prot = prot;
	args.dev.maxprot = maxprot;
	args.ram = ram;
	return as_map(as, *addrp, len, gm_create, (caddr_t)&args);
}
