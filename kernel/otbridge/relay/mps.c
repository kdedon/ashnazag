#include "mps.h"
#include "host.h"

#define M_IOCTL 0x0e
#define M_IOCNAK 0x82
#define M_FLUSH 0x86
#define M_HANGUP 0x89
#define M_ERROR 0x8a
#define ENOMEM_OT 12
#define EINVAL_OT 22
#define EIO_OT 5
#define TIMER_MS 8
#define OTR_MODOPEN 1
#define OTR_CLONEOPEN 2

struct endpoint {
	struct endpoint *next, *owned_next;
	unsigned long device;
	int cloned;
	struct otr_provider relay;
	struct otr_boot boot;
	void *read, *write;
	struct otr_msg *timer;
	int failed, notified, eof, closing, info_done, protocol;
	unsigned long frames, interval, identity;
#ifdef OTR_TRACE_TPI
	unsigned char traced[3];
#endif
};
struct ioctl_prefix {
	long command;
	void *credentials;
	unsigned long id;
	long count, error, result;
};
#if defined(__m68k__)
typedef char otr_ioctl_size[(sizeof(struct ioctl_prefix) == 24) ? 1 : -1];
#endif
static struct otr_boot session;
static unsigned long cookie;
static int opening;
static struct endpoint *clones, *owned;
#ifdef OTR_DIAGNOSTICS
static void capture_accept(struct endpoint *, unsigned long);
static void capture_ioctl(struct endpoint *, long, long);
#ifdef OTR_TRACE_TPI
static void capture_packet(struct endpoint *, int, int, const unsigned char *, unsigned long);
static void capture_pump(struct endpoint *, int);
static void capture_handoff(struct endpoint *, int);
#endif
#endif

