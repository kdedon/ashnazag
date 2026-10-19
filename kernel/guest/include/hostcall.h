/*
 * hostcall.h -- host calls: patched guest code calling into its launcher.
 *
 * A patch is `jsr` (or `jmp`, from a routine's entry) to HC_STUB(n).  The
 * stub saves the guest's registers on its stack, moves to a host stack
 * and calls hostcall(r) with them:
 *
 *	r[0..7] d0-d7, r[8..14] a0-a6, r[15] the stub's return (HC_CALL),
 *	r[16] the patch's return, r[17..] the guest's stack before the patch
 *
 * Registers the service changes in r[] go back to the guest.  The module
 * holds interrupts while the pc is in the launcher's code.  HC_ENTRY goes
 * in one file, at file scope.
 */

#define	HC_NSTUB	64
#define	HC_NSTACK	2048	/* longs */
#define	HC_STUB(n)	((unsigned long)hc_stubs + 6 * (n))
/* a 16-bit divide: a 32-bit one by a constant is a 64-bit multiply, trapped by the 68060 */
#define	HC_CALL(r)	((int)((unsigned short)((r)[15] - (unsigned long)hc_stubs) / 6 - 1))

extern char hc_stubs[];
extern unsigned long *hc_sp, hc_stack[];

/* the asm sits in a function that is never called, so gcc's section stays right */
#define	HC_ENTRY \
unsigned long hc_stack[HC_NSTACK], *hc_sp = hc_stack + HC_NSTACK; \
void hc_entry() { __asm__("\n\
	.text\n\
	.even\n\
	.globl hc_stubs\n\
hc_stubs:\n\
	.rept 64\n\
	jsr hc_common\n\
	.endr\n\
hc_common:\n\
	movem.l %d0-%d7/%a0-%a6,-(%sp)\n\
	move.l %sp,%a0\n\
	move.l hc_sp,%sp\n\
	move.l %a0,-(%sp)\n\
	jsr hostcall\n\
	move.l (%sp),%sp\n\
	movem.l (%sp)+,%d0-%d7/%a0-%a6\n\
	addq.l &4,%sp\n\
	rts\n"); }
