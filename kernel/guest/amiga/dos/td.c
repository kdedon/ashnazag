/*
 * trackdisk.device for the environment: DF0-DF3 served by the floppy
 * helper, on host drives (raw tracks) or ADF files.  Quick commands
 * answer from the helper's unit status; the rest queue for the device
 * task, which also polls drives for disk changes.  ROM code: all state
 * is in the device base.
 */
#include "../floppyshare.h"
typedef unsigned int U;
typedef int L;
typedef unsigned char B;
typedef unsigned short W;

extern U xcall(void *, void *, U *);

#define EXEC execbase()
#define P(x) ((U)(x))
#define W16(p) (*(volatile W *)(p))
#define L32(p) (*(volatile U *)(p))

/* IOStdReq and IOExtTD */
#define IO_DEVICE 20
#define IO_UNIT 24
#define IO_COMMAND 28
#define IO_FLAGS 30
#define IO_ERROR 31
#define IO_ACTUAL 32
#define IO_LENGTH 36
#define IO_DATA 40
#define IO_OFFSET 44
#define IOTD_COUNT 48
#define IOF_QUICK 1
#define IOTDF_INDEXSYNC 0x10
#define IOTDF_WORDSYNC 0x20

#define CMD_READ 2
#define CMD_WRITE 3
#define CMD_UPDATE 4
#define CMD_CLEAR 5
#define TD_MOTOR 9
#define TD_FORMAT 11
#define TD_REMOVE 12
#define TD_CHANGENUM 13
#define TD_CHANGESTATE 14
#define TD_PROTSTATUS 15
#define TD_RAWREAD 16
#define TD_RAWWRITE 17
#define TD_GETDRIVETYPE 18
#define TD_GETNUMTRACKS 19
#define TD_ADDCHANGEINT 20
#define TD_REMCHANGEINT 21
#define TD_GETGEOMETRY 22
#define TD_EJECT 23
#define TD_LASTCOMM 24
#define TDF_EXTCOM 0x8000
#define NSCMD_DEVICEQUERY 0x4000

#define IOERR_ABORTED (-2)
#define IOERR_NOCMD (-3)
#define IOERR_BADLENGTH (-4)
#define TDERR_WRITEPROT 28
#define TDERR_DISKCHANGED 29
#define TDERR_BADUNITNUM 32

#define SIG_PORT 16
#define SIG_POLL 17
#define SIG_WAIT 18
#define POLL_SECS 2

struct tdu {
    B pub[64];			/* TDU_PublicUnit */
    B ints[12];			/* TD_ADDCHANGEINT requests */
    B *remove;			/* TD_REMOVE's interrupt */
    U seen, motor, index;
};
struct td {
    B lib[34];
    W pad;
    B port[34];
    W pad1;
    B task[92];
    B tport[34];
    W pad2;
    B wport[34];
    W pad3;
    B treq[40];
    B wreq[40];
    struct tdu u[MIG_FD_UNITS];
};

static B *execbase(void) { B *b; __asm__("move.l 4.w,%0" : "=a"(b)); return b; }

static U ex(L lvo, U d0, U d1, U a0, U a1)
{
    U r[14];
    int i;
    for (i = 0; i < 14; i++) r[i] = 0;
    r[0] = d0; r[1] = d1; r[8] = a0; r[9] = a1;
    return xcall(EXEC, EXEC + lvo, r);
}
#define Forbid() ex(-132, 0, 0, 0, 0)
#define Permit() ex(-138, 0, 0, 0, 0)
#define Disable() ex(-120, 0, 0, 0, 0)
#define Enable() ex(-126, 0, 0, 0, 0)
#define Cause(i) ex(-180, 0, 0, 0, P(i))
#define AddTail(l, n) ex(-246, 0, 0, P(l), P(n))
#define Remove(n) ex(-252, 0, 0, 0, P(n))
#define Wait(s) ex(-318, s, 0, 0, 0)
#define PutMsg(p, m) ex(-366, 0, 0, P(p), P(m))
#define GetMsg(p) ((B *)ex(-372, 0, 0, P(p), 0))
#define ReplyMsg(m) ex(-378, 0, 0, 0, P(m))
#define DoIO(r) ex(-456, 0, 0, 0, P(r))
#define SendIO(r) ex(-462, 0, 0, 0, P(r))
#define CopyMem(s, d, n) ex(-624, n, 0, P(s), P(d))

