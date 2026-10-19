/*
 * m68k_asm.c -- pick the vertex routines for the CPU, as Mesa picks its
 * x86 ones: the assembly where the CPU is known to have an FPU that runs
 * it at speed, the C otherwise.  MESA_M68K=030, 040, 060 or c overrides.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/systeminfo.h>
#include "m68k_asm.h"

/* Mesa's clip bits */
#define RIGHT	0x01
#define LEFT	0x02
#define TOP	0x04
#define BOTTOM	0x08
#define NEAR	0x10
#define FAR	0x20

unsigned long
m68k_xfcl3_c(const float m[16], const float *from, int stride, float (*clip)[4],
    float (*ndc)[4], unsigned char *mask, int count)
{
	unsigned char or = 0, and = 0x3f, k;
	unsigned long c = 0;
	int i;

	for (i = 0; i < count; i++, from = (const float *)((const char *)from + stride)) {
		const float ox = from[0], oy = from[1], oz = from[2];
		const float cx = m[0] * ox + m[4] * oy + m[8] * oz + m[12];
		const float cy = m[1] * ox + m[5] * oy + m[9] * oz + m[13];
		const float cz = m[2] * ox + m[6] * oy + m[10] * oz + m[14];
		const float cw = m[3] * ox + m[7] * oy + m[11] * oz + m[15];
		const float nw = -cw;

		clip[i][0] = cx;
		clip[i][1] = cy;
		clip[i][2] = cz;
		clip[i][3] = cw;
		k = 0;
		if (cx > cw) k |= RIGHT;
		if (cx < nw) k |= LEFT;
		if (cy > cw) k |= TOP;
		if (cy < nw) k |= BOTTOM;
		if (cz > cw) k |= FAR;
		if (cz < nw) k |= NEAR;
		mask[i] = k;
		if (k) {
			c++;
			and &= k;
			or |= k;
			ndc[i][0] = 0;
			ndc[i][1] = 0;
			ndc[i][2] = 0;
			ndc[i][3] = 1;
		} else {
			const float oow = 1.0F / cw;
			ndc[i][0] = cx * oow;
			ndc[i][1] = cy * oow;
			ndc[i][2] = cz * oow;
			ndc[i][3] = oow;
		}
	}
	return or | and << 8 | c << 16;
}

static unsigned long
swapf(float f)
{
	union { float f; unsigned long u; } v;

	v.f = f;
	return v.u << 16 | v.u >> 16;
}

void
m68k_lxf_c(const float m[16], const float (*pos)[3], int n, unsigned long *out,
    const unsigned long *col, const unsigned short *vn)
{
	int i;

	for (i = 0; i < n; i++, out += 5) {
		const float ox = pos[i][0], oy = pos[i][1], oz = pos[i][2];
		const float cx = m[0] * ox + m[4] * oy + m[8] * oz + m[12];
		const float cy = m[1] * ox + m[5] * oy + m[9] * oz + m[13];
		const float cz = m[2] * ox + m[6] * oy + m[10] * oz + m[14];
		const float cw = m[3] * ox + m[7] * oy + m[11] * oz + m[15];
		const float oow = 1.0F / cw;

		out[0] = swapf(cx * oow);
		out[1] = swapf(cy * oow);
		out[2] = col[vn[i]];
		out[3] = swapf(cz * oow);
		out[4] = swapf(oow);
	}
}

unsigned long *
m68k_lfan_c(unsigned long *f, const unsigned short *fan, int nfan, int nv,
    unsigned long hdr, const unsigned long *vtx, int words, int flat)
{
	const unsigned long *p;
	unsigned long c;
	int k;

	unsigned long *cc = (unsigned long *)flat;

	while (nfan-- > 0) {
		c = ((const unsigned long *)((const char *)vtx + *fan++))[2];
		if (words == 3 && c != cc[1]) {
			*f++ = cc[0];
			*f++ = c;
			cc[1] = c;
		}
		*f++ = hdr;
		for (k = 0; k < nv; k++) {
			p = (const unsigned long *)((const char *)vtx + *fan++);
			f[0] = p[0];
			f[1] = p[1];
			if (words == 3) {
				f[2] = p[3];
				f += 3;
				continue;
			}
			f[2] = flat ? c : p[2];
			f[3] = p[3];
			if (words == 5)
				f[4] = p[4];
			f += words;
		}
	}
	return f;
}

