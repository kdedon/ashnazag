#define GMEM_SEG_HOST
#define main allocator_main
#include "t_gmem.c"
#undef main
#include "../mod/guestcore/gmemseg.c"

static int loads, unlocks, unloads, fault_delegations, dup_error, unmap_error;
static int deny_page = -1, miss_page = -1;
static struct seg_ops native_ops;
static struct seg split;

page_t *page_numtookpp(pf) u_int pf;
{ return (int)(pf - pages_base) == miss_page ? 0 : pool + pf - pages_base; }
void hat_memload(s,a,p,prot,flags) struct seg*s; addr_t a; page_t*p; u_int prot; int flags; { loads++; }
void hat_unlock(s,a) struct seg*s; addr_t a; { unlocks++; }
void hat_unload(s,a,n,f) struct seg*s; addr_t a; u_int n; int f; { unloads++; }
void flushmmu(a,n) addr_t a; int n; { }
void map_addr(a,n,o,f) addr_t*a; u_int n; off_t o; int f; { *a=(addr_t)0x100000; }
struct seg *as_segat(as,a) struct as*as; addr_t a;
{ int i; for(i=0;i<as->n;i++) if (a>=as->seg[i]->s_base && a<as->seg[i]->s_base+as->seg[i]->s_size) return as->seg[i]; return 0; }
static int native_getprot(s,a,n,p) struct seg*s; addr_t a; u_int n,*p;
{ p[0] = ((a-s->s_base)>>shift)==deny_page ? PROT_READ : ((struct segdev_data*)s->s_data)->prot; return 0; }
static faultcode_t native_fault() { fault_delegations++; return 0; }
static void native_free(s) struct seg*s; { free(s->s_data); s->s_data=0; }
static int native_dup(s,n) struct seg*s,*n;
{ if(dup_error) return dup_error; n->s_data=malloc(sizeof(struct segdev_data)); memcpy(n->s_data,s->s_data,sizeof(struct segdev_data)); n->s_ops=s->s_ops; return 0; }
int segdev_create(s,a) struct seg*s; struct segdev_crargs*a;
{ struct segdev_data*d=malloc(sizeof*d); d->offset=a->offset; d->prot=a->prot; d->maxprot=a->maxprot; s->s_data=d; s->s_ops=&native_ops; return 0; }
static int native_unmap(s,a,n) struct seg*s; addr_t a; u_int n;
{ struct segdev_data*d=s->s_data;
 if(unmap_error) return unmap_error;
 if(a==s->s_base && n==s->s_size) { (*s->s_ops->free)(s); return 0; }
 if(a>s->s_base && a+n<s->s_base+s->s_size) {
  split=*s; split.s_base=a+n; split.s_size=s->s_base+s->s_size-split.s_base;
  split.s_data=malloc(sizeof*d); memcpy(split.s_data,d,sizeof*d);
  ((struct segdev_data*)split.s_data)->offset+=split.s_base-s->s_base;
  s->s_size=a-s->s_base; s->s_as->seg[s->s_as->n++]=&split;
 } else if(a==s->s_base) { s->s_base+=n; s->s_size-=n; d->offset+=n; }
 else s->s_size-=n;
 return 0;
}
int as_unmap(as,a,n) struct as*as; addr_t a; u_int n;
{ struct seg*s=as_segat(as,a); return s ? (*s->s_ops->unmap)(s,a,n) : 0; }
int as_map(as,a,n,create,args) struct as*as; addr_t a; u_int n; int(*create)(); caddr_t args;
{ static struct seg s; s.s_base=a; s.s_size=n; s.s_as=as; as->seg[as->n++]=&s; return (*create)(&s,args); }

