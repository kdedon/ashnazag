/* Data link provider interface: the connectionless primitives. */
#ifndef _SYS_DLPI_H
#define _SYS_DLPI_H

/* dl_primitive */
#define DL_INFO_REQ		0x00
#define DL_BIND_REQ		0x01
#define DL_UNBIND_REQ		0x02
#define DL_INFO_ACK		0x03
#define DL_BIND_ACK		0x04
#define DL_ERROR_ACK		0x05
#define DL_OK_ACK		0x06
#define DL_UNITDATA_REQ		0x07
#define DL_UNITDATA_IND		0x08
#define DL_UDERROR_IND		0x09
#define DL_UDQOS_REQ		0x0a
#define DL_ATTACH_REQ		0x0b
#define DL_DETACH_REQ		0x0c
#define DL_SUBS_BIND_REQ	0x1b
#define DL_SUBS_BIND_ACK	0x1c

/* dl_current_state */
#define DL_UNBOUND		0x00
#define DL_BIND_PENDING		0x01
#define DL_UNBIND_PENDING	0x02
#define DL_IDLE			0x03
#define DL_UNATTACHED		0x04
#define DL_ATTACH_PENDING	0x05
#define DL_DETACH_PENDING	0x06

/* dl_errno */
#define DL_BADSAP		0x00
#define DL_BADADDR		0x01
#define DL_ACCESS		0x02
#define DL_OUTSTATE		0x03
#define DL_SYSERR		0x04
#define DL_BADCORR		0x05
#define DL_BADDATA		0x06
#define DL_UNSUPPORTED		0x07
#define DL_BADPPA		0x08
#define DL_BADPRIM		0x09
#define DL_BOUND		0x0d
#define DL_INITFAILED		0x0e
#define DL_NOADDR		0x0f
#define DL_NOTINIT		0x10
#define DL_UNDELIVERABLE	0x11
#define DL_NOTSUPPORTED		0x12

/* dl_mac_type */
#define DL_CSMACD		0x0
#define DL_TPB			0x1
#define DL_TPR			0x2
#define DL_METRO		0x3
#define DL_ETHER		0x4

/* dl_service_mode */
#define DL_CODLS		0x01
#define DL_CLDLS		0x02

/* dl_provider_style */
#define DL_STYLE1		0x0500
#define DL_STYLE2		0x0501

typedef struct {
	ulong	dl_primitive;
} dl_info_req_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_max_sdu;
	ulong	dl_min_sdu;
	ulong	dl_addr_length;
	ulong	dl_mac_type;
	ulong	dl_reserved;
	ulong	dl_current_state;
	ulong	dl_max_idu;
	ulong	dl_service_mode;
	ulong	dl_qos_length;
	ulong	dl_qos_offset;
	ulong	dl_qos_range_length;
	ulong	dl_qos_range_offset;
	long	dl_provider_style;
	ulong	dl_addr_offset;
	ulong	dl_growth;
} dl_info_ack_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_ppa;
} dl_attach_req_t;

typedef struct {
	ulong	dl_primitive;
} dl_detach_req_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_sap;
	ulong	dl_max_conind;
	ushort	dl_service_mode;
	ushort	dl_conn_mgmt;
} dl_bind_req_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_sap;
	ulong	dl_addr_length;
	ulong	dl_addr_offset;
	ulong	dl_max_conind;
	ulong	dl_growth;
} dl_bind_ack_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_subs_sap_offset;
	ulong	dl_subs_sap_len;
} dl_subs_bind_req_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_subs_sap_offset;
	ulong	dl_subs_sap_len;
} dl_subs_bind_ack_t;

typedef struct {
	ulong	dl_primitive;
} dl_unbind_req_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_correct_primitive;
} dl_ok_ack_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_error_primitive;
	ulong	dl_errno;
	ulong	dl_unix_errno;
} dl_error_ack_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_dest_addr_length;
	ulong	dl_dest_addr_offset;
	ulong	dl_reserved[2];
} dl_unitdata_req_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_dest_addr_length;
	ulong	dl_dest_addr_offset;
	ulong	dl_src_addr_length;
	ulong	dl_src_addr_offset;
	ulong	dl_reserved;
} dl_unitdata_ind_t;

typedef struct {
	ulong	dl_primitive;
	ulong	dl_dest_addr_length;
	ulong	dl_dest_addr_offset;
	ulong	dl_reserved;
	ulong	dl_errno;
} dl_uderror_ind_t;

union DL_primitives {
	ulong			dl_primitive;
	dl_info_req_t		info_req;
	dl_info_ack_t		info_ack;
	dl_attach_req_t		attach_req;
	dl_detach_req_t		detach_req;
	dl_bind_req_t		bind_req;
	dl_bind_ack_t		bind_ack;
	dl_unbind_req_t		unbind_req;
	dl_subs_bind_req_t	subs_bind_req;
	dl_subs_bind_ack_t	subs_bind_ack;
	dl_ok_ack_t		ok_ack;
	dl_error_ack_t		error_ack;
	dl_unitdata_req_t	unitdata_req;
	dl_unitdata_ind_t	unitdata_ind;
	dl_uderror_ind_t	uderror_ind;
};

#define DL_INFO_REQ_SIZE	sizeof(dl_info_req_t)
#define DL_INFO_ACK_SIZE	sizeof(dl_info_ack_t)
#define DL_ATTACH_REQ_SIZE	sizeof(dl_attach_req_t)
#define DL_DETACH_REQ_SIZE	sizeof(dl_detach_req_t)
#define DL_BIND_REQ_SIZE	sizeof(dl_bind_req_t)
#define DL_BIND_ACK_SIZE	sizeof(dl_bind_ack_t)
#define DL_UNBIND_REQ_SIZE	sizeof(dl_unbind_req_t)
#define DL_SUBS_BIND_REQ_SIZE	sizeof(dl_subs_bind_req_t)
#define DL_SUBS_BIND_ACK_SIZE	sizeof(dl_subs_bind_ack_t)
#define DL_OK_ACK_SIZE		sizeof(dl_ok_ack_t)
#define DL_ERROR_ACK_SIZE	sizeof(dl_error_ack_t)
#define DL_UNITDATA_REQ_SIZE	sizeof(dl_unitdata_req_t)
#define DL_UNITDATA_IND_SIZE	sizeof(dl_unitdata_ind_t)
#define DL_UDERROR_IND_SIZE	sizeof(dl_uderror_ind_t)

#endif
