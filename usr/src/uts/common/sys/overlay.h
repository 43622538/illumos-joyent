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
	datalink_id_t	oic_overlay_id;
	char		oic_encap[MAXLINKNAMELEN];
} overlay_ioc_create_t;

typedef struct overlay_ioc_delete {
	datalink_id_t	oid_overlay_id;
} overlay_ioc_delete_t;

typedef struct overlay_ioc_nprops {
	datalink_id_t	oipn_overlay_id;
	int32_t		oipn_nprops;
} overlay_ioc_nprops_t;

typedef struct overlay_ioc_propinfo {
	datalink_id_t	oipi_overlay_id;
	uint32_t	oipi_id;
	char		oipi_name[OVERLAY_PROP_NAMELEN];
	uint_t		oipi_type;
	uint_t		oipi_prot;
	ssize_t		oipi_size;
	void		*oipi_default;
	ssize_t		oipi_defsize;
	mac_propval_range_t oipi_poss;
} overlay_ioc_propinfo_t;

typedef struct overlay_ioc_prop {
	datalink_id_t	oip_overlay_id;
	int32_t		oip_id;
	char		oip_name[OVERLAY_PROP_NAMELEN];
	ssize_t		oip_size;
	void		*oip_value;
} overlay_ioc_prop_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_OVERLAY_H */