static struct endpoint *
enter(q)
	void *q;
{
	struct endpoint *e = ((struct otr_queue_prefix *)q)->private;
	if (!e || e->closing) return 0;
	e->frames++;
	return e;
}
static void
leave(e)
	struct endpoint *e;
{
	if (!--e->frames && e->closing) OTFreeMem(e);
}
#ifdef OTR_TRACE_LOAD
extern long otr_sys_write(int, const unsigned char *, unsigned long);
#define trace_load(text) ((void)otr_sys_write(1, (const unsigned char *)(text), sizeof(text) - 1))
#else
#define trace_load(text) ((void)0)
#endif
#ifdef OTR_TRACE_ACCEPT
extern long otr_sys_write(int, const unsigned char *, unsigned long);
static void
trace_accept(e, token)
	struct endpoint *e;
	unsigned long token;
{
	unsigned char line[80];
	const char *prefix = "otrelay I accept token/rq/wq/cookie ";
	unsigned long values[4];
	int n = 0, i, shift;
	values[0] = token; values[1] = (unsigned long)e->read;
	values[2] = (unsigned long)e->write; values[3] = e->identity;
	while (*prefix) line[n++] = *prefix++;
	for (i = 0; i < 4; i++) {
		for (shift = 28; shift >= 0; shift -= 4)
			line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
		line[n++] = i == 3 ? '\n' : ' ';
	}
	(void)otr_sys_write(1, line, n);
}
#endif
#ifdef OTR_TRACE_TPI
extern long otr_sys_write(int, const unsigned char *, unsigned long);
static void
trace_packet(e, phase, type, bytes, length)
    struct endpoint *e;
    int phase, type;
    const unsigned char *bytes;
    unsigned long length;
{
    unsigned char line[80];
    const char *prefix = "otrelay I tpi cookie/phase/type/primitive ";
    unsigned long values[4];
    int n = 0, i, shift;
    if (phase < 0 || phase > 2) return;
#ifdef OTR_DIAGNOSTICS
    capture_packet(e, phase, type, bytes, length);
#endif
    if (e->traced[phase] >= 8) return;
    ++e->traced[phase];
    values[0] = e->identity; values[1] = phase; values[2] = type;
    values[3] = 0xffffffffUL;
    if (bytes && length >= 4)
        values[3] = ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
            ((unsigned long)bytes[2] << 8) | bytes[3];
    while (*prefix) line[n++] = *prefix++;
    for (i = 0; i < 4; i++) {
        for (shift = 28; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
        line[n++] = i == 3 ? '\n' : ' ';
    }
    (void)otr_sys_write(1, line, n);
}
#else
#define trace_packet(e,phase,type,bytes,length) ((void)0)
#endif
static void
release(arg, mp)
	void *arg;
	struct otr_msg *mp;
{
	(void)arg; freemsg(mp);
}
static void
close_host(arg, fd)
	void *arg;
	int fd;
{
	struct endpoint *e = arg;
	(void)fd;
	otr_boot_close(&e->boot);
	if (!session.children) otr_boot_close(&session);
}
static int
deliver(arg, q, type, ctl, clen, data, dlen)
	void *arg, *q;
	int type;
	const unsigned char *ctl, *data;
	unsigned long clen, dlen;
{
	struct otr_msg *head, *tail;
	unsigned long i;
	int handed;
	(void)arg;
	if (type != OTB_R_PCPROTO && !canputnext(q)) return OTR_WAIT;
	head = allocb(type == OTB_R_DATA ? dlen : clen, 2L);
	if (!head) return OTR_WAIT;
	head->block->type = type;
	if (type == OTB_R_DATA) {
		for (i = 0; i < dlen; i++) *head->write++ = data[i];
	} else {
		for (i = 0; i < clen; i++) *head->write++ = ctl[i];
		if (dlen) {
			tail = allocb(dlen, 2L);
			if (!tail) { freemsg(head); return OTR_WAIT; }
			tail->block->type = OTR_M_DATA;
			for (i = 0; i < dlen; i++) *tail->write++ = data[i];
			head->cont = tail;
		}
	}
	trace_packet((struct endpoint *)arg, 2, type, ctl, clen);
	handed = putnext(q, head);
#if defined(OTR_TRACE_TPI) && defined(OTR_DIAGNOSTICS)
	capture_handoff((struct endpoint *)arg, handed);
#else
	(void)handed;
#endif
	if ((type == OTB_R_PROTO || type == OTB_R_PCPROTO) && clen >= 4 &&
	    !ctl[0] && !ctl[1] && !ctl[2] &&
	    (ctl[3] == 129 || (ctl[3] == 127 && clen >= 8 &&
	    !ctl[4] && !ctl[5] && !ctl[6] && ctl[7] == 107)))
		((struct endpoint *)arg)->info_done = 1;
	return OTR_DONE;
}
static long
read_host(arg, fd, bytes, length)
	void *arg;
	int fd;
	unsigned char *bytes;
	unsigned long length;
{
	struct endpoint *e = arg;
	long n = otr_host_read(arg, fd, bytes, length);
	if (n == OTR_HOST_EOF) e->eof = 1;
	return n;
}
static const struct otr_provider_ops relay_ops = {
	read_host, otr_host_write, close_host, release, deliver
};

static void
notify(e)
	struct endpoint *e;
{
	if (!e->notified)
		e->notified = e->eof ? putnextctl(e->read, M_HANGUP) :
		    putnextctl1(e->read, M_ERROR, EIO_OT);
}
static long
service(q)
	void *q;
{
	struct endpoint *e = enter(q);
	struct otr_msg *mp;
	int n = OTR_WAIT, budget = 8;
	if (!e) return 0;
	trace_packet(e, 1, 0, 0, 0);
	if (e->failed) {
		notify(e);
		if (!e->notified && e->interval < 64) e->interval *= 2;
		goto out;
	}
	while (budget-- && !e->relay.pending && (mp = getq(e->write)) != 0) {
		if (e->closing) { freemsg(mp); goto out; }
		n = otr_provider_send(&e->relay, mp);
		if (n < 0) { freemsg(mp); e->failed = 1; break; }
		if (!n) { putbq(e->write, mp); break; }
	}
	if (e->closing) goto out;
	n = e->failed ? OTR_BAD : otr_provider_pump(&e->relay, 8);
	if (!e->closing && n < 0) {
		e->failed = 1; otr_provider_close(&e->relay); notify(e);
	}
	if (n == OTR_DONE) e->interval = TIMER_MS;
	else if (e->interval < 64) e->interval *= 2;
out:
#if defined(OTR_TRACE_TPI) && defined(OTR_DIAGNOSTICS)
    capture_pump(e, n);
#endif
	if (!e->closing && (!e->failed || !e->notified)) mi_timer(e->timer, e->interval);
	leave(e);
	return 0;
}
static long
readput(q, mp)
	void *q;
	struct otr_msg *mp;
{
	struct endpoint *e = enter(q);
	if (!e) { freemsg(mp); return 0; }
	if (mp == e->timer) {
		if (mi_timer_valid(mp) && !e->closing) qenable(e->read);
	} else freemsg(mp);
	leave(e);
	return 0;
}
static void
accept_word(unsigned char *p, unsigned long n)
{
    p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;
}

/* Native acceptor read queues identify retained endpoints, not host cookies. */
static int
accept_control(struct endpoint *e, struct otr_msg **message)
{
    struct endpoint *target;
    struct otr_msg *m=*message,*copy;
    unsigned long n,token,i,error=4,unixerror=0;
    if(m->block->type!=OTR_M_PROTO && m->block->type!=OTR_M_PCPROTO)return 0;
    if(m->read<m->block->base || m->write>m->block->limit || m->write<m->read)return 0;
    n=m->write-m->read;
    if(n<20 || n>OTB_MAXCTL || m->read[0] || m->read[1] || m->read[2] || m->read[3]!=103)return 0;
    token=((unsigned long)m->read[4]<<24)|((unsigned long)m->read[5]<<16)|
        ((unsigned long)m->read[6]<<8)|m->read[7];
    for(target=owned;target;target=target->owned_next)
        if(!target->closing && !target->failed && target->relay.live &&
           e->protocol==OTR_TCP && target->protocol==OTR_TCP &&
           token==(unsigned long)target->read)break;
    if(target) {
        copy=allocb(n,2L);
        if(copy) {
            copy->block->type=m->block->type;copy->band=m->band;copy->flags=m->flags;
            for(i=0;i<n;i++)*copy->write++=m->read[i];
            accept_word(copy->read+4,target->identity);
            copy->cont=m->cont;m->cont=0;freeb(m);*message=copy;return 0;
        }
        error=8;unixerror=ENOMEM_OT;
    }
    copy=allocb(16,2L);
    if(copy) {
        copy->block->type=OTR_M_PCPROTO;
        accept_word(copy->write,127);accept_word(copy->write+4,103);
        accept_word(copy->write+8,error);accept_word(copy->write+12,unixerror);
        copy->write+=16;
    }
    freemsg(m);
    if(copy)qreply(e->write,copy);
    else {e->failed=1;otr_provider_close(&e->relay);notify(e);}
    return 1;
}

static long
writeput(q, mp)
	void *q;
	struct otr_msg *mp;
{
	struct endpoint *e = enter(q);
	struct ioctl_prefix *ioc;
	int type;
	if (!e) { freemsg(mp); return 0; }
	if (e->failed) { freemsg(mp); goto out; }
	type = mp->block->type;
    if ((type == OTR_M_PROTO || type == OTR_M_PCPROTO) &&
        mp->read >= mp->block->base && mp->write <= mp->block->limit && mp->write >= mp->read)
        trace_packet(e, 0, type, mp->read, (unsigned long)(mp->write - mp->read));
#if defined(OTR_TRACE_ACCEPT) || defined(OTR_DIAGNOSTICS)
	if ((type == OTR_M_PROTO || type == OTR_M_PCPROTO) &&
	    mp->read >= mp->block->base && mp->write <= mp->block->limit &&
	    mp->write >= mp->read && (unsigned long)(mp->write - mp->read) >= 8 &&
	    !mp->read[0] && !mp->read[1] && !mp->read[2] && mp->read[3] == 103) {
		unsigned long token = ((unsigned long)mp->read[4] << 24) |
		    ((unsigned long)mp->read[5] << 16) | ((unsigned long)mp->read[6] << 8) | mp->read[7];
#ifdef OTR_DIAGNOSTICS
		capture_accept(e, token);
#endif
#ifdef OTR_TRACE_ACCEPT
		trace_accept(e, token);
#endif
	}
#endif
    if(accept_control(e,&mp))goto out;
	/* The initial information operation borrows its caller's stack. */
	if (type == OTR_M_PCPROTO && !mp->cont &&
	    mp->read >= mp->block->base && mp->write <= mp->block->limit &&
	    mp->write >= mp->read && (unsigned long)(mp->write - mp->read) == 4 &&
	    !mp->read[0] && !mp->read[1] && !mp->read[2] && mp->read[3] == 107 &&
	    !e->relay.pending && !e->relay.pumping) {
		int status;
        e->info_done = 0;
        status = otr_provider_send(&e->relay, mp);
		if (status == OTR_DONE) {
			unsigned int pass, waits = 0;
			for (pass = 0; pass < 16; pass++) {
				unsigned long sent = e->relay.tx.sent;
				status = otr_provider_pump(&e->relay, 8);
				if (e->closing || status < 0 || e->info_done) break;
				if (e->relay.pending && e->relay.tx.sent != sent) continue;
				if (waits++ == 8 || otr_host_wait(e->boot.fd,
				    e->relay.pending ? 2 : 1, 100) < 0) {
					status = OTR_BAD; break;
				}
			}
			if (!e->closing && !e->info_done) status = OTR_BAD;
			if (!e->closing && status < 0) otr_provider_close(&e->relay);
			if (!e->closing && status < 0) { e->failed = 1; notify(e); }
			if (!e->closing && e->relay.live) qenable(e->read);
			goto out;
		}
		if (status < 0) {
			freemsg(mp); e->failed = 1;
			otr_provider_close(&e->relay); notify(e);
			goto out;
		}
	}
	if (type == M_FLUSH && mp->write > mp->read && !(*mp->read & ~3)) {
		int flags = *mp->read;
		if (otr_host_flush(e->boot.fd, flags) < 0) {
			freemsg(mp); e->failed = 1; otr_provider_close(&e->relay); notify(e);
			goto out;
		}
		otr_provider_flush(&e->relay, flags & 1, flags & 2);
		if (flags & 2) flushq(e->write, 0L);
		if (flags & 1) { *mp->read &= ~2; qreply(q, mp); }
		else freemsg(mp);
		goto out;
	}
	if (type == M_IOCTL && (unsigned long)(mp->write - mp->read) >= sizeof(*ioc)) {
		ioc = (struct ioctl_prefix *)mp->read;
#ifdef OTR_DIAGNOSTICS
		capture_ioctl(e, ioc->command, ioc->count);
#endif
		ioc->count = 0; ioc->error = EINVAL_OT; ioc->result = -1;
		mp->block->type = M_IOCNAK;
		if (mp->cont) { freemsg(mp->cont); mp->cont = 0; }
		qreply(q, mp);
		goto out;
	}
	if (type != OTR_M_DATA && type != OTR_M_PROTO && type != OTR_M_PCPROTO) {
		freemsg(mp); e->failed = 1;
		otr_provider_close(&e->relay); notify(e);
		goto out;
	}
	if (!putq(q, mp)) {
		freemsg(mp); e->failed = 1;
		otr_provider_close(&e->relay); notify(e);
	} else if (!e->closing) { e->interval = TIMER_MS; qenable(e->read); }
out:
	leave(e);
	return 0;
}
struct install_diagnostic {
    struct otr_install install;
#ifdef OTR_DIAGNOSTICS
#ifdef OTR_TRACE_TPI
    unsigned long words[264];
#else
    unsigned long words[16];
#endif
#endif
};
static struct install_diagnostic tcpinstall, udpinstall;
#ifdef OTR_DIAGNOSTICS
static unsigned long *diagnostics(int protocol)
{
    return protocol == OTR_TCP ? tcpinstall.words : udpinstall.words;
}
#define DSET(p,n,v) (diagnostics(p)[n] = (unsigned long)(v))
#define DINC(p,n) (++diagnostics(p)[n])
#ifdef OTR_TRACE_TPI
#define DINSTALL(p) , { 0x4f544447UL, 1, p, 0,0,0,0,0,0,0,0,0,0,0,0,0, 0x4f545450UL,0x000a00e0UL }
#else
#define DINSTALL(p) , { 0x4f544447UL, 1, p }
#endif
#if defined(__m68k__)
typedef char diagnostic_prefix[(offsetof(struct install_diagnostic, words) == 24) ? 1 : -1];
#endif
#else
#define DSET(p,n,v) ((void)0)
#define DINC(p,n) ((void)0)
#define DINSTALL(p)
#endif
#ifdef OTR_DIAGNOSTICS
static void
capture_ioctl(e, command, count)
	struct endpoint *e;
	long command, count;
{
	DSET(e->protocol,14,command);
	DSET(e->protocol,15,count);
}
static void
capture_accept(e, token)
	struct endpoint *e;
	unsigned long token;
{
	DSET(e->protocol,14,token);
	DSET(e->protocol,15,e->read);
#ifdef OTR_TRACE_TPI
    { void *q=e->read; unsigned int i;
      DSET(e->protocol,224,0x4f544143UL);DSET(e->protocol,225,1);
      DSET(e->protocol,226,token);DSET(e->protocol,227,e->identity);
      DSET(e->protocol,228,e->read);DSET(e->protocol,229,e->write);
      DSET(e->protocol,230,0);DSET(e->protocol,231,0);
      for(i=0;i<16 && q;i++) {
          DSET(e->protocol,232+i*2,q);DSET(e->protocol,233+i*2,WR(q));
          DSET(e->protocol,230,i+1);
          q=((struct otr_queue_prefix *)q)->next;
          if(q==e->read)break;
      }
      DSET(e->protocol,231,q!=0); }
#endif
}
#endif
#if defined(OTR_TRACE_TPI) && defined(OTR_DIAGNOSTICS)
static unsigned long capture_word(const unsigned char *p)
{
    return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3];
}
static void capture_live(struct endpoint *e, unsigned long *w)
{
    static const unsigned long signature[8] = {
        0x4e56ffdcUL,0x48e71c38UL,0x206e0008UL,0x20280014UL,
        0x2d40ffdcUL,0x246e000cUL,0x206a0014UL,0x1428000dUL };
    struct otr_queue_prefix *q = ((struct otr_queue_prefix *)e->read)->next;
    struct otr_qinit *qi;
    const unsigned char *code,*state;
    int i;
    for(i=44;i<224;i++)w[i]=0;
    if(!q || !q->info || !q->private)return;
    qi=q->info;
    if(!qi->put)return;
    code=(const unsigned char *)qi->put;
    for(i=0;i<8;i++)if(capture_word(code+i*4)!=signature[i])return;
    w[45]=(unsigned long)q->info;w[46]=(unsigned long)q->first;
    w[47]=(unsigned long)q->last;w[48]=(unsigned long)q->next;
    w[49]=(unsigned long)q->link;w[50]=(unsigned long)q->private;
    w[51]=(unsigned long)qi->put;
    for(i=0;i<8;i++)w[52+i]=signature[i];
    state=q->private;
    for(i=0;i<28;i++)w[60+i]=capture_word(state+i*4);
    state=(const unsigned char *)capture_word(state+36);
    for(i=0;i<4 && state;i++) {
        int j;
        w[88+i*8]=(unsigned long)state;
        for(j=0;j<6;j++)w[89+i*8+j]=capture_word(state+j*4);
        w[95+i*8]=1;
        state=(const unsigned char *)w[89+i*8];
    }
    if(q->link) {
        const unsigned char *link=(const unsigned char *)q->link;
        w[120]=1;w[121]=(unsigned long)link;
        for(i=0;i<12;i++)w[122+i]=capture_word(link+i*4);
    }
    w[44]=1;
}
static void
capture_packet(e, phase, type, bytes, length)
    struct endpoint *e;
    int phase, type;
    const unsigned char *bytes;
    unsigned long length;
{
    unsigned long *w = diagnostics(e->protocol), primitive = 0xffffffffUL;
    if (w[18 + phase] < 0xffffffffUL) ++w[18 + phase];
    if (phase == 1) return;
    if (bytes && length >= 4)
        primitive = ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
            ((unsigned long)bytes[2] << 8) | bytes[3];
    if (phase == 0) {
        if (w[18] == 1) w[21] = primitive;
        w[22] = primitive; w[26] = type;
    } else {
        if (w[20] == 1) w[23] = primitive;
        w[24] = primitive; w[27] = type;
        if (primitive == 129 && length >= 44) {
            int i;
            for (i = 0; i < 11; i++) {
                const unsigned char *b = bytes + i * 4;
                w[28+i] = ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) |
                    ((unsigned long)b[2] << 8) | b[3];
            }
        }
        capture_live(e,w);
        w[39] = (unsigned long)e->read;
        w[40] = (unsigned long)((struct otr_queue_prefix *)e->read)->next;
    }
}
static void
capture_handoff(e, result)
    struct endpoint *e;
    int result;
{
    unsigned long *w = diagnostics(e->protocol);
    if (w[41] < 0xffffffffUL) ++w[41];
    w[42] = result; w[43] = e->closing;
    if(!e->closing && w[44]) {
        struct otr_queue_prefix *q=((struct otr_queue_prefix *)e->read)->next;
        const unsigned char *code,*state,*link;
        struct otr_qinit *qi;
        int i;
        if(!q || !q->info || !q->private)return;
        qi=q->info;if(!qi->put)return;
        code=(const unsigned char *)qi->put;
        for(i=0;i<8;i++)if(capture_word(code+i*4)!=w[52+i])return;
        w[134]=1;w[135]=(unsigned long)q;w[136]=(unsigned long)q->link;
        if(q->link) {
            link=(const unsigned char *)q->link;
            for(i=0;i<12;i++)w[137+i]=capture_word(link+i*4);
        }
        {
            const unsigned char *producer=code+0x80e,*slot,*globals,*callback;
            unsigned long address,fn;
            if(producer[0]==0x2f && producer[1]==0x08 &&
                producer[2]==0x20 && producer[3]==0x79 &&
                capture_word(producer+8)==0x41e801fcUL &&
                producer[12]==0x2f && producer[13]==0x08 &&
                producer[14]==0x4e && producer[15]==0xb9) {
                address=capture_word(producer+4);w[177]=(unsigned long)producer;w[178]=address;
                if(address>=2 && address<=0x7ffffffbUL && !(address&1)) {
                    slot=(const unsigned char *)address;address=capture_word(slot);w[179]=address;
                    if(address>=2 && address<=0x7ffffdffUL && !(address&1)) {
                        globals=(const unsigned char *)address;w[176]=1;
                        w[180]=capture_word(globals+156);w[181]=capture_word(globals+272);
                        w[182]=capture_word(globals+308);w[183]=capture_word(globals+312);
                        w[184]=capture_word(globals+340);w[185]=capture_word(globals+344);
                        w[186]=capture_word(globals+348);w[187]=capture_word(globals+352);
                        w[188]=capture_word(globals+504);w[189]=capture_word(globals+508);
                        address=capture_word(globals+404);w[218]=address;
                        if(address>=2 && address<=0x7fffffe7UL && !(address&1)) {
                            const unsigned char *published=(const unsigned char *)address;
                            for(i=0;i<5;i++)w[219+i]=capture_word(published+i*4);
                        }
                        fn=w[183];
                        if(fn>=2 && fn<=0x7fffffefUL && !(fn&1)) {
                            callback=(const unsigned char *)fn;
                            for(i=0;i<4;i++)w[190+i]=capture_word(callback+i*4);
                            if(w[190]==0x4e560000UL && w[191]==0x2f0a7000UL &&
                                callback[8]==0x20 && callback[9]==0x79 &&
                                callback[14]==0x41 && callback[15]==0xe8 &&
                                callback[16]==1 && callback[17]==0x5e &&
                                fn<=0x7fffff77UL &&
                                capture_word(callback+0x78)==0x42a74879UL &&
                                callback[0x80]==0x4e && callback[0x81]==0xb9 &&
                                callback[0x86]==0x50 && callback[0x87]==0x4f) {
                                const unsigned char *stub,*delegate,*descriptor;
                                unsigned long binding,target,desc;
                                target=capture_word(callback+0x82);
                                if(target<2 || target>0x7fffffe7UL || (target&1))return;
                                stub=(const unsigned char *)target;
                                w[201]=(unsigned long)stub;
                                for(i=0;i<6;i++)w[194+i]=capture_word(stub+i*4);
                                if(stub[0]==0x41 && stub[1]==0xf9) {
                                    binding=capture_word(stub+2);w[202]=binding;
                                    if(binding>=2 && binding<=0x7ffffffbUL && !(binding&1)) {
                                        target=capture_word((const unsigned char *)binding);w[203]=target;
                                        if(target>=2 && target<=0x7fffffe7UL && !(target&1)) {
                                            delegate=(const unsigned char *)target;
                                            for(i=0;i<6;i++)w[212+i]=capture_word(delegate+i*4);
                                            if(delegate[0]==0x41 && delegate[1]==0xf9 &&
                                                capture_word(delegate+6)==0x4a906602UL &&
                                                capture_word(delegate+10)==0x4e754aa8UL &&
                                                capture_word(delegate+14)==0x00086718UL) {
                                                desc=capture_word(delegate+2);w[204]=desc;
                                                if(desc>=2 && desc<=0x7fffffe7UL && !(desc&1)) {
                                                    descriptor=(const unsigned char *)desc;
                                                    for(i=0;i<6;i++)w[205+i]=capture_word(descriptor+i*4);
                                                    w[200]=1;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        w[149]=(unsigned long)q->first;w[150]=(unsigned long)q->last;
        state=(const unsigned char *)q->private;
        w[151]=capture_word(state+12);w[152]=capture_word(state+36);
        w[159]=capture_word(state+100);
        state=(const unsigned char *)w[152];
        if(state) {
            unsigned long context,cell,queue,sync;
            for(i=0;i<6;i++)w[153+i]=capture_word(state+i*4);
            context=w[156];w[160]=context;
            if(context<2 || context>0x7fffffe5UL || (context&1))return;
            state=(const unsigned char *)context;
            for(i=0;i<6;i++)w[162+i]=capture_word(state+i*4);
            w[168]=((unsigned long)state[24]<<24)|((unsigned long)state[25]<<16);
            w[161]=1;w[174]=state[25];w[175]=w[154];
            cell=capture_word(state);w[170]=cell;
            if(cell<2 || cell>0x7ffffffbUL || (cell&1))return;
            queue=capture_word((const unsigned char *)cell);w[171]=queue;
            if(queue<2 || queue>0x7fffffebUL || (queue&1))return;
            sync=capture_word((const unsigned char *)queue+16);w[172]=sync;
            if(sync<2 || sync>0x7fffffdfUL || (sync&1))return;
            w[173]=*((const unsigned char *)sync+32);
        }
    }
}
static void
capture_pump(e, result)
    struct endpoint *e;
    int result;
{
    DSET(e->protocol, 25, result);
}
#endif
static unsigned long
clone_device(unsigned long device)
{
    struct endpoint *e;
    unsigned long candidate = (device & 0xffff0000UL) | 10;
    for (e = clones; e; e = e->next) {
        if (e->device > candidate) break;
        if (e->device == candidate) {
            if ((candidate & 0xffffUL) == 0xffffUL) return 0;
            ++candidate;
        }
    }
    return candidate;
}
static void
clone_insert(struct endpoint *e)
{
    struct endpoint **p;
    for (p = &clones; *p && (*p)->device < e->device; p = &(*p)->next) {}
    e->next = *p; *p = e;
}
static void
clone_remove(struct endpoint *e)
{
    struct endpoint **p;
    for (p = &clones; *p; p = &(*p)->next)
        if (*p == e) { *p = e->next; break; }
    e->cloned = 0;
}
static long
open_endpoint(q, protocol, sflag, dev)
	void *q;
	unsigned long *dev;
	int protocol;
	long sflag;
{
	struct endpoint *e;
	unsigned long i, device = 0, previous = 0;
	DINC(protocol,3);DSET(protocol,4,sflag);DSET(protocol,5,((struct otr_queue_prefix *)q)->private);
    DSET(protocol,6,1);DSET(protocol,7,0);
    if (((struct otr_queue_prefix *)q)->private) {
        e = ((struct otr_queue_prefix *)q)->private;
        if (sflag == OTR_CLONEOPEN && dev && e->cloned) *dev = e->device;
        DSET(protocol,6,10);return 0;
    }
	trace_load("otrelay I qopen begin\n");
	if (sflag != OTR_MODOPEN && sflag != OTR_CLONEOPEN) {DSET(protocol,7,EINVAL_OT);return EINVAL_OT;}
	if (sflag == OTR_CLONEOPEN) {
        if (!dev) {DSET(protocol,7,EINVAL_OT);return EINVAL_OT;}
        previous = *dev; device = clone_device(previous);
        DSET(protocol,14,previous);DSET(protocol,15,device);
        if (!device) {DSET(protocol,7,ENOMEM_OT);return ENOMEM_OT;}
    }
	if (opening || cookie == 0xffffffffUL) {DSET(protocol,6,2);DSET(protocol,7,EINVAL_OT);return EINVAL_OT;}
	opening = 1;
	DSET(protocol,6,3);
	e = OTAllocMem(sizeof(*e));
	trace_load("otrelay I qopen allocation returned\n");
	if (!e) { opening = 0;DSET(protocol,7,ENOMEM_OT);return ENOMEM_OT; }
	for (i = 0; i < sizeof(*e); i++) ((unsigned char *)e)[i] = 0;
	e->read = q; e->write = WR(q); e->interval = TIMER_MS;
	e->protocol = protocol;
	DSET(protocol,6,4);
	if (!session.live) {long status=otr_boot_session(&session, &otr_host_boot, 0);
        DSET(protocol,12,otr_host_step);DSET(protocol,13,otr_host_result);
        DSET(protocol,8,status);if(status<0)goto bad;}
	e->identity = ++cookie;
	DSET(protocol,6,5);
	{long status=otr_boot_join(&e->boot, &session, protocol, e->identity);
        DSET(protocol,12,otr_host_step);DSET(protocol,13,otr_host_result);
        DSET(protocol,9,status);if(status<0)goto bad;}
	DSET(protocol,6,6);
	{long status=otr_provider_open(&e->relay, &relay_ops, e, q, e->boot.fd);
        DSET(protocol,10,status);if(status<0)goto bad;}
	trace_load("otrelay I qopen native endpoint joined\n");
	DSET(protocol,6,7);
	e->timer = mi_timer_alloc(q, 0);
	DSET(protocol,11,e->timer==0);
	if (!e->timer) goto bad;
	((struct otr_queue_prefix *)q)->private = e;
	((struct otr_queue_prefix *)e->write)->private = e;
	e->frames = 1;
    if (sflag == OTR_CLONEOPEN) {
        e->device = device; e->cloned = 1;
        clone_insert(e);
        *dev = device;
    }
    e->owned_next=owned;owned=e;
	qprocson(q);
	trace_load("otrelay I qopen queues enabled\n");
	if (!e->closing) mi_timer(e->timer, TIMER_MS);
	i = e->closing;DSET(protocol,6,8);DSET(protocol,7,i ? EINVAL_OT : 0);
	if (i && sflag == OTR_CLONEOPEN) *dev = previous;
	opening = 0; leave(e);
	return i ? EINVAL_OT : 0;
bad:
	DSET(protocol,7,ENOMEM_OT);
	trace_load("otrelay I qopen rollback\n");
	otr_provider_close(&e->relay); otr_boot_close(&e->boot);
	if (!session.children) otr_boot_close(&session);
	OTFreeMem(e); opening = 0;
	return ENOMEM_OT;
}
static long
tcpopen(q, dev, flags, sflag, credentials)
	void *q, *dev, *credentials;
	long flags, sflag;
{
	(void)flags; (void)credentials;
	return open_endpoint(q, OTR_TCP, sflag, dev);
}
static long
udpopen(q, dev, flags, sflag, credentials)
	void *q, *dev, *credentials;
	long flags, sflag;
{
	(void)flags; (void)credentials;
	return open_endpoint(q, OTR_UDP, sflag, dev);
}
static long
close_endpoint(q, flags, credentials)
	void *q, *credentials;
	long flags;
{
	struct endpoint *e = enter(q);
	struct otr_msg *mp;
	(void)flags; (void)credentials;
	if (!e) return 0;
	e->closing = 1;
    {struct endpoint **p;for(p=&owned;*p;p=&(*p)->owned_next)
        if(*p==e){*p=e->owned_next;break;}}
	if (e->cloned) clone_remove(e);
#if defined(OTR_TRACE_TPI) && defined(OTR_DIAGNOSTICS)
    { unsigned long *w = diagnostics(e->protocol);
      if (w[39] == (unsigned long)e->read) {
          w[43] = 1; w[39] = w[40] = 0;
      } }
#endif
	((struct otr_queue_prefix *)e->read)->private = 0;
	((struct otr_queue_prefix *)e->write)->private = 0;
	qprocsoff(q);
	mi_timer_cancel(e->timer);
	while ((mp = getq(e->read)) != 0) {
		if (mp == e->timer) (void)mi_timer_valid(mp);
		else freemsg(mp);
	}
	mi_timer_free(e->timer);
	flushq(e->write, 1L);
	otr_provider_close(&e->relay);
	leave(e);
	return 0;
}
static struct otr_module_info tcpinfo = { 0, "auxtcp", 0, 65536L, 65536UL, 8192UL };
static struct otr_module_info udpinfo = { 0, "auxudp", 0, 65536L, 65536UL, 8192UL };
static struct otr_qinit tcpr = { readput, service, tcpopen, close_endpoint, 0, &tcpinfo, 0 };
static struct otr_qinit tcpw = { writeput, service, 0, 0, 0, &tcpinfo, 0 };
static struct otr_qinit udpr = { readput, service, udpopen, close_endpoint, 0, &udpinfo, 0 };
static struct otr_qinit udpw = { writeput, service, 0, 0, 0, &udpinfo, 0 };
static struct otr_streamtab tcptab = { &tcpr, &tcpw, 0, 0 };
static struct otr_streamtab udptab = { &udpr, &udpw, 0, 0 };
static struct install_diagnostic tcpinstall = { { &tcptab, 0x1001UL, 4UL, 0, 0, 0 } DINSTALL(OTR_TCP) };
static struct install_diagnostic udpinstall = { { &udptab, 0x1001UL, 4UL, 0, 0, 0 } DINSTALL(OTR_UDP) };
struct otr_install *GetOTauxtcpInstallInfo(void) { trace_load("otrelay I getter tcp\n"); return &tcpinstall.install; }
struct otr_install *GetOTauxudpInstallInfo(void) { trace_load("otrelay I getter udp\n"); return &udpinstall.install; }
