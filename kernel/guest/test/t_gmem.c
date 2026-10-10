#include "gmem-host/kinc.h"
#include "../mod/guestcore/gmem.c"

static page_t pool[64];
page_t *pages = pool;
u_int pages_base = 10, pages_pp_kernel;
int availrmem = 64, page_fail, test_kmem_fail;
struct tune tune = { 2 };
struct modwrapper guestcore_wrapper;
long hat_cm_ram;
static int invalidations, flushes, checks;
static unsigned int shift;
static int page_pressure, pressure_waits;
static struct guest_ram *race_ram;
static unsigned long race_off;

#define C(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d line %d\n", checks, __LINE__); exit(1); } } while (0)

page_t *page_get(bytes, flags)
unsigned long bytes;
int flags;
{
	unsigned long n = bytes >> shift, i, j, first;
	page_t *head = 0, *prev = 0;
	if (page_pressure) {
		if (!(flags & P_CANWAIT))
			return 0;
		pressure_waits++;
		page_pressure = 0;
	}
	if (race_ram) {
		struct guest_ram *r = race_ram;
		race_ram = 0;
		C(guest_ram_frame(r, race_off) >= 0);
	}
	if (page_fail)
		return 0;
	for (first = 0; first + n <= 64; first++) {
		for (j = 0; j < n && !pool[first + j].used; j++) ;
		if (j == n) break;
	}
	if (first + n > 64)
		return 0;
	for (i = 0; i < n; i++) {
		page_t *p = pool + first + i;
		p->used = p->p_keepcnt = 1;
		if (!head) head = p;
		if (prev) prev->p_next = p;
		prev = p;
	}
	prev->p_next = head;
	return head;
}
void page_abort(p) page_t *p; { C(p->p_keepcnt == 0); p->used = 0; }
void pagezero(p, off, len) page_t *p; unsigned long off, len; { memset(p->data + off, 0, len); }
void ppcopy(a, b) page_t *a, *b; { memcpy(b->data, a->data, 1UL << shift); }
void dlm_cacheflush() { flushes++; }
#ifndef GMEM_SEG_HOST
void gm_unload(m) struct guest_ram *m; { invalidations++; }
#endif

static void run(sh)
unsigned int sh;
{
	struct guest_ram *m, *other;
	unsigned long pa = 9, ps = 1UL << sh;
	int a, b;
	shift = sh;
	gm_hat4k = sh == 12 ? &hat_cm_ram : 0;
	C(guest_ram_new(0, 0, 0, &m) == EINVAL && !m);
	C(guest_ram_new(ps + 1, 0, 0, &m) == EINVAL);
	C(guest_ram_new(ps, 8, 0, &m) == EINVAL);
	C(guest_ram_new(4 * ps, 0, 0, &m) == 0);
	C(availrmem == 60 && pages_pp_kernel == 0 && guestcore_wrapper.refs == 1 && guest_ram_reserved == 4);
	C(guest_ram_size(m) == 4 * ps && guest_ram_frame(m, 4 * ps) == -1);
	a = guest_ram_frame(m, 0);
	C(a >= 0 && availrmem == 60 && pages_pp_kernel == 1);
	C(guest_ram_frame(m, 0) == a && availrmem == 60);
	C(m->page[0]->data[0] == 0);
	m->page[0]->data[0] = 0x73;
	C(guest_ram_new(ps, GMR_CONTIG, 0, &other) == 0);
	b = guest_ram_frame(m, ps);
	C(b != a + 1);
	m->page[1]->data[0] = 0x31;
	page_fail = 1;
	C(guest_ram_phys(m, 0, 2 * ps, 0, &pa) == ENOMEM && !pa);
	C(m->page[0]->data[0] == 0x73 && availrmem == 59);
	page_fail = 0;
	m->softlocks = 1;
	C(guest_ram_phys(m, 0, 2 * ps, 0, &pa) == EBUSY && !pa && availrmem == 59);
	m->softlocks = 0;
	C(guest_ram_phys(m, 0, 2 * ps, 0, &pa) == 0);
	C(pa == ((pages_base + (m->page[0] - pages)) << shift));
	C(m->page[1] == m->page[0] + 1 && m->page[0]->data[0] == 0x73 && m->page[1]->data[0] == 0x31);
	C(availrmem == 59 && pages_pp_kernel == 3 && invalidations && flushes == 2);
	C(guest_ram_phys(m, 0, ps, 0, &pa) == 0);
	C(guest_ram_phys(m, 0, 3 * ps, 0, &pa) == EBUSY && !pa);
	C(guest_ram_phys(m, 4 * ps - 1, 2, 0, &pa) == EINVAL && !pa);
	C(guest_ram_phys(m, 0, 0, 0, &pa) == EINVAL);
	C(guest_ram_phys(m, 3 * ps, ps, ps, &pa) == ENOMEM && availrmem == 59);
	guest_ram_free(other);
	guest_ram_free(m);
	C(availrmem == 64 && pages_pp_kernel == 0 && !guestcore_wrapper.refs);
	page_fail = 1;
	C(guest_ram_new(4 * ps, GMR_CONTIG, 0, &m) == ENOMEM && !m && availrmem == 64);
	page_fail = 0;
	C(guest_ram_new(ps, GMR_CONTIG, ps, &m) == ENOMEM && !m && availrmem == 64);
	C(guest_ram_new(ps, 0, 0, &m) == 0);
	availrmem = 2;
	page_fail = 1;
	C(guest_ram_frame(m, 0) == -1 && availrmem == 2);
	page_fail = 0;
	guest_ram_free(m);
	C(availrmem == 3 && !guest_ram_reserved && !guest_ram_pages);
	availrmem = 64;
	C(guest_ram_reserved == 0 && guest_ram_pages == 0);
	availrmem = 5;
	C(guest_ram_new(4 * ps, 0, 0, &m) == ENOMEM && !m && availrmem == 5);
	C(!guest_ram_reserved && !guest_ram_objects && !pages_pp_kernel);
	availrmem = 64;
	C(guest_ram_new(4 * ps, 0, 0, &m) == 0 && availrmem == 60);
	page_pressure = 1;
	C(guest_ram_frame(m, 0) >= 0 && pressure_waits && availrmem == 60);
	race_ram = m; race_off = ps;
	C(guest_ram_frame(m, ps) >= 0 && guest_ram_pages == 2 && pages_pp_kernel == 2 && availrmem == 60);
	availrmem = 1;
	C(guest_ram_phys(m, 2 * ps, 2 * ps, 0, &pa) == 0 && availrmem == 1 && guest_ram_pages == 4);
	availrmem = 60;
	guest_ram_free(m);
	C(availrmem == 64 && !guest_ram_reserved && !guest_ram_pages && !guestcore_wrapper.refs);
	C(guest_ram_new(4 * ps, 0, 0, &m) == 0);
	C(guest_ram_frame(m, 0) >= 0);
	availrmem = 2;
	C(guest_ram_phys(m, 0, 3 * ps, 0, &pa) == ENOMEM && !pa && availrmem == 2);
	availrmem = 3;
	C(guest_ram_phys(m, 0, 3 * ps, 0, &pa) == 0 && availrmem == 3 && guest_ram_pages == 3);
	guest_ram_free(m);
	C(availrmem == 7 && !guest_ram_reserved && !guest_ram_pages);
	availrmem = 64;
	flushes = invalidations = 0;
}
int main() { run(11); run(12); printf("guest RAM: %d production checks passed\n", checks); return 0; }
