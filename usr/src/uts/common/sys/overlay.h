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

#ifndef _SYS_OVERLAY_H
#define	_SYS_OVERLAY_H

/*
 * Overlay device support
 */

#include <sys/param.h>
#include <sys/dld_ioc.h>
#include <sys/mac.h>
#include <sys/overlay_prop.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	OVERLAY_IOC_CREATE	OVERLAYIOC(1)
#define	OVERLAY_IOC_DELETE	OVERLAYIOC(2)
#define	OVERLAY_IOC_PROPINFO	OVERLAYIOC(3)
#define	OVERLAY_IOC_GETPROP	OVERLAYIOC(4)
#define	OVERLAY_IOC_SETPROP	OVERLAYIOC(5)
#define	OVERLAY_IOC_NPROPS	OVERLAYIOC(6)

typedef struct overlay_ioc_create {
	datalink_id_t	oic_linkid;
	uint32_t	oic_filler;
	uint64_t	oic_vnetid;
	char		oic_encap[MAXLINKNAMELEN];
} overlay_ioc_create_t;

typedef struct overlay_ioc_delete {
	datalink_id_t	oid_linkid;
} overlay_ioc_delete_t;

typedef struct overlay_ioc_nprops {
	datalink_id_t	oipn_linkid;
	int32_t		oipn_nprops;
} overlay_ioc_nprops_t;

typedef struct overlay_ioc_propinfo {
	datalink_id_t	oipi_linkid;
	int32_t		oipi_id;
	char		oipi_name[OVERLAY_PROP_NAMELEN];
	uint_t		oipi_type;
	uint_t		oipi_prot;
	uchar_t		oipi_default[OVERLAY_PROP_SIZEMAX];
	uint32_t	oipi_defsize;
	mac_propval_range_t oipi_poss;
} overlay_ioc_propinfo_t;

typedef struct overlay_ioc_prop {
	datalink_id_t	oip_linkid;
	int32_t		oip_id;
	char		oip_name[OVERLAY_PROP_NAMELEN];
	uchar_t		oip_value[OVERLAY_PROP_SIZEMAX];
	uint32_t	oip_size;
} overlay_ioc_prop_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_OVERLAY_H */