/* vertices first..first+n-1 of a strip (its entries as a fan's) as one
 * packet, coloured as m68k_lfan colours a fan
 */
unsigned long *
m68k_lstrip(unsigned long *f, const unsigned short *strip, int first, int n,
    unsigned long hdr, const unsigned long *vtx, int words, int flat)
{
	const unsigned long *p;
	unsigned long c, *cc = (unsigned long *)flat;

	c = ((const unsigned long *)((const char *)vtx + *strip))[2];
	if (words == 3 && c != cc[1]) {
		*f++ = cc[0];
		*f++ = c;
		cc[1] = c;
	}
	*f++ = hdr;
	for (strip += first + 1; n-- > 0; f += words) {
		p = (const unsigned long *)((const char *)vtx + *strip++);
		f[0] = p[0];
		f[1] = p[1];
		if (words == 3) {
			f[2] = p[3];
			continue;
		}
		f[2] = flat ? c : p[2];
		f[3] = p[3];
		if (words == 5)
			f[4] = p[4];
	}
	return f;
}

/* Mesa's float to byte, for 0..1 with clamping */
static unsigned long
ubyte(float f)
{
	union { float r; unsigned long i; } t;

	t.r = f;
	if (t.i >= 0x3f7f0000)
		return (long)t.i < 0 ? 0 : 255;
	t.r = t.r * (255.0F / 256.0F) + 32768.0F;
	return t.i & 0xff;
}

void
m68k_light_c(const float (*n)[3], int nn, const struct m68k_lt *l,
    unsigned long *col)
{
	int i;

	for (i = 0; i < nn; i++) {
		const float d = n[i][0] * l->vp[0] + n[i][1] * l->vp[1] +
		    n[i][2] * l->vp[2];

		if (d < 0.0F)
			col[i] = l->basecol;
		else
			col[i] = ubyte(l->base[0] + d * l->diff[0]) << 24 |
			    ubyte(l->base[1] + d * l->diff[1]) << 16 |
			    ubyte(l->base[2] + d * l->diff[2]) << 8 | l->alpha;
	}
}

int m68k_cpu;
m68k_light_fn m68k_light_p = m68k_light_c;
m68k_xfcl3_fn m68k_xfcl3_p = m68k_xfcl3_c;
m68k_lxf_fn m68k_lxf_p = m68k_lxf_c;
m68k_lfan_fn m68k_lfan_p = m68k_lfan_c;
void (*m68k_cas_built)(struct m68k_cas *);

/* 30, 40, 60, or 0 for unknown or no FPU */
static int
cpu(void)
{
	char b[32], *e = getenv("MESA_M68K");

	if (e != NULL)
		return atoi(e);
	if (sysinfo(SI_ARCHITECTURE, b, sizeof b) > 0) {
		if (strcmp(b, "m68060") == 0)
			return 60;
		if (strcmp(b, "m68040") == 0)
			return 40;
		if (strcmp(b, "m68030") == 0)
			return 30;
	}
	return 0;
}

void
m68k_asm_init(void)
{
	m68k_cpu = cpu();
	switch (m68k_cpu) {
	case 30:
	case 40:
	case 60:
		m68k_xfcl3_p = m68k_xfcl3;
		m68k_lxf_p = m68k_cpu == 60 ? m68k_lxf60 :
		    m68k_cpu == 40 ? m68k_lxf40 : m68k_lxf;
		m68k_lfan_p = m68k_lfan;
		m68k_light_p = m68k_light;
		break;
	default:
		m68k_xfcl3_p = m68k_xfcl3_c;
		m68k_lxf_p = m68k_lxf_c;
		m68k_lfan_p = m68k_lfan_c;
		m68k_light_p = m68k_light_c;
	}
}
