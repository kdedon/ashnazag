/*
 * Binds the Picasso96 board before DOS, from the files startmig copied
 * out of SYS: into the boot extension.  rtg.library's initialization
 * needs dos.library and icon.library, so stand-ins answer from those
 * files until the real libraries exist and pass every call on after.
 */
#include "../earlyshare.h"
#include "../inputshare.h"
typedef unsigned int U;
typedef int L;
typedef unsigned char B;
typedef unsigned short W;

/* r: d0-d7, a0-a5 in, out; the call's a6 and address */
extern U xcall(void *, void *, U *);
extern void fake_vectors(void), osl_stub(void), os_stub(void), sp_stub(void), alert_stub(void), talert_stub(void);
#define EXEC execbase()
#define D0 0
#define D1 1
#define D2 2
#define D3 3
#define A0 8
#define A1 9
#define A2 10
#define A3 11
#define A6 14
#define P(x) ((U)(x))
#define BPTR(x) ((U)(x) >> 2)
#define BADDR(x) ((B *)((U)(x) << 2))
#define SA_DisplayID 0x80000032U
#define SA_Type 0x8000002dU
#define SA_Depth 0x80000025U
#define SA_LikeWorkbench 0x80000047U
#define SA_Title 0x80000028U
#define WBENCHSCREEN 1
/* rtg.library 40.3945: the ENV handler's table of setting readers */
#define RTG_ENVREADERS 0x36bc
#define RTG_DISABLEBLITTER 7
#define RTG_HANDLER 816
/* a write here wakes the host's helpers */
#define MIG_FS_RING 0x00f7fffcUL

struct state {
    U succ, pred;
    B type, pri;
    const char *name;
    struct mig_early *dir;
    B *dos, *icon, *rtg;
    L ioerr;
    U procs[4], nprocs;
    U menu, boardid, bound, depth;
    void *oldosl;
    W tramp[6], tramp2[6], tramp3[6], peek[6];
    void *oldsetprefs;
    const B *sm;
    void *oldpeek;
    U *fssm;
    B *startless[4];
};
/* the stand-ins' fields after struct Library */
#define F_REAL 34
#define F_STATE 38
#define F_KIND 42
#define F_SIZE 44
struct fh { U magic; const B *data; U size, pos; };
#define FH_MAGIC 0x45464831U

void *memset(void *d, int c, unsigned long n) { B *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { B *p = d; const B *q = s; while (n--) *p++ = *q++; return d; }

static B *execbase(void) { B *b; __asm__("move.l 4.w,%0" : "=a"(b)); return b; }
static U lcall(void *base, L lvo, U *r) { return xcall(base, (B *)base + lvo, r); }
static U ex(L lvo, U d0, U d1, U a0, U a1)
{
    U r[14];
    memset(r, 0, sizeof r);
    r[D0] = d0; r[D1] = d1; r[A0] = a0; r[A1] = a1;
    return lcall(EXEC, lvo, r);
}
#define Forbid() ex(-132, 0, 0, 0, 0)
#define Permit() ex(-138, 0, 0, 0, 0)
#define AllocMem(n) ((B *)ex(-198, n, 0x10001, 0, 0))
#define FreeMem(p, n) ex(-210, n, 0, 0, P(p))
#define FindName(l, n) ex(-276, 0, 0, P(l), P(n))
#define OpenLibrary(n, v) ((B *)ex(-552, v, 0, 0, P(n)))
#define CloseLibrary(b) ex(-414, 0, 0, 0, P(b))
#define LIBLIST (EXEC + 378)

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return !*a && !*b;
}
static U slen(const char *s) { U n = 0; while (s[n]) n++; return n; }
static const char *filepart(const char *p)
{
    const char *f = p;
    for (; *p; p++)
        if (*p == ':' || *p == '/') f = p + 1;
    return f;
}
static const B *dfile(struct mig_early *d, const char *name, U *size)
{
    U i;
    for (i = 0; i < d->count && i < MIG_EARLY_FILES; i++)
        if (ieq(name, d->file[i].name)) {
            if (size) *size = d->file[i].size;
            return (B *)d + d->file[i].offset;
        }
    return 0;
}
#define pfile(s, name, size) dfile((s)->dir, name, size)
static struct mig_early_status *status(void) { return (struct mig_early_status *)MIG_EARLY_STATUS; }

