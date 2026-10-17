/* Inline interrupt priority control.  splN() sets IPL N in the status
 * register and returns the old one; levels above 4 stop at 4. */
#ifndef _SYS_INLINE_H
#define _SYS_INLINE_H

static inline int _spl0() { int s; asm volatile("mov.w %%sr,%0;mov.w &0x2000,%%sr" : "=d"(s) :); return s; }
static inline int _spl1() { int s; asm volatile("mov.w %%sr,%0;mov.w &0x2100,%%sr" : "=d"(s) :); return s; }
static inline int _spl2() { int s; asm volatile("mov.w %%sr,%0;mov.w &0x2200,%%sr" : "=d"(s) :); return s; }
static inline int _spl3() { int s; asm volatile("mov.w %%sr,%0;mov.w &0x2300,%%sr" : "=d"(s) :); return s; }
static inline int _spl4() { int s; asm volatile("mov.w %%sr,%0;mov.w &0x2400,%%sr" : "=d"(s) :); return s; }

static inline int
splx(sr)
	unsigned short sr;
{
	int s;

	asm volatile("mov.w %%sr,%0;mov.w %1,%%sr" : "=&d"(s) : "d"(sr));
	return s;
}

static inline int
strlen(p)
	char *p;
{
	char *e = p;

	while (*e++)
		;
	return e - p - 1;
}

#define spl0		_spl0
#define spl1		_spl1
#define spl2		_spl2
#define spl3		_spl3
#define spl4		_spl4
#define spl5		_spl4
#define spl6		_spl4
#define spl7		_spl4
#define splhi		_spl4
#define spltty		_spl4
#define splimp		_spl4

#endif