static volatile struct mig_fd *box(void) { return (volatile struct mig_fd *)MIG_FD_BASE; }

static void newlist(B *l)
{
    L32(l) = P(l + 4);
    L32(l + 4) = 0;
    L32(l + 8) = P(l);
}

static void initport(B *p, int sig, B *task)
{
    p[8] = 4;			/* NT_MSGPORT */
    p[14] = 0;			/* PA_SIGNAL */
    p[15] = sig;
    L32(p + 16) = P(task);
    newlist(p + 20);
}

static struct tdu *unitof(B *ior) { return (struct tdu *)L32(ior + IO_UNIT); }

/* asks the helper, waiting in 10 ms steps; trackdisk's error or 0 */
static L req(struct td *t, U op, U unit, U track, U len, U flags)
{
    volatile struct mig_fd *b = box();
    b->op = op; b->unit = unit; b->track = track; b->len = len; b->flags = flags;
    b->error = 0;
    MIG_FD_BARRIER();
    b->state = MIG_FD_REQUEST;
    *(volatile U *)MIG_FD_BELL = 2;
    while (b->state != MIG_FD_REPLY) {
        W16(t->wreq + IO_COMMAND) = 9;		/* TR_ADDREQUEST */
        L32(t->wreq + 32) = 0;
        L32(t->wreq + 36) = 10000;
        DoIO(t->wreq);
    }
    b->state = MIG_FD_IDLE;
    return (L)b->error;
}

static void changed(struct td *t, struct tdu *u)
{
    B *n;
    (void)t;
    Disable();
    for (n = (B *)L32(u->ints); L32(n); n = (B *)L32(n))
        Cause(L32(n + IO_DATA));
    if (u->remove)
        Cause(u->remove);
    Enable();
}

static void poll(struct td *t)
{
    volatile struct mig_fd *b = box();
    struct tdu *u;
    U i;
    for (i = 0; i < MIG_FD_UNITS; i++) {
        if (b->u[i].kind != MIG_FD_DRIVE)
            continue;
        u = &t->u[i];
        req(t, MIG_FD_STATUS, i, 0, 0, 0);
        if (b->u[i].changes != u->seen) {
            u->seen = b->u[i].changes;
            changed(t, u);
        }
    }
}

/* sectors: whole 512-byte sectors through the helper's track buffer */
static L sectors(struct td *t, B *ior, U cmd)
{
    volatile struct mig_fd *b = box();
    struct tdu *u = unitof(ior);
    volatile struct mig_fd_unit *s = &b->u[u->index];
    U off = L32(ior + IO_OFFSET), len = L32(ior + IO_LENGTH), ts = s->sectors * 512;
    B *data = (B *)L32(ior + IO_DATA), *buf = (B *)MIG_FD_DATA;
    U tr, so, n;
    L e = 0;
    if ((W16(ior + IO_COMMAND) & TDF_EXTCOM) && L32(ior + IOTD_COUNT) < s->changes)
        return TDERR_DISKCHANGED;
    if (!(s->flags & MIG_FD_DISK))
        return TDERR_DISKCHANGED;
    if ((off | len) & 511 || off + len > 160 * ts || off + len < off)
        return IOERR_BADLENGTH;
    if (cmd != CMD_READ && (s->flags & MIG_FD_WPROT))
        return TDERR_WRITEPROT;
    if (cmd == TD_FORMAT && (off % ts || len % ts))
        return IOERR_BADLENGTH;
    while (len) {
        tr = off / ts;
        so = off % ts;
        n = ts - so < len ? ts - so : len;
        if (cmd == CMD_READ) {
            if ((e = req(t, MIG_FD_READ, u->index, tr, 0, 0)) != 0)
                break;
            CopyMem(buf + so, data, n);
        } else {
            if (n < ts && (e = req(t, MIG_FD_READ, u->index, tr, 0, 0)) != 0)
                break;
            CopyMem(data, buf + so, n);
            if ((e = req(t, MIG_FD_WRITE, u->index, tr, 0, 0)) != 0)
                break;
        }
        data += n; off += n; len -= n;
        L32(ior + IO_ACTUAL) += n;
    }
    return e;
}

