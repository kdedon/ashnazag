/*
 * Virtual Ethernet stations: extra unicast addresses on the host's NIC
 * whose frames go to a handler instead of the host's streams.  Frames
 * between the host and its stations, and between stations, are switched
 * locally, since a NIC does not receive its own transmissions.
 *
 * K&R C.
 */

#ifndef _VSTATION_H
#define	_VSTATION_H

#define	VST_MINFRAME	14
#define	VST_MAXFRAME	1514		/* without CRC */
#define	VST_NMC		4		/* multicast addresses per station */

struct vst_ops {
	int	(*vs_attach)();		/* (mac, rx, arg): station >= 0, or -errno */
	void	(*vs_detach)();		/* (st) */
	int	(*vs_xmit)();		/* (st, frame, len): 0 or errno; copies */
	int	(*vs_mcast)();		/* (st, addr, on): 0 or errno */
	void	(*vs_hwaddr)();		/* (mac): the host's own address */
	int	(*vs_room)();		/* (): 1 if a frame can be sent now */
};

/*
 * rx(arg, frame, len) runs at the NIC's interrupt level; frame is valid
 * only during the call.
 */
extern struct vst_ops *vst_ops;		/* set by the NIC driver */

#endif