static void unloadseg(U seg)
{
    B *p;
    while (seg) {
        p = BADDR(seg);
        seg = *(U *)p;
        FreeMem(p - 4, *(U *)(p - 4));
    }
}
/* the hunk n of a segment list's data, or 0 */
static B *hunk(U seg, U n)
{
    for (; seg && n; n--) seg = *(U *)BADDR(seg);
    return seg ? BADDR(seg) + 4 : 0;
}
/* a load file in memory as a segment list, as LoadSeg would */
static U loadseg(const B *f, U size)
{
    const U *w = (const U *)f, *e = (const U *)(f + size);
    U n, first, last, i, k, h, cur, type, seg = 0, *link = &seg;
    B *hk[64];
    if (size < 24 || *w++ != 0x3f3) return 0;
    while (w < e && *w) w += *w + 1;
    if (w + 3 > e) return 0;
    n = w[1]; first = w[2]; last = w[3]; w += 4;
    if (n > 64 || first || last != n - 1) return 0;
    for (i = 0; i < n; i++) {
        U sz = (*w & 0x3fffffff) * 4, fl = *w >> 30 == 1 ? 2 : 0;
        if (*w++ >> 30 == 3) w++;
        hk[i] = (B *)ex(-198, sz + 8, 0x10001 | fl, 0, 0);
        if (!hk[i]) { *link = 0; unloadseg(seg); return 0; }
        *(U *)hk[i] = sz + 8;
        *link = BPTR(hk[i] + 4);
        link = (U *)(hk[i] + 4);
    }
    *link = 0;
    for (cur = 0; w < e && cur < n; ) {
        type = *w++ & 0x3fffffff;
        switch (type) {
        case 0x3e9: case 0x3ea:
            k = *w++;
            memcpy(hk[cur] + 8, w, k * 4);
            w += k;
            break;
        case 0x3eb:
            w++;
            break;
        case 0x3ec:
            while ((k = *w++) != 0) {
                h = *w++;
                if (h >= n) goto bad;
                while (k--) *(U *)(hk[cur] + 8 + *w++) += P(hk[h] + 8);
            }
            break;
        case 0x3f7: case 0x3fc: {
            const W *s = (const W *)w;
            while ((k = *s++) != 0) {
                h = *s++;
                if (h >= n) goto bad;
                while (k--) *(U *)(hk[cur] + 8 + *s++) += P(hk[h] + 8);
            }
            w = (const U *)(((U)s + 3) & ~3U);
            break;
        }
        case 0x3f0:
            while ((k = *w++) != 0) w += k + 1;
            break;
        case 0x3f1: case 0x3e8:
            w += *w + 1;
            break;
        case 0x3f2:
            cur++;
            break;
        default:
            goto bad;
        }
    }
    ex(-636, 0, 0, 0, 0);
    return seg;
bad:
    unloadseg(seg);
    return 0;
}
/* the resident tag in a segment list */
static B *resident(U seg)
{
    B *p, *e;
    for (; seg; seg = *(U *)BADDR(seg)) {
        p = BADDR(seg) + 4;
        e = p + *(U *)(BADDR(seg) - 4) - 34;
        for (; p < e; p += 2)
            if (*(W *)p == 0x4afc && *(B **)(p + 2) == p) return p;
    }
    return 0;
}
/* load and initialize a library; its base, or 0 */
static B *initlib(struct state *s, const char *path, const char *rename, U *segp)
{
    U size, seg, r[14];
    const B *f = pfile(s, path, &size);
    B *res, *base;
    if (!f || !(seg = loadseg(f, size))) return 0;
    if (!(res = resident(seg))) { unloadseg(seg); return 0; }
    memset(r, 0, sizeof r);
    r[A1] = P(res); r[D1] = seg;
    if (!(base = (B *)lcall(EXEC, -102, r))) { unloadseg(seg); return 0; }
    if (rename) *(const char **)(base + 10) = rename;
    if (segp) *segp = seg;
    return base;
}

