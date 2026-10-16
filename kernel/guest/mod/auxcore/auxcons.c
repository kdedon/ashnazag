#include "auxcore.h"
#include "sys/conf.h"
#include "sys/pathname.h"
#include "sys/stream.h"
#include "sys/stropts.h"
#include "sys/strsubr.h"
#include "../../../mac/scc/consroute.h"

long aux_conopen, aux_conopenerr, aux_conioc, aux_concmd, aux_conlen, aux_conreply;
long aux_conbind, aux_conout, aux_conin, aux_conread, aux_connotify, aux_condetach;

__asm__(".weak scc_conroute\n.weak scc_conunroute\n.weak scc_confeed\n.weak scc_conresume");
static int (*__volatile__ routefn)() = scc_conroute;
static int (*__volatile__ feedfn)() = scc_confeed;
static void (*__volatile__ unroutefn)() = scc_conunroute;
static void (*__volatile__ resumefn)() = scc_conresume;

struct acstate {
    queue_t *rq;
    struct acstate *next;
    struct vnode *console;
    cred_t *cred;
    struct proc *owner;
    pid_t pid;
    struct pid *pidref;
    int active, notify, console_error;
};

static struct acstate *acstates;

static void acdetach();
static int acopen(), acclose(), acrput(), acrsrv(), acwput(), acwsrv();
static struct module_info acinfo = { 6101, "auxcons", 0, INFPSZ, 512, 256 };
static struct qinit acrinit = { acrput, acrsrv, acopen, acclose, 0, &acinfo, 0 };
static struct qinit acwinit = { acwput, acwsrv, 0, 0, 0, &acinfo, 0 };
static struct streamtab actab = { &acrinit, &acwinit, 0, 0 };
static int acflag = D_NEW;
struct mod_str_data auxcore_strdata[] = { { "auxcons", &actab, &acflag }, { "", 0, 0 } };

static int
acowner(st)
    struct acstate *st;
{
    struct proc *p = prfind(st->pid);
    return p == st->owner && p->p_pidp == st->pidref && p->p_stat != SZOMB;
}

static int
acoutput(key, mp)
    caddr_t key;
    mblk_t *mp;
{
    struct acstate *st = (struct acstate *)key;
    queue_t *q = WR(st->rq);
    if (!st->active) return 0;
    if (!acowner(st)) {acdetach(st);return 0;}
    if (!canput(q)) return -1;
    putq(q, mp);qenable(q);aux_conout++;
    return 1;
}

static void
acreading(key, event)
    caddr_t key;
    int event;
{
    struct acstate *st = (struct acstate *)key;
    if (!st->active) return;
    if (event == 2) qenable(st->rq);
    else {
        aux_conread++;
        if (st->notify && acowner(st)) {
            psignal(st->owner, aux_sig_in(16));aux_connotify++;
        }
    }
}

static void
acdetach(st)
    struct acstate *st;
{
    void (*fn)() = unroutefn;
    if (st->active) {
        if (fn) (*fn)((caddr_t)st);
        st->active = st->notify = 0;aux_condetach++;
    }
}

static int
acopen(q, dev, flag, how, cr)
    queue_t *q;
    dev_t *dev;
    int flag, how;
    cred_t *cr;
{
    struct guest_proc *gp = GUESTP(u.u_procp);
    struct acstate *st;
    int e;
    if (q->q_ptr) return 0;
    aux_conopen++;
    if (how != MODOPEN) return EINVAL;
    if (u.u_procp->p_pidp->pid_ref == 63) return EAGAIN;
    st = (struct acstate *)kmem_zalloc(sizeof *st, KM_SLEEP);
    st->rq = q;st->owner = u.u_procp;st->pid = u.u_procp->p_pid;
    st->pidref = u.u_procp->p_pidp;PID_HOLD(st->pidref);
    st->cred = cr;crhold(cr);
    st->console_error = routefn && feedfn && unroutefn && resumefn ? EPERM : ENODEV;
    if (st->console_error != ENODEV && gp && gp->gp_prof == &aux_profile && (AUXP(gp)->ap_mac & APM_TASK)) {
        e = vn_open("/dev/console", UIO_SYSSPACE, FREAD|FWRITE|FNOCTTY,
            0, &st->console, 0);
        st->console_error = e;
        if (e) st->console = 0;
        if (!e && (st->console->v_type != VCHR ||
            getmajor(st->console->v_rdev) != 0 || getminor(st->console->v_rdev) != 0))
            st->console_error = ENXIO;
    }
    aux_conopenerr=st->console_error;
    st->next=acstates;acstates=st;
    q->q_ptr = WR(q)->q_ptr = (caddr_t)st;
    return 0;
}

