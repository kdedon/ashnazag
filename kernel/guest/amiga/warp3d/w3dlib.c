/*
 * Warp3D.library: library housekeeping, and the BitMap a call names
 * looked up through Picasso96API for the host side.
 */
typedef unsigned long U;
typedef long L;
typedef unsigned char B;
typedef unsigned short W;

struct base {
	B lib[36];
	B sem[48];
	void *seg;
	void *p96;
};

extern L libcall(void *, L, L, L, L, L);
extern void linit(void);
extern L vectors[];
static void *execbase(void) { void *b; __asm__ __volatile__("move.l 4.w,%0" : "=a"(b)); return b; }
#define EXEC execbase()
#define EX(lvo, d0, d1, a0, a1) libcall(EXEC, lvo, (L)(d0), (L)(d1), (L)(a0), (L)(a1))
#define TAB ((volatile U *)0x25000000)
#define MAGIC 0x57334448U
#define CC_BITMAP 0x80200000U
#define CC_W3DBM 0x80200003U

const char w3d_name[] = "Warp3D.library";
const char w3d_idstring[] = "Warp3D.library 4.2 (11.10.2026) Mesa";
const L w3d_autoinit[4] = { sizeof(struct base), (L)vectors, 0, (L)linit };
static const char p96name[] = "Picasso96API.library";

void *w3d_init(B *lib, void *seg)
{
	struct base *b = (struct base *)lib;
	B *p;
	U i;
	for (p = lib + 34; p < (B *)(b + 1); p++) *p = 0;
	lib[8] = 9;
	*(const char **)(lib + 10) = w3d_name;
	lib[14] = 6;
	*(W *)(lib + 20) = 4;
	*(W *)(lib + 22) = 2;
	*(const char **)(lib + 24) = w3d_idstring;
	b->seg = seg;
	EX(-558, 0, 0, b->sem, 0);		/* InitSemaphore */
	/* without startmig's table there is no 3D */
	if (TAB[0] != MAGIC || TAB[1] != 1 || TAB[2] != 84) {
		i = *(W *)(lib + 16);
		EX(-210, i + *(W *)(lib + 18), 0, 0, lib - i);
		return 0;
	}
	return lib;
}

void *w3d_open(struct base *b)
{
	(*(W *)(b->lib + 32))++;
	b->lib[14] &= ~8;
	return b;
}

void *w3d_expunge(struct base *b)
{
	void *seg = b->seg;
	U neg = *(W *)(b->lib + 16);
	if (*(W *)(b->lib + 32)) {
		b->lib[14] |= 8;		/* LIBF_DELEXP */
		return 0;
	}
	if (b->p96) EX(-414, 0, 0, 0, b->p96);	/* CloseLibrary */
	EX(-252, 0, 0, 0, b);			/* Remove */
	EX(-210, neg + *(W *)(b->lib + 18), 0, 0, (B *)b - neg);
	return seg;
}

void *w3d_close(struct base *b)
{
	if (--*(W *)(b->lib + 32) == 0 && (b->lib[14] & 8))
		return w3d_expunge(b);
	return 0;
}

static U tag(U *t, U want)
{
	while (t) {
		if (t[0] == 0) return 0;
		if (t[0] == 2) { t = (U *)t[1]; continue; }
		if (t[0] == 3) { t += 2 * (t[1] + 1); continue; }
		if (t[0] == want) return t[1];
		t += 2;
	}
	return 0;
}

#define ATTR(a) ((U)libcall(b->p96, -42, a, 0, bm, 0))

/* regs: the caller's d0-d7/a0-a6 */
void w3d_pre(L n, struct base *b, U *regs)
{
	U bm = 0;
	if (n == 0 && !tag((U *)regs[9], CC_W3DBM))
		bm = tag((U *)regs[9], CC_BITMAP);
	else if (n == 27)
		bm = regs[9];
	TAB[5] = 0;
	/*
	 * opened here, not in the open vector, which runs under Forbid that a
	 * load from disk would break; without it only W3D_Bitmap contexts work
	 */
	if (bm && !b->p96)
		b->p96 = (void *)EX(-552, 0, 0, 0, p96name);
	if (!bm || !b->p96 || !ATTR(8))		/* P96BMA_ISP96 */
		return;
	TAB[6] = ATTR(3);			/* MEMORY */
	TAB[7] = ATTR(4);			/* BYTESPERROW */
	TAB[8] = ATTR(7);			/* RGBFORMAT */
	TAB[9] = ATTR(0);			/* WIDTH */
	TAB[10] = ATTR(1);			/* HEIGHT */
	TAB[5] = bm;
}