static B *fake(struct state *s, const char *name, W kind)
{
    U r[14];
    B *b;
    memset(r, 0, sizeof r);
    r[A0] = P(fake_vectors); r[D0] = F_SIZE;
    if (!(b = (B *)lcall(EXEC, -84, r))) return 0;
    b[8] = 9;
    *(const char **)(b + 10) = name;
    *(W *)(b + 20) = 47;
    *(B **)(b + F_STATE) = (B *)s;
    *(W *)(b + F_KIND) = kind;
    ex(-396, 0, 0, 0, P(b));
    return b;
}

/* the real library behind a stand-in, once it exists */
U early_resolve(B *f)
{
    struct state *s = *(struct state **)(f + F_STATE);
    B *real, *d;
    if (*(U *)(f + F_REAL)) return *(U *)(f + F_REAL);
    Forbid();
    d = (B *)FindName(LIBLIST, "dos.library");
    if (d == s->dos) d = 0;
    Permit();
    if (!d) return 0;
    real = *(W *)(f + F_KIND) ? OpenLibrary("icon.library", 0) : d;
    *(B **)(f + F_REAL) = real;
    return P(real);
}

/* tool type name=value in a list: the value, or 0 */
static U findtooltype(char **tt, const char *name)
{
    U n = slen(name), i;
    char *t;
    for (; tt && *tt; tt++) {
        t = *tt;
        for (i = 0; i < n && lower(t[i]) == lower(name[i]); i++)
            ;
        if (i == n && (t[n] == '=' || !t[n])) return P(t + n + (t[n] == '='));
    }
    return 0;
}
static U matchtoolvalue(const char *type, const char *value)
{
    U n = slen(value), i;
    while (*type) {
        for (i = 0; i < n && lower(type[i]) == lower(value[i]); i++)
            ;
        if (i == n && (!type[n] || type[n] == '|')) return 1;
        while (*type && *type++ != '|')
            ;
    }
    return 0;
}

/* a stand-in's function while its library does not exist yet */
void early_emulate(B *base, L off, U *r)
{
    struct state *s = *(struct state **)(base + F_STATE);
    struct fh *h;
    const char *name;
    U v = 0, size, seg;
    const B *f;
    if (off == 6) {
        ++*(W *)(base + 32);
        r[D0] = P(base);
        return;
    }
    if (off == 12) --*(W *)(base + 32);
    if (off <= 24) { r[D0] = 0; return; }
    if (*(W *)(base + F_KIND)) {
        if (off == 96) v = findtooltype((char **)r[A0], (char *)r[A1]);
        if (off == 102) v = matchtoolvalue((char *)r[A0], (char *)r[A1]);
        r[D0] = v;
        return;
    }
    if (off / 6 < 256) status()->calls[off / 6 / 32] |= 1U << (off / 6 % 32);
    name = (const char *)r[D1];
    h = (struct fh *)BADDR(r[D1]);
    switch (off) {
    case 30:        /* Open: the board's settings */
        f = ieq(filepart(name), "Picasso96Settings") ? pfile(s, "devs/picasso96settings", &size) : 0;
        if (!f || (h = (struct fh *)AllocMem(sizeof *h)) == 0) { s->ioerr = 205; break; }
        h->magic = FH_MAGIC; h->data = f; h->size = size; h->pos = 0;
        v = BPTR(h);
        break;
    case 36:
        if (r[D1] && h->magic == FH_MAGIC) { h->magic = 0; FreeMem(h, sizeof *h); }
        v = -1;
        break;
    case 42:
        if (!r[D1] || h->magic != FH_MAGIC) { v = -1; break; }
        v = h->size - h->pos < r[D3] ? h->size - h->pos : r[D3];
        memcpy((B *)r[D2], h->data + h->pos, v);
        h->pos += v;
        break;
    case 48:
        v = -1;
        break;
    case 66: {      /* Seek */
        L to = r[D2];
        if (!r[D1] || h->magic != FH_MAGIC) { v = -1; break; }
        if ((L)r[D3] == 0) to += h->pos;
        else if ((L)r[D3] == 1) to += h->size;
        if (to < 0 || (U)to > h->size) { v = -1; s->ioerr = 219; break; }
        v = h->pos;
        h->pos = to;
        break;
    }
    case 132:
        v = s->ioerr;
        break;
    case 462:
        v = s->ioerr;
        s->ioerr = r[D1];
        break;
    case 150: {     /* LoadSeg: a board driver */
        U i;
        name = filepart(name);
        for (i = 0; i < s->dir->count && i < MIG_EARLY_FILES; i++)
            if (ieq(name, filepart(s->dir->file[i].name))) {
                f = (B *)s->dir + s->dir->file[i].offset;
                if ((seg = loadseg(f, s->dir->file[i].size)) != 0) v = seg;
                break;
            }
        if (!v) s->ioerr = 205;
        break;
    }
    case 156:
        unloadseg(r[D1]);
        v = -1;
        break;
    case 498:       /* CreateNewProc: started once DOS is up */
        if (s->nprocs < 4) s->procs[s->nprocs++] = r[D1];
        break;
    case 870:
        v = P(filepart(name));
        break;
    case 876: {
        const char *p = filepart(name);
        if (p > name && p[-1] == '/') p--;
        v = P(p);
        break;
    }
    case 906:       /* GetVar */
        f = pfile(s, "prefs/env-archive/picasso96/disableamigablitter", &size);
        v = -1;
        if (f && ieq(filepart(name), "DisableAmigaBlitter") && r[D3]) {
            if (size >= r[D3]) size = r[D3] - 1;
            memcpy((B *)r[D2], f, size);
            ((B *)r[D2])[size] = 0;
            v = size;
        }
        break;
    }
    r[D0] = v;
}