static L perform(struct td *t, B *ior)
{
    volatile struct mig_fd *b = box();
    struct tdu *u = unitof(ior);
    U cmd = W16(ior + IO_COMMAND) & ~TDF_EXTCOM, len = L32(ior + IO_LENGTH);
    L e;
    switch (cmd) {
    case CMD_READ:
    case CMD_WRITE:
    case TD_FORMAT:
        return sectors(t, ior, cmd);
    case CMD_UPDATE:
        return req(t, MIG_FD_UPDATE, u->index, 0, 0, 0);
    case CMD_CLEAR:
        return req(t, MIG_FD_CLEAR, u->index, 0, 0, 0);
    case TD_MOTOR:
        L32(ior + IO_ACTUAL) = u->motor;
        u->motor = len != 0;
        return req(t, MIG_FD_MOTOR, u->index, 0, u->motor, 0);
    case TD_RAWREAD:
    case TD_RAWWRITE:
        if (len == 0 || len > 32766 || (len & 1))
            return IOERR_BADLENGTH;
        if (cmd == TD_RAWWRITE)
            CopyMem((B *)L32(ior + IO_DATA), (B *)MIG_FD_DATA, len);
        e = req(t, cmd == TD_RAWREAD ? MIG_FD_RAWREAD : MIG_FD_RAWWRITE, u->index,
            L32(ior + IO_OFFSET), len,
            (ior[IO_FLAGS] & IOTDF_WORDSYNC ? MIG_FD_SYNC : 0) |
            (ior[IO_FLAGS] & IOTDF_INDEXSYNC ? MIG_FD_INDEX : 0));
        if (!e && cmd == TD_RAWREAD)
            CopyMem((B *)MIG_FD_DATA, (B *)L32(ior + IO_DATA), len);
        if (!e)
            L32(ior + IO_ACTUAL) = b->actual;
        return e;
    }
    return 0;
}

static const W cmds[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    0x8002, 0x8003, 0x8004, 0x8005, 0x8009, 0x800a, 0x800b, 0x8010, 0x8011, NSCMD_DEVICEQUERY, 0
};

/* answers now: status, change interrupts, no-ops; 1 if queued for the task */
static int quick(struct td *t, B *ior)
{
    volatile struct mig_fd_unit *s = &box()->u[unitof(ior)->index];
    struct tdu *u = unitof(ior);
    U cmd = W16(ior + IO_COMMAND), base = cmd & ~TDF_EXTCOM;
    B *d = (B *)L32(ior + IO_DATA);
    (void)t;
    if (cmd == NSCMD_DEVICEQUERY) {
        if (L32(ior + IO_LENGTH) < 16) { ior[IO_ERROR] = IOERR_BADLENGTH; return 0; }
        L32(d) = 0; L32(d + 4) = 16;
        W16(d + 8) = 5;		/* NSDEVTYPE_TRACKDISK */
        W16(d + 10) = 0;
        L32(d + 12) = P(cmds);
        L32(ior + IO_ACTUAL) = 16;
        return 0;
    }
    if ((cmd & TDF_EXTCOM) && base != CMD_READ && base != CMD_WRITE && base != CMD_UPDATE &&
        base != CMD_CLEAR && base != TD_MOTOR && base != 10 && base != TD_FORMAT &&
        base != TD_RAWREAD && base != TD_RAWWRITE) {
        ior[IO_ERROR] = IOERR_NOCMD;
        return 0;
    }
    switch (base) {
    case 1: case 6: case 7: case 8: case 10: case TD_EJECT:
        return 0;
    case TD_REMOVE:
        u->remove = d;
        return 0;
    case TD_CHANGENUM:
        L32(ior + IO_ACTUAL) = s->changes;
        return 0;
    case TD_CHANGESTATE:
        L32(ior + IO_ACTUAL) = !(s->flags & MIG_FD_DISK);
        return 0;
    case TD_PROTSTATUS:
        if (!(s->flags & MIG_FD_DISK)) { ior[IO_ERROR] = TDERR_DISKCHANGED; return 0; }
        L32(ior + IO_ACTUAL) = (s->flags & MIG_FD_WPROT) != 0;
        return 0;
    case TD_GETDRIVETYPE:
        L32(ior + IO_ACTUAL) = s->flags & MIG_FD_HD ? 3 : 1;
        return 0;
    case TD_GETNUMTRACKS:
        L32(ior + IO_ACTUAL) = 160;
        return 0;
    case TD_GETGEOMETRY:
        L32(d) = 512;
        L32(d + 4) = 160 * s->sectors;
        L32(d + 8) = 80;
        L32(d + 12) = 2 * s->sectors;
        L32(d + 16) = 2;
        L32(d + 20) = s->sectors;
        L32(d + 24) = 1;	/* MEMF_PUBLIC */
        d[28] = 0;		/* DG_DIRECT_ACCESS */
        d[29] = 1;		/* DGF_REMOVABLE */
        W16(d + 30) = 0;
        L32(ior + IO_ACTUAL) = 32;
        return 0;
    case TD_ADDCHANGEINT:
        ior[IO_FLAGS] &= ~IOF_QUICK;
        Disable();
        AddTail(u->ints, ior);
        Enable();
        return 2;
    case TD_REMCHANGEINT:
        Disable();
        Remove(ior);
        Enable();
        return 0;
    case CMD_READ: case CMD_WRITE: case CMD_UPDATE: case CMD_CLEAR: case TD_MOTOR:
    case TD_FORMAT: case TD_RAWREAD: case TD_RAWWRITE:
        return 1;
    }
    ior[IO_ERROR] = IOERR_NOCMD;
    return 0;
}

