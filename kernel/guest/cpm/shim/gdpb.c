/* The disk parameter block and allocation vector a guest reads, derived
 * from the one our BDOS keeps.  Placement is each shim's business, since
 * the address spaces differ; the bytes are not. */

#include "gdpb.h"

/*
 * gdpbpack -- our drive as the seventeen bytes a CP/M guest expects:
 * SPT, BSH, BLM, EXM, DSM, DRM, AL0, AL1, CKS, OFF, PSH, PHM, words low
 * byte first because both guests are little-endian machines and the
 * Z8001 this runs on is not.  A CP/M 2.2 guest reads the first fifteen
 * and stops, which is why there is one block here and not two.
 *
 * Three of the twelve fields need a decision; the rest pass through.
 *
 * AL0 and AL1 are one word here (dir_al), and its bits run from block 0
 * downwards from the top, so the byte order is the word's HIGH byte
 * first.  Our 0xF000 is the four directory blocks src/bios/bios900.c
 * reserves, and the same four seldsk() computes from DRM and BLM when it
 * logs a drive in (src/bdos/fileio.c).
 *
 * PSH and PHM describe the physical sector the BDOS would have to
 * deblock to, and ours are zero because there is nothing above the BIOS
 * to deblock: its read and write entries take one 128-byte record
 * (src/bios/bios900.c), and the 512-byte blocks underneath them are its
 * own affair and never reach a DPB.
 *
 * CKS passes through at our BIOS's zero -- a fixed disk whose directory
 * is never checksummed -- rather than being invented as DRM/4.
 */
int gdpbpack(d, p)
struct gdpb *d;
char *p;
{
	p[0] = (char)(d->spt & 0xff);
	p[1] = (char)((d->spt >> 8) & 0xff);
	p[2] = (char)d->bsh;
	p[3] = (char)d->blm;
	p[4] = (char)d->exm;
	p[5] = (char)(d->dsm & 0xff);
	p[6] = (char)((d->dsm >> 8) & 0xff);
	p[7] = (char)(d->drm & 0xff);
	p[8] = (char)((d->drm >> 8) & 0xff);
	p[9] = (char)((d->dir_al >> 8) & 0xff);		/* AL0	*/
	p[10] = (char)(d->dir_al & 0xff);		/* AL1	*/
	p[11] = (char)(d->cks & 0xff);
	p[12] = (char)((d->cks >> 8) & 0xff);
	p[13] = (char)(d->trk_off & 0xff);
	p[14] = (char)((d->trk_off >> 8) & 0xff);
	p[15] = 0;					/* PSH	*/
	p[16] = 0;					/* PHM	*/
	return (GDPB_LEN);
}

/*
 * gdpbalv -- the length of this drive's allocation vector.
 *
 * One bit per block with block 0 in the TOP bit of the first byte, which
 * is the order src/bdos/dskutil.c setaloc() uses, so the vector the BDOS
 * keeps is the vector a guest expects and not a mirror of it.  DSM is
 * the highest block number, so the count is dsm+1 bits rounded up to a
 * byte -- 320 bytes for drive A: and 8 KB for the largest drive the BIOS
 * will admit.
 */
long gdpbalv(d)
struct gdpb *d;
{
	return ((long)(d->dsm >> 3) + 1L);
}
