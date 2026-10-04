/*
 * auxfid.c -- fidop, the File ID daemon's message queues, and csop,
 * the catalog-search daemon's, which answers as with no daemon.
 *
 *	Mac side --1 send--> requests --2 receive--> fidd
 *	Mac side <--4 receive-- replies <--3 reply-- fidd
 *
 * A message is {type, length, data}; the caller passes a 16-byte
 * header {type, buf, bufsize, len}.  Receives block until a message
 * is queued; a send returns the message's id, which 5 cancels.
 *
 * K&R C.
 */

#include "auxcore.h"

#define	FQ_MAX		20		/* messages a queue holds */
#define	FQ_MAXLEN	0x2000		/* bytes a message holds */

struct fidmsg {
	struct fidmsg	*m_next;
	long		m_id;
	long		m_type;
	long		m_len;
	/* m_len bytes follow */
};

struct fidq {
	struct fidmsg	*q_head;	/* oldest first */
	int		q_count;
};

static struct fidq fid_cq, fid_rq;	/* requests, replies */
static long fid_nextid;
static uid_t fid_uid;			/* the client with a request out */

static void
fq_free(m)
	struct fidmsg *m;
{
	kmem_free((_VOID *)m, sizeof *m + m->m_len);
}

static void
fq_clear(q)
	struct fidq *q;
{
	struct fidmsg *m;

	while ((m = q->q_head) != 0) {
		q->q_head = m->m_next;
		fq_free(m);
	}
	q->q_count = 0;
}

/* queue the caller's message; its id in *idp */
static int
fq_send(q, uh, idp)
	struct fidq *q;
	caddr_t uh;
	long *idp;
{
	long h[4];
	struct fidmsg *m, **pp;

	if (copyin(uh, (caddr_t)h, sizeof h))
		return EFAULT;
	if (h[3] <= 0 || h[2] < h[3])
		return EINVAL;
	if (h[3] > FQ_MAXLEN || q->q_count > FQ_MAX)
		return ENOMEM;
	m = (struct fidmsg *)kmem_alloc(sizeof *m + h[3], KM_SLEEP);
	m->m_len = h[3];
	if (copyin((caddr_t)h[1], (caddr_t)(m + 1), (u_int)h[3])) {
		fq_free(m);
		return EFAULT;
	}
	m->m_next = 0;
	m->m_type = h[0];
	m->m_id = *idp = fid_nextid++;
	for (pp = &q->q_head; *pp; pp = &(*pp)->m_next)
		;
	*pp = m;
	q->q_count++;
	wakeup((caddr_t)q);
	return 0;
}

/* wait for the oldest message and hand it over */
static int
fq_recv(q, uh)
	struct fidq *q;
	caddr_t uh;
{
	long h[4];
	struct fidmsg *m;

	if (copyin(uh, (caddr_t)h, sizeof h))
		return EFAULT;
	while (q->q_head == 0)
		if (sleep((caddr_t)q, (PZERO + 1) | PCATCH))
			return EINTR;
	m = q->q_head;
	if (h[2] > 0 && h[2] < m->m_len)
		return EINVAL;
	/* off the queue before copyout can sleep; back at the head on a fault */
	q->q_head = m->m_next;
	q->q_count--;
	h[0] = m->m_type;
	h[3] = m->m_len;
	if ((h[2] > 0 && copyout((caddr_t)(m + 1), (caddr_t)h[1], (u_int)m->m_len)) ||
	    copyout((caddr_t)h, uh, sizeof h)) {
		m->m_next = q->q_head;
		q->q_head = m;
		q->q_count++;
		return EFAULT;
	}
	fq_free(m);
	return 0;
}

static int
fq_cancel(q, id)
	struct fidq *q;
	long id;
{
	struct fidmsg *m, **pp;

	for (pp = &q->q_head; (m = *pp) != 0; pp = &m->m_next)
		if (m->m_id == id) {
			*pp = m->m_next;
			q->q_count--;
			fq_free(m);
			return 0;
		}
	return EINVAL;
}

/* fidop(op, hdr): 1 send, 2 serve, 3 reply, 4 take reply, 5 cancel */
int
aux_fidop(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	int e;

	switch (a[0]) {
	case 1: case 4: case 5:
		/* the client's own queue: another user waits until it is empty */
		if ((fid_cq.q_count || fid_rq.q_count) && u.u_cred->cr_uid != fid_uid &&
		    !suser(u.u_cred))
			return EPERM;
		break;
	case 2: case 3:
		/* the daemon's side: root */
		if (!suser(u.u_cred))
			return EPERM;
		break;
	}
	switch (a[0]) {
	case 1:
		/* one client: a new request drops stale replies */
		fq_clear(&fid_rq);
		if ((e = fq_send(&fid_cq, (caddr_t)a[1], &rv->r_val1)) == 0)
			fid_uid = u.u_cred->cr_uid;
		return e;
	case 2:
		return fq_recv(&fid_cq, (caddr_t)a[1]);
	case 3:
		return fq_send(&fid_rq, (caddr_t)a[1], &rv->r_val1);
	case 4:
		return fq_recv(&fid_rq, (caddr_t)a[1]);
	case 5:
		return fq_cancel(&fid_cq, a[1]);
	}
	return EINVAL;
}

/*
 * csop(sel, buf, len): no catalog-search daemon.  "Ready" and its pid
 * read 0, so the Mac side searches by itself; requests fail.
 */
int
aux_csop(ap, a, rv, r)
	struct aux_proc *ap;
	long *a;
	rval_t *rv;
	char *r;
{
	switch (a[0]) {
	case 8: case 10:		/* GET_READY, GET_PID */
		rv->r_val1 = 0;
		return 0;
	case 1:				/* SEND_REQUEST */
		return AUXE_AGAIN;
	case 6:				/* POLL_REPLY */
		return AUXE_WOULDBLOCK;
	}
	return EINVAL;
}

void
aux_fidfree()
{
	fq_clear(&fid_cq);
	fq_clear(&fid_rq);
}