/*
 * Native screens open in the board's mode and depth: nothing shows the
 * chipset's display.  Before DOS, P96 cannot load its planar emulation, so
 * screens in the board's mode get its depth too.  Workbench-like and
 * failing opens keep their own.
 */
void early_openscreen(struct state *s, U *r)
{
    B *ns = (B *)r[A0];
    U *t = (U *)r[A1], *e, tags[2 * 36], q[14], n = 2, id = ~0U, like = 0, native;
    if (ns) id = *(W *)(ns + 12);
    if (ns && (*(W *)(ns + 14) & 15) == WBENCHSCREEN) like = 1;
    /* an ExtNewScreen's own tags, which the list's override */
    if (ns && *(W *)(ns + 14) & 0x1000)
        for (e = *(U **)(ns + 32); e && e[0]; e += 2) {
            if (e[0] == 2) { e = (U *)e[1] - 2; continue; }
            if (e[0] == SA_DisplayID) id = e[1];
            if (e[0] == SA_LikeWorkbench && e[1]) like = 1;
        }
    while (t && t[0] && n < 34) {
        if (t[0] == 2) { t = (U *)t[1]; continue; }
        if (t[0] == 3) { t += 2 * (t[1] + 1); continue; }
        if (t[0] == SA_DisplayID) id = t[1];
        if (t[0] == SA_LikeWorkbench && t[1]) like = 1;
        if (t[0] == SA_Type && (t[1] & 15) == WBENCHSCREEN) like = 1;
        tags[2 * n] = t[0] == SA_DisplayID || t[0] == SA_Depth ? 1 : t[0];
        tags[2 * n + 1] = t[1];
        n++; t += 2;
    }
    memcpy(q, r, sizeof q);
    native = id == ~0U || !(id & 0xf0000000U);
    if (!like && (!t || !t[0]) && (native || !early_resolve(s->dos))) {
        U k = status()->screens++;
        tags[0] = SA_DisplayID; tags[1] = native ? s->boardid : id;
        tags[2] = SA_Depth; tags[3] = s->depth;
        tags[2 * n] = 0;
        q[A1] = P(tags);
        if ((r[D0] = xcall((B *)r[A6], s->oldosl, q)) == 0) {
            memcpy(q, r, sizeof q);
            r[D0] = xcall((B *)r[A6], s->oldosl, q);
        }
        if (k < 2) {
            status()->screen[2 * k] = (id & 0x7fffffff) | (r[D0] && q[A1] == P(tags) ? 0x80000000U : 0);
            status()->screen[2 * k + 1] = r[D0];
        }
        return;
    }
    r[D0] = xcall((B *)r[A6], s->oldosl, q);
}

/*
 * The boot menu opens when input.device reports both buttons held.  Events
 * written to it do not change that, so its PeekQualifier says so instead,
 * until the menu has returned.
 */
/*
 * DOS gives intuition the old preferences from devs:system-configuration
 * before the startup runs, which sets Workbench's mode back to a native
 * one: the board's mode again after that first call, as IPrefs will.
 */
