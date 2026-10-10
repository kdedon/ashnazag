/* Our drive as a CP/M disk parameter block a guest can read.  Both guest
 * shims place the block inside their own guest's memory; the bytes are
 * derived here, once, so the two seams describe the same disk. */

#define GDPB_LEN	17	/* a CP/M 3 disk parameter block	*/

/*
 * src/bdos/bdosdef.h `struct dpb', byte for byte: what function 31
 * copies out to its caller.  `dpbdum' is a member and not padding to be
 * dropped -- the Z8001 word-aligns dsm -- and every word is 16 bits at
 * both ends, so this declaration names the same object the BDOS wrote.
 */
struct gdpb {
	unsigned short	spt;		/* 128-byte records per track	*/
	unsigned char	bsh;		/* block shift			*/
	unsigned char	blm;		/* block mask			*/
	unsigned char	exm;		/* extent mask			*/
	unsigned char	dpbdum;
	unsigned short	dsm;		/* highest block number		*/
	unsigned short	drm;		/* highest directory entry	*/
	unsigned short	dir_al;		/* directory allocation bits	*/
	unsigned short	cks;		/* checksummed directory records */
	unsigned short	trk_off;	/* reserved tracks		*/
};

extern int gdpbpack();		/* (struct gdpb *, char *) -> GDPB_LEN	*/
extern long gdpbalv();		/* (struct gdpb *) -> allocation bytes	*/