void td_beginio(struct td *t, B *ior)
{
    int q;
    ior[IO_ERROR] = 0;
    L32(ior + IO_ACTUAL) = 0;
    q = quick(t, ior);
    if (q == 1) {
        ior[IO_FLAGS] &= ~IOF_QUICK;
        PutMsg(t->port, ior);
    } else if (!q && !(ior[IO_FLAGS] & IOF_QUICK))
        ReplyMsg(ior);
}

void td_abortio(struct td *t, B *ior)
{
    B *n;
    int i;
    Forbid();
    for (n = (B *)L32(t->port + 20); L32(n); n = (B *)L32(n))
        if (n == ior)
            goto found;
    for (i = 0; i < MIG_FD_UNITS; i++)
        for (n = (B *)L32(t->u[i].ints); L32(n); n = (B *)L32(n))
            if (n == ior)
                goto found;
    Permit();
    return;
found:
    Disable();
    Remove(ior);
    Enable();
    ior[IO_ERROR] = IOERR_ABORTED;
    ReplyMsg(ior);
    Permit();
}

L td_open(struct td *t, B *ior, U unit)
{
    volatile struct mig_fd *b = box();
    if (unit >= MIG_FD_UNITS || b->magic != MIG_FD_MAGIC || b->version != MIG_FD_VERSION ||
        b->u[unit].kind == MIG_FD_NONE) {
        ior[IO_ERROR] = TDERR_BADUNITNUM;
        return TDERR_BADUNITNUM;
    }
    L32(ior + IO_UNIT) = P(t->u[unit].pub);
    W16(t->u[unit].pub + 36) += 1;
    W16(t->lib + 32) += 1;
    ior[IO_ERROR] = 0;
    return 0;
}

U td_close(struct td *t, B *ior)
{
    struct tdu *u = unitof(ior);
    W16(u->pub + 36) -= 1;
    W16(t->lib + 32) -= 1;
    L32(ior + IO_UNIT) = 0;
    L32(ior + IO_DEVICE) = 0;
    return 0;
}

/* the device task: queued requests, and a drive poll every POLL_SECS */
void td_task(void)
{
    B *self = (B *)ex(-294, 0, 0, 0, 0);
    struct td *t = (struct td *)L32(self + 88);
    B *ior;
    U r[14];
    int i;
    initport(t->tport, SIG_POLL, self);
    initport(t->wport, SIG_WAIT, self);
    for (i = 0; i < 2; i++) {
        B *q = i ? t->wreq : t->treq;
        int j;
        for (j = 0; j < 40; j++)
            q[j] = 0;
        q[8] = 5;			/* NT_MESSAGE */
        L32(q + 14) = P(i ? t->wport : t->tport);
        W16(q + 18) = 40;
        for (j = 0; j < 14; j++)
            r[j] = 0;
        /* polls on UNIT_VBLANK, waits for the helper on UNIT_MICROHZ */
        r[8] = P("timer.device"); r[0] = !i; r[9] = P(q);
        xcall(EXEC, EXEC - 444, r);
    }
    W16(t->treq + IO_COMMAND) = 9;
    L32(t->treq + 32) = POLL_SECS;
    L32(t->treq + 36) = 0;
    SendIO(t->treq);
    for (;;) {
        Wait(1U << SIG_PORT | 1U << SIG_POLL);
        while ((ior = GetMsg(t->port)) != 0) {
            ior[IO_ERROR] = (B)perform(t, ior);
            ReplyMsg(ior);
        }
        if (GetMsg(t->tport)) {
            poll(t);
            W16(t->treq + IO_COMMAND) = 9;
            L32(t->treq + 32) = POLL_SECS;
            L32(t->treq + 36) = 0;
            SendIO(t->treq);
        }
    }
}

