/* osbind.h -- the GEMDOS, BIOS and XBIOS calls fVDI makes, for a bare gcc. */
#ifndef _OSBIND_H
#define _OSBIND_H
#include <sys/types.h>


typedef struct {
	char		dta_buf[21];
	char		dta_attribute;
	unsigned short	dta_time;
	unsigned short	dta_date;
	long		dta_size;
	char		dta_name[14];
} _DTA;

#define	_TCLOB	"d1", "d2", "a0", "a1", "a2", "memory", "cc"

#define	_t_w(t, f) ({ register long _r __asm__("d0"); \
	__asm__ __volatile__("movew %1,%%sp@-\n\ttrap #" #t "\n\taddql #2,%%sp" \
	: "=r"(_r) : "n"(f) : _TCLOB); _r; })
#define	_t_ww(t, f, a) ({ register long _r __asm__("d0"); short _a = (short)(a); \
	__asm__ __volatile__("movew %2,%%sp@-\n\tmovew %1,%%sp@-\n\ttrap #" #t "\n\taddql #4,%%sp" \
	: "=r"(_r) : "n"(f), "r"(_a) : _TCLOB); _r; })
#define	_t_wl(t, f, a) ({ register long _r __asm__("d0"); long _a = (long)(a); \
	__asm__ __volatile__("movel %2,%%sp@-\n\tmovew %1,%%sp@-\n\ttrap #" #t "\n\taddql #6,%%sp" \
	: "=r"(_r) : "n"(f), "r"(_a) : _TCLOB); _r; })
#define	_t_www(t, f, a, b) ({ register long _r __asm__("d0"); short _a = (short)(a), _b = (short)(b); \
	__asm__ __volatile__("movew %3,%%sp@-\n\tmovew %2,%%sp@-\n\tmovew %1,%%sp@-\n\ttrap #" #t "\n\taddql #6,%%sp" \
	: "=r"(_r) : "n"(f), "r"(_a), "r"(_b) : _TCLOB); _r; })
#define	_t_wwl(t, f, a, b) ({ register long _r __asm__("d0"); short _a = (short)(a); long _b = (long)(b); \
	__asm__ __volatile__("movel %3,%%sp@-\n\tmovew %2,%%sp@-\n\tmovew %1,%%sp@-\n\ttrap #" #t "\n\taddql #8,%%sp" \
	: "=r"(_r) : "n"(f), "r"(_a), "r"(_b) : _TCLOB); _r; })
#define	_t_wlw(t, f, a, b) ({ register long _r __asm__("d0"); long _a = (long)(a); short _b = (short)(b); \
	__asm__ __volatile__("movew %3,%%sp@-\n\tmovel %2,%%sp@-\n\tmovew %1,%%sp@-\n\ttrap #" #t "\n\taddql #8,%%sp" \
	: "=r"(_r) : "n"(f), "r"(_a), "r"(_b) : _TCLOB); _r; })
#define	_t_wlww(t, f, a, b, c) ({ register long _r __asm__("d0"); long _a = (long)(a); short _b = (short)(b), _c = (short)(c); \
	__asm__ __volatile__("movew %4,%%sp@-\n\tmovew %3,%%sp@-\n\tmovel %2,%%sp@-\n\tmovew %1,%%sp@-\n\ttrap #" #t "\n\tlea %%sp@(10),%%sp" \
	: "=r"(_r) : "n"(f), "r"(_a), "r"(_b), "r"(_c) : _TCLOB); _r; })
#define	_t_wwll(t, f, a, b, c) ({ register long _r __asm__("d0"); short _a = (short)(a); long _b = (long)(b), _c = (long)(c); \
	__asm__ __volatile__("movel %4,%%sp@-\n\tmovel %3,%%sp@-\n\tmovew %2,%%sp@-\n\tmovew %1,%%sp@-\n\ttrap #" #t "\n\tlea %%sp@(12),%%sp" \
	: "=r"(_r) : "n"(f), "r"(_a), "r"(_b), "r"(_c) : _TCLOB); _r; })

#define	Cconout(c)		_t_ww(1, 2, c)
#define	Cconws(s)		_t_wl(1, 9, s)
#define	Super(s)		_t_wl(1, 0x20, s)
#define	Fsetdta(d)		_t_wl(1, 0x1a, d)
#define	Fgetdta()		((_DTA *)_t_w(1, 0x2f))
#define	Malloc(n)		_t_wl(1, 0x48, n)
#define	Mfree(p)		_t_wl(1, 0x49, p)
#define	Mxalloc(n, m)		_t_wlw(1, 0x44, n, m)
#define	Fcreate(f, a)		_t_wlw(1, 0x3c, f, a)
#define	Fopen(f, m)		_t_wlw(1, 0x3d, f, m)
#define	Fclose(h)		_t_ww(1, 0x3e, h)
#define	Fread(h, n, b)		_t_wwll(1, 0x3f, h, n, b)
#define	Fwrite(h, n, b)		_t_wwll(1, 0x40, h, n, b)
#define	Fseek(o, h, w)		_t_wlww(1, 0x42, o, h, w)
#define	Fsfirst(f, a)		_t_wlw(1, 0x4e, f, a)
#define	Fsnext()		_t_w(1, 0x4f)
#define	Bconout(d, c)		_t_www(13, 3, d, c)
#define	Setexc(v, a)		_t_wwl(13, 5, v, a)
#define	Kbshift(m)		_t_ww(13, 11, m)
#define	Physbase()		_t_w(14, 2)
#define	Logbase()		_t_w(14, 3)
#define	Getrez()		_t_w(14, 4)
#define	Supexec(f)		_t_wl(14, 38, f)
#define	Initmouse(t, p, v)	_t_wwll(14, 0, t, p, v)


#define	__CLOBBER_RETURN(a)
#define	AND_MEMORY	, "memory"
#define	FA_RDONLY	0x01
#define	FA_HIDDEN	0x02
#define	FA_SYSTEM	0x04
#define	FA_LABEL	0x08
#define	FA_DIR		0x10
#define	FA_CHANGED	0x20

#endif
