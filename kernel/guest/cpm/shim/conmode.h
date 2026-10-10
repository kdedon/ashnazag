/* The console mode BDOS function 109 gets and sets, as both guest shims
 * use it.  The bits are the BDOS's own, named here so the two seams ask
 * for the same thing. */

#define CM_NOSTOP	0x0002	/* ^S/^Q stop-scroll disabled		*/