extern const char td_name[], td_id[];

/* DFn: for DOS, DF0 bootable after the system volume */
static void dosnode(U unit, U sectors)
{
    static const U envec[20] = {
        19, 128, 0, 2, 1, 11, 2, 0, 0, 0, 79, 5, 1, 0x200000, 0x7ffffffe, 0,
        0x444f5300, 0, 0, 0
    };
    static const char *const names[MIG_FD_UNITS] = { "DF0", "DF1", "DF2", "DF3" };
    U pp[24], r[14], i;
    B *exp, *dn;
    for (i = 0; i < 14; i++) r[i] = 0;
    r[9] = P("expansion.library"); r[0] = 37;
    if (!(exp = (B *)xcall(EXEC, EXEC - 552, r)))
        return;
    pp[0] = P(names[unit]);
    pp[1] = P(td_name);
    pp[2] = unit;
    pp[3] = 0;
    for (i = 0; i < 20; i++)
        pp[4 + i] = envec[i];
    pp[4 + 5] = sectors == 22 ? 22 : 11;
    for (i = 0; i < 14; i++) r[i] = 0;
    r[8] = P(pp);
    if ((dn = (B *)xcall(exp, exp - 144, r)) != 0) {	/* MakeDosNode */
        for (i = 0; i < 14; i++) r[i] = 0;
        r[0] = unit ? (U)-128 : 5; r[8] = P(dn);
        xcall(exp, exp - 36, r);			/* AddBootNode */
    }
    for (i = 0; i < 14; i++) r[i] = 0;
    r[9] = P(exp);
    xcall(EXEC, EXEC - 414, r);
}

extern void td_vectors(void);

/* from the resident: the device, and its task when a unit is there */
B *td_init(void)
{
    volatile struct mig_fd *b = box();
    struct td *t;
    B *stack;
    U r[14], i, any = 0;
    for (i = 0; i < 14; i++) r[i] = 0;
    r[8] = P(td_vectors); r[0] = sizeof(struct td);
    t = (struct td *)xcall(EXEC, EXEC - 84, r);	/* MakeLibrary */
    if (!t)
        return 0;
    t->lib[8] = 3;			/* NT_DEVICE */
    L32(t->lib + 10) = P(td_name);
    t->lib[14] = 6;			/* LIBF_SUMUSED | LIBF_CHANGED */
    W16(t->lib + 20) = 47;
    W16(t->lib + 22) = 1;
    L32(t->lib + 24) = P(td_id);
    for (i = 0; i < MIG_FD_UNITS; i++) {
        struct tdu *u = &t->u[i];
        u->pub[8] = 6;			/* NT_UNIT */
        newlist(u->pub + 20);
        W16(u->pub + 56) = (W)-1;	/* tdu_CurrTrk: unknown */
        newlist(u->ints);
        u->index = i;
        if (b->magic == MIG_FD_MAGIC && b->version == MIG_FD_VERSION && b->u[i].kind) {
            u->seen = b->u[i].changes;
            any = 1;
        }
    }
    if (any && (stack = (B *)ex(-198, 4096, 0x10001, 0, 0)) != 0) {
        B *k = t->task;
        k[8] = 1;			/* NT_TASK */
        k[9] = 5;
        L32(k + 10) = P(td_name);
        L32(k + 18) = 0xffffU | 1U << SIG_PORT | 1U << SIG_POLL | 1U << SIG_WAIT;
        L32(k + 58) = P(stack);
        L32(k + 62) = P(stack + 4096);
        L32(k + 54) = P(stack + 4096);
        newlist(k + 74);
        L32(k + 88) = P(t);
        initport(t->port, SIG_PORT, k);
        for (i = 0; i < 14; i++) r[i] = 0;
        r[9] = P(k); r[10] = P(td_task);
        xcall(EXEC, EXEC - 282, r);	/* AddTask */
    }
    for (i = 0; i < 14; i++) r[i] = 0;
    r[9] = P(t);
    xcall(EXEC, EXEC - 432, r);		/* AddDevice */
    for (i = 0; i < MIG_FD_UNITS; i++)
        if (b->magic == MIG_FD_MAGIC && b->version == MIG_FD_VERSION && b->u[i].kind)
            dosnode(i, b->u[i].sectors);
    return t->lib;
}