void early_setprefs(struct state *s, U *r)
{
    U q[14], v;
    memcpy(q, r, sizeof q);
    r[D0] = xcall((B *)r[A6], s->oldsetprefs, q);
    memset(q, 0, sizeof q);
    q[A0] = P(s->sm); q[D0] = 12; q[D1] = 1;
    lcall((B *)r[A6], -576, q);
    Forbid();
    memset(q, 0, sizeof q);
    q[A1] = r[A6]; q[A0] = -324; q[D0] = P(s->oldsetprefs);
    v = lcall(EXEC, -420, q);
    if (v != P(s->tramp3)) {
        q[A1] = r[A6]; q[A0] = -324; q[D0] = v;
        lcall(EXEC, -420, q);
    }
    Permit();
}

static void buttons(struct state *s, int down)
{
    U r[14];
    B *in;
    Forbid();
    in = (B *)FindName(EXEC + 350, "input.device");
    if (in) {
        memset(r, 0, sizeof r);
        r[A1] = P(in); r[A0] = -42;
        if (down) {
            s->peek[0] = 0x4eb9;
            s->peek[3] = 0x0040; s->peek[4] = 0x6000;
            s->peek[5] = 0x4e75;
            r[D0] = P(s->peek);
            s->oldpeek = (void *)lcall(EXEC, -420, r);
            *(void **)&s->peek[1] = s->oldpeek;
            ex(-636, 0, 0, 0, 0);
        } else {
            r[D0] = P(s->oldpeek);
            lcall(EXEC, -420, r);
        }
    }
    Permit();
}

/*
 * The boot menu lists each boot node's DOS type from its startup message;
 * the session's node has none, so it gets one while the menu shows.
 */
static void startups(struct state *s, int on)
{
    B *x = OpenLibrary("expansion.library", 36), *n, *dn;
    U i = 0, *f = s->fssm;
    if (!x) return;
    Forbid();
    if (on && f) {
        f[1] = BPTR(f + 24);
        f[2] = BPTR(f + 4);
        f[4] = 16;
        f[4 + 16] = 0x4d494700;
        *(B *)(f + 24) = 9;
        memcpy((B *)(f + 24) + 1, "container", 9);
        for (n = *(B **)(x + 74); *(B **)n && i < 4; n = *(B **)n)
            if ((dn = *(B **)(n + 16)) != 0 && !*(U *)(dn + 28)) {
                *(U *)(dn + 28) = BPTR(f);
                s->startless[i++] = dn;
            }
    }
    if (!on)
        for (i = 0; i < 4; i++)
            if (s->startless[i]) *(U *)(s->startless[i] + 28) = 0;
    Permit();
    CloseLibrary(x);
}

/* an alert, drawn by the host over whatever shows; nonzero for the left button */
U early_alert(U number, const B *str, U height, U frames)
{
    struct mig_early_alert *a = (struct mig_early_alert *)MIG_EARLY_ALERT;
    U n = 0, seq;
    do {
        for (str += 3; *str; str++)
            if (n < sizeof a->text - 2) a->text[n++] = *str;
        a->text[n++] = '\n';
    } while (*++str && n < sizeof a->text - 2);
    a->text[n] = 0;
    a->number = number; a->height = height; a->frames = frames;
    a->click = 0;
    seq = a->done + 1;
    a->seq = seq;
    *(volatile U *)MIG_FS_RING = 2;
    while (a->done != seq)
        ;
    return a->answer;
}

/* intuition's DisplayAlert and TimedDisplayAlert to the host */
void early_alerts(void)
{
    B *in = OpenLibrary("intuition.library", 39);
    U r[14];
    if (!in) return;
    Forbid();
    memset(r, 0, sizeof r);
    r[A1] = P(in); r[A0] = -90; r[D0] = P(alert_stub);
    lcall(EXEC, -420, r);
    r[A1] = P(in); r[A0] = -822; r[D0] = P(talert_stub);
    lcall(EXEC, -420, r);
    Permit();
    CloseLibrary(in);
}