static void mappings(sh)
unsigned int sh;
{
 struct guest_ram*r; struct as as={0}, child={0}; struct seg s={0}, c={0}; struct gm_args args;
 unsigned long ps=1UL<<sh, pa; int prior;
 shift=sh; gm_hat4k=sh==12?&hat_cm_ram:0;
 C(guest_ram_new(4*ps,0,0,&r)==0);
 s.s_base=(addr_t)0x100000; s.s_size=4*ps; s.s_as=&as; as.seg[as.n++]=&s;
 args.dev.offset=0; args.dev.prot=args.dev.maxprot=PROT_READ|PROT_WRITE|PROT_EXEC; args.ram=r;
 test_kmem_fail=1; C(gm_create(&s,&args)==ENOMEM && r->refs==1); test_kmem_fail=0;
 C(gm_create(&s,&args)==0 && r->refs==2);
 C(gm_fault(&s,s.s_base,ps,F_INVAL,S_READ)==0);
 ((struct segdev_data*)s.s_data)->prot=PROT_WRITE;
 C(gm_fault(&s,s.s_base,ps,F_INVAL,S_READ)==FC_PROT);
 C(gm_fault(&s,s.s_base,ps,F_INVAL,S_EXEC)==FC_PROT);
 ((struct segdev_data*)s.s_data)->prot=PROT_EXEC;
 C(gm_fault(&s,s.s_base,ps,F_INVAL,S_READ)==FC_PROT);
 C(gm_fault(&s,s.s_base,ps,F_INVAL,S_WRITE)==FC_PROT);
 ((struct segdev_data*)s.s_data)->prot=PROT_READ|PROT_WRITE;
 prior=unlocks; deny_page=1;
 C(gm_fault(&s,s.s_base,2*ps,F_SOFTLOCK,S_WRITE)==FC_PROT);
 C(!r->softlocks && unlocks==prior+1); deny_page=-1;
 miss_page=guest_ram_frame(r,ps)-pages_base; prior=unlocks;
 C(gm_fault(&s,s.s_base,2*ps,F_SOFTLOCK,S_READ)==FC_MAKE_ERR(ENOMEM));
 C(!r->softlocks && unlocks==prior+1); miss_page=-1;
 C(gm_fault(&s,s.s_base,2*ps,F_SOFTLOCK,S_READ)==0 && r->softlocks==2);
 C(gm_fault(&s,s.s_base,ps,F_SOFTLOCK,S_READ)==FC_MAKE_ERR(EBUSY) && r->softlocks==2);
 C(guest_ram_phys(r,0,2*ps,0,&pa)==EBUSY && !pa);
 C(gm_unmap(&s,s.s_base,ps)==EBUSY);
 prior=unlocks; C(gm_fault(&s,s.s_base,ps,F_SOFTUNLOCK,S_READ)==0 && r->softlocks==1);
 C(gm_fault(&s,s.s_base+ps,ps,F_SOFTUNLOCK,S_READ)==0 && !r->softlocks && unlocks==prior+2);
 C(guest_ram_phys(r,0,2*ps,0,&pa)==0);
 C(gm_fault(&s,s.s_base,ps,F_SOFTLOCK,S_READ)==0);
 C(guest_ram_phys(r,0,ps,0,&pa)==0 && pa);
 C(gm_fault(&s,s.s_base,ps,F_SOFTUNLOCK,S_READ)==0);
 c=s; c.s_as=&child; child.seg[child.n++]=&c;
 test_kmem_fail=1; C(gm_dup(&s,&c)==ENOMEM && r->refs==2); test_kmem_fail=0;
 dup_error=ENOMEM; C(gm_dup(&s,&c)==ENOMEM && r->refs==2); dup_error=0;
 C(gm_dup(&s,&c)==0 && r->refs==3);
 guest_ram_free(r); C(r->refs==2 && guestcore_wrapper.refs==1 && availrmem==64-r->reserved && guest_ram_reserved==r->reserved);
 test_kmem_fail=1; C(gm_unmap(&s,s.s_base+ps,ps)==ENOMEM && s.s_size==4*ps && r->refs==2); test_kmem_fail=0;
 C(gm_unmap(&s,s.s_base+ps,ps)==0 && gm_find(&split)!=0 && r->refs==3);
 C(split.s_ops==&gm_ops && gm_fault(&split,split.s_base,ps,F_INVAL,S_READ)==0);
 C(gm_unmap(&s,s.s_base,s.s_size)==0 && r->refs==2);
 C(gm_fault(&split,split.s_base,ps,F_SOFTLOCK,S_READ)==0 && r->softlocks==1);
 prior=unlocks; gm_free(&split); C(r->refs==1 && !r->softlocks && unlocks==prior+1);
 C(gm_fault(&c,c.s_base+2*ps,ps,F_INVAL,S_READ)==0);
 C(gm_unmap(&c,c.s_base,c.s_size)==0 && !guestcore_wrapper.refs);
 C(availrmem==64 && !pages_pp_kernel && !guest_ram_objects && !gm_maps);
}
int main() {
 native_ops.dup=native_dup; native_ops.unmap=native_unmap; native_ops.free=native_free;
 native_ops.getprot=native_getprot; native_ops.fault=native_fault;
 mappings(11); mappings(12);
 printf("guest RAM mappings: %d production checks passed\n",checks); return 0;
}
