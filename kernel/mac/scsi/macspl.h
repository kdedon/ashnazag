/*
 * Priority levels.  The sys/inline.h spl functions use "asm volatile",
 * which -traditional compiles as plain asm and drops when the result is
 * unused; these use __volatile__, with a "memory" clobber so shared
 * state is not cached across a level change.
 */

#define SR_GET(s)	__asm__ __volatile__("movew %%sr,%0" : "=d" (s) : : "memory")
#define SR_SET(s)	__asm__ __volatile__("movew %0,%%sr" : : "d" (s) : "memory")

/* raise to the SCSI interrupt level (2), never lower; returns the old SR */
static int
splscsi()
{
	int s, n;

	SR_GET(s);
	if ((s & 0x700) < 0x200) {
		n = (s & ~0x700) | 0x200;
		SR_SET(n);
	}
	return s;
}

static void
splrestore(s)
int s;
{
	SR_SET(s);
}
