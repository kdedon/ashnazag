/*
 * gsx.h - GSX's parameter block, as the shims hand it to a device.
 * The arrays are 0-based, so contrl[0] is the opcode.
 */

struct gsxpb {
	int	*contrl;
	int	*intin;
	int	*ptsin;
	int	*intout;
	int	*ptsout;
};

#define	GSX_OK		0
#define	GSX_NA		(-1)	/* opcode, escape or GDP not available	*/

#define	GSXMAXV		256	/* ptsin vertices			*/