/* the session's input bridge as a task, so input works before DOS */
static void bridge(struct state *s)
{
    U size, seg, r[14];
    const B *f = pfile(s, "c/container-input", &size);
    B *t, *stack;
    if (!f || !(seg = loadseg(f, size))) return;
    t = AllocMem(92);
    stack = AllocMem(4096);
    if (!t || !stack) {
        if (t) FreeMem(t, 92);
        if (stack) FreeMem(stack, 4096);
        unloadseg(seg);
        return;
    }
    t[8] = 1; t[9] = 10;
    *(const char **)(t + 10) = "container-input";
    *(B **)(t + 58) = stack;
    *(B **)(t + 62) = stack + 4096;
    *(B **)(t + 54) = stack + 4096;
    *(B **)(t + 74) = t + 78;
    *(B **)(t + 82) = t + 74;
    memset(r, 0, sizeof r);
    r[A1] = P(t); r[A2] = P(hunk(seg, 0));
    lcall(EXEC, -282, r);
}

static void unfake(struct state *s)
{
    Forbid();
    if (s->dos) ex(-252, 0, 0, 0, P(s->dos));
    if (s->icon) ex(-252, 0, 0, 0, P(s->icon));
    Permit();
}

/* the board as the monitor driver binds it; nonzero when bound */
static int bind(struct state *s)
{
    U size, n = 0, r[14], tags[4];
    const B *tt = pfile(s, MIG_EARLY_TOOLTYPES, &size), *e;
    const B *env, *p;
    char **list;
    B *rtg, *board, *h2;
    U seg = 0, fn;
    if (!tt) return 0;
    for (p = tt, e = tt + size; p < e; p++) n += !*p;
    if (!(list = (char **)AllocMem((n + 1) * 4))) return 0;
    for (n = 0, p = tt; p < e; p += slen((const char *)p) + 1) list[n++] = (char *)p;
    list[n] = 0;
    status()->step = 1;
    if (!initlib(s, "libs/iffparse.library", 0, 0)) return 0;
    status()->step = 2;
    initlib(s, "libs/picasso96/fastlayers.library", "Picasso96/fastlayers.library", 0);
    status()->step = 3;
    if (!(s->rtg = initlib(s, "libs/picasso96/rtg.library", "Picasso96/rtg.library", &seg)))
        return 0;
    status()->step = 4;
    /* what P96's ENV handler would read first: the native blitter is not there */
    env = pfile(s, "prefs/env-archive/picasso96/disableamigablitter", &size);
    h2 = hunk(seg, 2);
    if (env && h2) {
        fn = *(U *)(h2 + RTG_ENVREADERS + 4 * RTG_DISABLEBLITTER);
        memset(r, 0, sizeof r);
        r[A0] = P(env); r[D0] = size;
        xcall(EXEC, (void *)fn, r);
    }
    if (!(rtg = OpenLibrary("Picasso96/rtg.library", 40))) return 0;
    tags[0] = 0x8000415c; tags[1] = P(list); tags[2] = 0; tags[3] = 0;
    memset(r, 0, sizeof r);
    r[A0] = P("Container"); r[A1] = P(tags);
    status()->step = 5;
    board = (B *)lcall(rtg, -30, r);
    status()->step = 6;
    if (board) {
        memset(r, 0, sizeof r);
        r[A0] = P(board);
        board = (B *)lcall(rtg, -36, r);
    }
    status()->step = 7;
    CloseLibrary(rtg);
    return board != 0;
}

/* for tests: Workbench and a screen in front of it before DOS, shown for two seconds */
static void twoscreens(struct state *s)
{
    B *in = OpenLibrary("intuition.library", 39), *gfx = OpenLibrary("graphics.library", 39), *sc = 0;
    U r[14], tags[7], i;
    if (in && gfx) {
        memset(r, 0, sizeof r);
        lcall(in, -210, r);
        tags[0] = SA_DisplayID; tags[1] = s->boardid;
        tags[2] = SA_Depth; tags[3] = s->depth;
        tags[4] = SA_Title; tags[5] = P("container");
        tags[6] = 0;
        memset(r, 0, sizeof r);
        r[A1] = P(tags);
        sc = (B *)lcall(in, -612, r);
    }
    for (i = 0; sc && i < 100; i++) {
        memset(r, 0, sizeof r);
        lcall(gfx, -270, r);
    }
    if (sc) {
        memset(r, 0, sizeof r);
        r[A0] = P(sc);
        lcall(in, -66, r);
    }
    if (gfx) CloseLibrary(gfx);
    if (in) CloseLibrary(in);
}