static int
acclose(q, flag, cr)
    queue_t *q;
    int flag;
    cred_t *cr;
{
    struct acstate *st = (struct acstate *)q->q_ptr;
    struct acstate **link;
    if (!st) return 0;
    for (link=&acstates; *link && *link != st; link=&(*link)->next) ;
    if (*link) *link=st->next;
    acdetach(st);flushq(q,FLUSHALL);flushq(WR(q),FLUSHALL);
    q->q_ptr = WR(q)->q_ptr = 0;
    if (st->console) {
        VOP_CLOSE(st->console,FREAD|FWRITE|FNOCTTY,1,(off_t)0,st->cred);
        VN_RELE(st->console);
    }
    crfree(st->cred);PID_RELE(st->pidref);kmem_free((caddr_t)st,sizeof *st);
    return 0;
}

static int
acrput(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    struct acstate *st = (struct acstate *)q->q_ptr;
    if (mp->b_datap->db_type == M_DATA && st && st->active) {
        putq(q,mp);qenable(q);return 0;
    }
    if (mp->b_datap->db_type == M_HANGUP && st) acdetach(st);
    putnext(q,mp);return 0;
}

static int
acrsrv(q)
    queue_t *q;
{
    struct acstate *st = (struct acstate *)q->q_ptr;
    mblk_t *mp;
    int e;
    int (*fn)() = feedfn;
    while ((mp=getq(q))) {
        e = st && st->active && fn ? (*fn)((caddr_t)st,mp) : ENXIO;
        if (e == EAGAIN) {putbq(q,mp);return 0;}
        if (e) freemsg(mp);else aux_conin++;
    }
    return 0;
}

/* Control reaches the owned module directly because tty disciplines reject it. */
int
aux_conscontrol(vp, cmd, pid, cr)
    struct vnode *vp;
    int cmd;
    pid_t pid;
    cred_t *cr;
{
    queue_t *q;
    struct acstate *st;
    int e = ENXIO;
    int (*fn)() = routefn;
    aux_conioc++;aux_concmd=cmd;aux_conlen=sizeof pid;
    if (cmd != AUXCROUTE && cmd != AUXCRDSIG) return aux_conreply=EINVAL;
    if (!vp || !vp->v_stream) return aux_conreply=ENXIO;
    for (q=vp->v_stream->sd_wrq; q; q=q->q_next) {
        if (!q->q_qinfo || q->q_qinfo->qi_minfo != &acinfo) continue;
        for (st=acstates; st && (caddr_t)st != q->q_ptr; st=st->next) ;
        if (!st || WR(st->rq) != q || st->rq->q_ptr != (caddr_t)st)
            return aux_conreply=ENXIO;
        if (!cr || cr->cr_uid != st->cred->cr_uid || pid != st->pid ||
            u.u_procp != st->owner || !acowner(st)) return aux_conreply=EPERM;
        if (cmd == AUXCROUTE) {
            e=st->console_error;
            if (!e && !st->active) {
                e=fn ? (*fn)((caddr_t)st,acoutput,acreading) : ENODEV;
                if (!e) {st->active=1;aux_conbind++;}
            }
        } else if (st->active) {st->notify=1;e=0;}
        else e=ENXIO;
        return aux_conreply=e;
    }
    return aux_conreply=e;
}

static int
acwput(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    if (mp->b_datap->db_type == M_DATA) {putq(q,mp);qenable(q);}
    else putnext(q,mp);
    return 0;
}

static int
acwsrv(q)
    queue_t *q;
{
    struct acstate *st = (struct acstate *)q->q_ptr;
    mblk_t *mp;
    void (*fn)() = resumefn;
    while ((mp=getq(q))) {
        if (!canput(q->q_next)) {putbq(q,mp);return 0;}
        putnext(q,mp);
    }
    if (st && st->active && fn) (*fn)((caddr_t)st);
    return 0;
}
