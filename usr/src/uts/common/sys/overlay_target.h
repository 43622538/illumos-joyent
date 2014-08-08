/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright (c) 2014 Joyent, Inc.  All rights reserved.
 */

#ifndef _OVERLAY_TARGET_H
#define	_OVERLAY_TARGET_H

/*
 * Overlay device varpd ioctl interface (/dev/overlay)
 */

#include <sys/types.h>
#include <sys/ethernet.h>
#include <netinet/in.h>
#include <sys/overlay_common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct overlay_target_point {
	uint8_t		otp_mac[ETHERADDRL];
	struct in6_addr	otp_ip;
	uint16_t	otp_port;
} overlay_target_point_t;

#define	OVERLAY_TARG_IOCTL	(('o' << 24) | ('v' << 16) | ('t' << 8))

#define	OVERLAY_TARG_INFO	(OVERLAY_TARG_IOCTL | 0x01)

typedef enum overlay_targ_info_flags {
	OVERLAY_TARG_INFO_F_ACTIVE = 0x01,
	OVERLAY_TARG_INFO_F_DEGRADED = 0x02
} overlay_targ_info_flags_t;

/*
 * Get target information about an overlay device
 */
typedef struct overlay_targ_info {
	datalink_id_t		oti_linkid;
	uint32_t		oti_needs;
	uint64_t		oti_flags;
} overlay_targ_info_t;

/*
 * Declare an association between a given varpd instance and a datalink.
 */
#define	OVERLAY_TARG_ASSOCIATE	(OVERLAY_TARG_IOCTL | 0x02)

typedef struct overlay_targ_associate {
	datalink_id_t		ota_linkid;
	uint32_t		ota_mode;
	uint64_t		ota_id;
	uint32_t		ota_provides;
	overlay_target_point_t	ota_point;
} overlay_targ_associate_t;

/*
 * Remove an association from a device. If the device has already been started,
 * this implies OVERLAY_TARG_DEGRADE.
 */
#define	OVERLAY_TARG_DISASSOCIATE	(OVERLAY_TARG_IOCTL | 0x3)

/*
 * Tells the kernel that while a varpd instance still exists, it basically isn't
 * making any forward progress, so the device should consider itself degraded.
 */
#define	OVERLAY_TARG_DEGRADE	(OVERLAY_TARG_IOCTL | 0x4)

/*
 * Tells the kernel to remove the degraded status that it set on a device.
 */
#define	OVERLAY_TARG_RESTORE	(OVERLAY_TARG_IOCTL | 0x5)

typedef struct overlay_targ_id {
	datalink_id_t	otid_linkid;
} overlay_targ_id_t;

/*
 * The following ioctls are all used to support dynamic lookups from userland,
 * generally serviced by varpd.
 *
 * The way this is designed to work is that user land will have threads sitting
 * in OVERLAY_TARG_LOOKUP ioctls waiting to service requests. A thread will sit
 * waiting for work for up to approximately one second of time before they will
 * be sent back out to user land to give user land a chance to clean itself up
 * or more generally, come back into the kernel for work. Once these threads
 * return, they will have a request with which more action can be done. The
 * following ioctls can all be used to answer the request.
 *
 *	OVERLAY_TARG_RESPOND - overlay_targ_resp_t
 *
 *		The overlay_targ_resp_t has the appropriate information from
 *		which a reply can be generated. The information is filled into
 *		an overlay_targ_point_t as appropriate based on the
 *		overlay_dest_t type.
 *
 *
 *	OVERLAY_TARG_DROP - overlay_targ_resp_t
 *
 *		The overlay_targ_resp_t should identify a request for which to
 *		drop a packet.
 *
 *
 * 	OVERLAY_TARG_INJECT - overlay_targ_pkt_t
 *
 * 		The overlay_targ_pkt_t injects a fully formed packet into the
 * 		virtual network. It may either be identified by its data link id
 * 		or by the request id. If both are specified, the
 * 		datalink id will be used. Note, that an injection is not
 * 		considered a reply and if this corresponds to a requeset, then
 * 		that individual packet must still be dropped.
 *
 *
 * 	OVERLAY_TARG_PKT - overlay_targ_pkt_t
 *
 * 		This ioctl can be used to copy data from a given request into a
 * 		user buffer. This can be used in combination with
 * 		OVERLAY_TARG_INJECT to implemnt services such as a proxy-arp.
 */
#define	OVERLAY_TARG_LOOKUP	(OVERLAY_TARG_IOCTL | 0x10)
#define	OVERLAY_TARG_RESPOND	(OVERLAY_TARG_IOCTL | 0x11)
#define	OVERLAY_TARG_DROP	(OVERLAY_TARG_IOCTL | 0x12)
#define	OVERLAY_TARG_INJECT	(OVERLAY_TARG_IOCTL | 0x13)
#define	OVERLAY_TARG_PKT	(OVERLAY_TARG_IOCTL | 0x14)

typedef struct overlay_targ_lookup {
	uint64_t	otl_dlid;
	uint64_t	otl_reqid;
	uint64_t	otl_varpdid;
	uint64_t	otl_vnetid;
	uint64_t	otl_hdrsize;
	uint64_t	otl_pktsize;
	uint8_t		otl_srcaddr[ETHERADDRL];
	uint8_t		otl_dstaddr[ETHERADDRL];
	uint32_t	otl_dsttype;
	uint32_t	otl_sap;
	int32_t		otl_vlan;
} overlay_targ_lookup_t;

typedef struct overlay_targ_resp {
	uint64_t	otr_reqid;
	overlay_target_point_t otr_answer;
} overlay_targ_resp_t;

typedef struct overlay_targ_pkt {
	uint64_t	otp_linkid;
	uint64_t	otp_reqid;
	uint64_t	otp_size;
	void		*otp_buf;
} overlay_targ_pkt_t;

#ifdef _KERNEL

typedef struct overlay_targ_pkt32 {
	uint64_t	otp_linkid;
	uint64_t	otp_reqid;
	uint64_t	otp_size;
	caddr32_t	otp_buf;
} overlay_targ_pkt32_t;

#endif /* _KERNEL */


#ifdef __cplusplus
}
#endif

#endif /* _OVERLAY_TARGET_H */