void early_init(void)
{
    struct mig_early *d = (struct mig_early *)MIG_EARLY_BASE;
    struct mig_input *in = (struct mig_input *)MIG_INPUT_BASE;
    const B *sm;
    B *intuition;
    struct state *s;
    U r[14], menu;
    early_alerts();
    if (d->magic != MIG_EARLY_MAGIC || !(sm = dfile(d, MIG_EARLY_SCREENMODE, 0)))
        return;
    status()->status = MIG_EARLY_STARTED;
    menu = in->magic == MIG_INPUT_MAGIC && ((in->menu & 4) || (in->menu & 3) == 3);
    if (!(s = (struct state *)AllocMem(sizeof *s))) return;
    s->type = 8;
    s->name = "container.early";
    s->dir = d;
    s->fssm = (U *)AllocMem(4 * 32);
    s->boardid = *(U *)sm;
    s->sm = sm;
    s->depth = *(W *)(sm + 8) ? *(W *)(sm + 8) : 8;
    s->dos = fake(s, "dos.library", 0);
    s->icon = fake(s, "icon.library", 1);
    if (!s->dos || !s->icon) {
        unfake(s);
        return;
    }
    bridge(s);
    if (!bind(s)) {
        unfake(s);
        status()->status = s->rtg ? MIG_EARLY_NOBOARD : MIG_EARLY_NOLIB;
        if (s->rtg) ex(-486, 0, 0, 0, P(s));
        return;
    }
    s->bound = 1;
    intuition = OpenLibrary("intuition.library", 39);
    if (intuition) {
        /* Workbench in the board's mode, as IPrefs would set it */
        memset(r, 0, sizeof r);
        r[A0] = P(sm); r[D0] = 12; r[D1] = 1;
        lcall(intuition, -576, r);
        s->tramp[0] = 0x2f3c;
        *(U *)&s->tramp[1] = P(s);
        s->tramp[3] = 0x4ef9;
        *(U *)&s->tramp[4] = P(osl_stub);
        memcpy(s->tramp2, s->tramp, sizeof s->tramp2);
        *(U *)&s->tramp2[4] = P(os_stub);
        memcpy(s->tramp3, s->tramp, sizeof s->tramp3);
        *(U *)&s->tramp3[4] = P(sp_stub);
        ex(-636, 0, 0, 0, 0);
        Forbid();
        memset(r, 0, sizeof r);
        r[A1] = P(intuition); r[A0] = -612; r[D0] = P(s->tramp);
        s->oldosl = (void *)lcall(EXEC, -420, r);
        /* OpenScreen as OpenScreenTagList without tags */
        r[A1] = P(intuition); r[A0] = -198; r[D0] = P(s->tramp2);
        lcall(EXEC, -420, r);
        r[A1] = P(intuition); r[A0] = -324; r[D0] = P(s->tramp3);
        s->oldsetprefs = (void *)lcall(EXEC, -420, r);
        Permit();
    }
    status()->step = 8;
    /* P96 has its own, which do not show on the board */
    early_alerts();
    unfake(s);
    ex(-486, 0, 0, 0, P(s));
    if (menu) {
        s->menu = 1;
        startups(s, 1);
        buttons(s, 1);
    }
    if (dfile(d, MIG_EARLY_SCREENS, 0))
        twoscreens(s);
    status()->status = MIG_EARLY_BOUND;
}

/* after the boot menu: the buttons as they are */
void early_menu(void)
{
    struct state *s = (struct state *)ex(-498, 0, 0, 0, P("container.early"));
    if (s && s->menu) {
        s->menu = 0;
        buttons(s, 0);
        startups(s, 0);
    }
}

/*
 * With DOS: start the processes asked for before it.  Nonzero when the
 * board is already bound.
 */
U early_late(B *dos)
{
    struct state *s = (struct state *)ex(-498, 0, 0, 0, P("container.early"));
    U i, r[14], p;
    if (!s) return 0;
    for (i = 0; i < s->nprocs; i++) {
        memset(r, 0, sizeof r);
        r[D1] = s->procs[i];
        p = lcall(dos, -498, r);
        if (!i && s->rtg) *(U *)(s->rtg + RTG_HANDLER) = p;
    }
    s->nprocs = 0;
    return s->bound;
}
