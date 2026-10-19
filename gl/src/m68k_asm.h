/* m68k_asm.h -- vertex routines in assembly and their C equivalents */

typedef unsigned long (*m68k_xfcl3_fn)(const float *, const float *, int,
    float (*)[4], float (*)[4], unsigned char *, int);

unsigned long m68k_xfcl3(const float m[16], const float *from, int stride,
    float (*clip)[4], float (*ndc)[4], unsigned char *mask, int count);
unsigned long m68k_xfcl3_c(const float m[16], const float *from, int stride,
    float (*clip)[4], float (*ndc)[4], unsigned char *mask, int count);

extern int m68k_cpu;
extern m68k_xfcl3_fn m68k_xfcl3_p;
void m68k_asm_init(void);

/* A display-list cassette packed for replay: the primitives that end in it
 * as fans of nv vertices.  A fan is its provoking vertex then its vertices
 * in drawing order, as byte offsets of 5-word records (m68k_lxf).
 */
struct m68k_run {
	unsigned short nv, nfan;
};

/* run nv flag: nfan strips of nv vertices, drawn by m68k_lstrip */
#define M68K_STRIP	0x8000

struct m68k_cas {
	int nv, nn, nrun;
	float (*pos)[3];
	unsigned short *vn;		/* vertex -> normal */
	float (*nrm)[3], (*unit)[3];	/* as given, normalised */
	struct m68k_run *run;
	unsigned short *fan;
	struct m68k_cas *head;		/* first cassette of the chain */
	float box[6];			/* head: chain's min and max x, y, z */
	int ok;				/* head: the chain can be replayed */
	int cont;			/* starts inside a primitive */
	int open;			/* ends inside one: its mode + 1 */
	int has_last;
	float last[3];			/* last normal given */
	unsigned long *col;		/* per normal: lit colour, R G B A bytes */
	unsigned long *rec;		/* per vertex: fifo record */
};

/* vertex records for the fifo: x, y, colour, z, 1/w, halves swapped */
typedef void (*m68k_lxf_fn)(const float m[16], const float (*pos)[3], int n,
    unsigned long *out, const unsigned long *col, const unsigned short *vn);
typedef unsigned long *(*m68k_lfan_fn)(unsigned long *f, const unsigned short *fan,
    int nfan, int nv, unsigned long hdr, const unsigned long *vtx, int words, int flat);

void m68k_lxf(const float m[16], const float (*pos)[3], int n,
    unsigned long *out, const unsigned long *col, const unsigned short *vn);
void m68k_lxf_c(const float m[16], const float (*pos)[3], int n,
    unsigned long *out, const unsigned long *col, const unsigned short *vn);
unsigned long *m68k_lfan(unsigned long *f, const unsigned short *fan, int nfan,
    int nv, unsigned long hdr, const unsigned long *vtx, int words, int flat);
unsigned long *m68k_lfan_c(unsigned long *f, const unsigned short *fan, int nfan,
    int nv, unsigned long hdr, const unsigned long *vtx, int words, int flat);
unsigned long *m68k_lstrip(unsigned long *f, const unsigned short *strip,
    int first, int n, unsigned long hdr, const unsigned long *vtx, int words,
    int flat);
void m68k_lxf40(const float m[16], const float (*pos)[3], int n,
    unsigned long *out, const unsigned long *col, const unsigned short *vn);
void m68k_lxf60(const float m[16], const float (*pos)[3], int n,
    unsigned long *out, const unsigned long *col, const unsigned short *vn);
extern m68k_lxf_fn m68k_lxf_p;

/* one infinite light, no specular: light direction, diffuse and base
 * colours; base and alpha as bytes */
struct m68k_lt {
	float vp[3], diff[3], base[3];
	unsigned long basecol, alpha;
};

typedef void (*m68k_light_fn)(const float (*n)[3], int nn,
    const struct m68k_lt *l, unsigned long *col);
void m68k_light(const float (*n)[3], int nn, const struct m68k_lt *l,
    unsigned long *col);
void m68k_light_c(const float (*n)[3], int nn, const struct m68k_lt *l,
    unsigned long *col);
extern m68k_light_fn m68k_light_p;
extern m68k_lfan_fn m68k_lfan_p;
extern void (*m68k_cas_built)(struct m68k_cas *);
