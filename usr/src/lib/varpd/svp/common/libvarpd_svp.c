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
 * Copyright (c) 2014, Joyent, Inc.
 */

/*
 * This plugin implements the SDC VXLAN Protocol (SVP).
 *
 * XXX Expand on everything.
 */

#include <umem.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libnvpair.h>
#include <strings.h>
#include <string.h>
#include <assert.h>

#include <libvarpd_provider.h>
#include "libvarpd_svp.h"

static int svp_defport = 169;
static int svp_defuport = 1690;

static const char *varpd_svp_props[] = {
	"svp/host",
	"svp/port",
	"svp/underlay_ip",
	"svp/underlay_port"
};

static boolean_t
varpd_svp_valid_dest(overlay_plugin_dest_t dest)
{
	if (dest != (OVERLAY_PLUGIN_D_IP | OVERLAY_PLUGIN_D_PORT))
		return (B_FALSE);

	return (B_TRUE);
}

static int
varpd_svp_create(varpd_provider_handle_t hdl, void **outp,
    overlay_plugin_dest_t dest)
{
	int ret;
	svp_t *svp;

	if (varpd_svp_valid_dest(dest) == B_FALSE)
		return (ENOTSUP);

	svp = umem_zalloc(sizeof (svp_t), UMEM_DEFAULT);
	if (svp == NULL)
		return (ENOMEM);

	if ((ret = mutex_init(&svp->svp_lock, USYNC_THREAD, NULL) != 0)) {
		umem_free(svp, sizeof (svp_t));
		return (ret);
	}

	svp->svp_hdl = hdl;
	*outp = svp;
	return (0);
}

static int
varpd_svp_start(void *arg)
{
	svp_t *svp = arg;

	mutex_lock(&svp->svp_lock);
	if (svp->svp_host == NULL || svp->svp_port == 0 ||
	    svp->svp_huip == B_FALSE || svp->svp_uport == 0) {
		mutex_unlock(&svp->svp_lock);
		return (EAGAIN);
	}
	mutex_unlock(&svp->svp_lock);

	/* XXX rig up backend */
	return (0);
}

static int
varpd_svp_stop(void *arg)
{
	/* XXX Rig up backend decrement */
	return (0);
}

static void
varpd_svp_destroy(void *arg)
{
	svp_t *svp = arg;

	if (svp->svp_host != NULL)
		umem_free(svp->svp_host, strlen(svp->svp_host) + 1);

	if (mutex_destroy(&svp->svp_lock) != 0)
		abort();

	umem_free(svp, sizeof (svp_t));
}

static void
varpd_svp_lookup(void *arg, varpd_query_handle_t vqh,
    const overlay_targ_lookup_t *otl, overlay_target_point_t *otp)
{
	libvarpd_plugin_query_reply(vqh, VARPD_LOOKUP_DROP);
}

static int
varpd_svp_nprops(void *arg, uint_t *nprops)
{
	*nprops = sizeof (varpd_svp_props) / sizeof (char *);
	return (0);
}

static int
varpd_svp_propinfo(void *arg, uint_t propid, varpd_prop_handle_t vph)
{
	switch (propid) {
	case 0:
		/* svp/host */
		libvarpd_prop_set_name(vph, varpd_svp_props[0]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_STRING);
		libvarpd_prop_set_nodefault(vph);
		break;
	case 1:
		/* svp/port */
		libvarpd_prop_set_name(vph, varpd_svp_props[1]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_UINT);
		libvarpd_prop_set_default(vph, &svp_defport,
		    sizeof (svp_defport));
		libvarpd_prop_set_range_uint32(vph, 1, UINT16_MAX);
		break;
	case 2:
		/* svp/underlay_ip */
		libvarpd_prop_set_name(vph, varpd_svp_props[2]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_IP);
		libvarpd_prop_set_nodefault(vph);
		break;
	case 3:
		/* svp/underlay_port */
		libvarpd_prop_set_name(vph, varpd_svp_props[3]);
		libvarpd_prop_set_prot(vph, OVERLAY_PROP_PERM_RRW);
		libvarpd_prop_set_type(vph, OVERLAY_PROP_T_UINT);
		libvarpd_prop_set_default(vph, &svp_defuport,
		    sizeof (svp_defuport));
		libvarpd_prop_set_range_uint32(vph, 1, UINT16_MAX);
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int
varpd_svp_getprop(void *arg, const char *pname, void *buf, uint32_t *sizep)
{
	svp_t *svp = arg;

	/* svp/host */
	if (strcmp(pname, varpd_svp_props[0]) == 0) {
		size_t len;

		mutex_lock(&svp->svp_lock);
		if (svp->svp_host == NULL) {
			*sizep = 0;
		} else {
			len = strlen(svp->svp_host) + 1;
			if (*sizep < len) {
				mutex_unlock(&svp->svp_lock);
				return (EOVERFLOW);
			}
			*sizep = len;
			(void) strlcpy(buf, svp->svp_host, *sizep);
		}
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/port */
	if (strcmp(pname, varpd_svp_props[1]) == 0) {
		uint64_t val;

		if (*sizep < sizeof (uint64_t))
			return (EOVERFLOW);

		mutex_lock(&svp->svp_lock);
		if (svp->svp_port == 0) {
			*sizep = 0;
		} else {
			val = svp->svp_port;
			bcopy(&val, buf, sizeof (uint64_t));
			*sizep = sizeof (uint64_t);
		}

		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/underlay_ip */
	if (strcmp(pname, varpd_svp_props[2]) == 0) {
		if (*sizep > sizeof (struct in6_addr))
			return (EOVERFLOW);
		mutex_lock(&svp->svp_lock);
		if (svp->svp_huip == B_FALSE) {
			*sizep = 0;
		} else {
			bcopy(&svp->svp_uip, buf, sizeof (struct in6_addr));
			*sizep = sizeof (struct in6_addr);
		}
		return (0);
	}

	/* svp/underlay_port */
	if (strcmp(pname, varpd_svp_props[3]) == 0) {
		uint64_t val;

		if (*sizep < sizeof (uint64_t))
			return (EOVERFLOW);

		mutex_lock(&svp->svp_lock);
		if (svp->svp_uport == 0) {
			*sizep = 0;
		} else {
			val = svp->svp_uport;
			bcopy(&val, buf, sizeof (uint64_t));
			*sizep = sizeof (uint64_t);
		}

		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	return (EINVAL);
}

static int
varpd_svp_setprop(void *arg, const char *pname, const void *buf,
    const uint32_t size)
{
	svp_t *svp = arg;

	/* svp/host */
	if (strcmp(pname, varpd_svp_props[0]) == 0) {
		char *dup;
		/* XXX Validate hostname characters, maybe grab a C locale */
		dup = umem_alloc(size, UMEM_DEFAULT);
		(void) strlcpy(dup, buf, size);
		if (dup == NULL)
			return (ENOMEM);
		mutex_lock(&svp->svp_lock);
		if (svp->svp_host != NULL)
			umem_free(svp->svp_host, strlen(svp->svp_host) + 1);
		svp->svp_host = dup;
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/port */
	if (strcmp(pname, varpd_svp_props[1]) == 0) {
		const uint64_t *valp = buf;
		if (size < sizeof (uint64_t))
			return (EOVERFLOW);

		if (*valp == 0 || *valp > UINT16_MAX)
			return (EINVAL);

		mutex_lock(&svp->svp_lock);
		svp->svp_port = (uint16_t)*valp;
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/underlay_ip */
	if (strcmp(pname, varpd_svp_props[2]) == 0) {
		const struct in6_addr *ipv6 = buf;

		if (size < sizeof (struct in6_addr))
			return (EOVERFLOW);

		/*
		 * XXX Is anything else disallowed?
		 */
		if (IN6_IS_ADDR_V4COMPAT(ipv6))
			return (EINVAL);
		mutex_lock(&svp->svp_lock);
		bcopy(buf, &svp->svp_uip, sizeof (struct in6_addr));
		svp->svp_huip = B_TRUE;
		mutex_unlock(&svp->svp_lock);
		return (0);
	}

	/* svp/underlay_port */
	if (strcmp(pname, varpd_svp_props[3]) == 0) {
		const uint64_t *valp = buf;
		if (size < sizeof (uint64_t))
			return (EOVERFLOW);

		if (*valp == 0 || *valp > UINT16_MAX)
			return (EINVAL);

		mutex_lock(&svp->svp_lock);
		svp->svp_uport = (uint16_t)*valp;
		mutex_unlock(&svp->svp_lock);

		return (0);
	}

	return (EINVAL);
}

static int
varpd_svp_save(void *arg, nvlist_t *nvp)
{
	int ret;
	svp_t *svp = svp;

	mutex_lock(&svp->svp_lock);
	if (svp->svp_host != NULL) {
		if ((ret = nvlist_add_string(nvp, varpd_svp_props[0],
		    svp->svp_host)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	if (svp->svp_port != 0) {
		if ((ret = nvlist_add_uint16(nvp, varpd_svp_props[1],
		    svp->svp_port)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	if (svp->svp_huip == B_TRUE) {
		char buf[INET6_ADDRSTRLEN];

		if (inet_ntop(AF_INET6, &svp->svp_uip, buf, sizeof (buf)) ==
		    NULL)
			abort();

		if ((ret = nvlist_add_string(nvp, varpd_svp_props[2],
		    buf)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	if (svp->svp_uport != 0) {
		if ((ret = nvlist_add_uint16(nvp, varpd_svp_props[3],
		    svp->svp_uport)) != 0) {
			mutex_unlock(&svp->svp_lock);
			return (ret);
		}
	}

	mutex_unlock(&svp->svp_lock);
	return (0);
}

static int
varpd_svp_restore(nvlist_t *nvp, varpd_provider_handle_t hdl,
    overlay_plugin_dest_t dest, void **outp)
{
	int ret;
	svp_t *svp;
	char *ipstr, *hstr;

	if (varpd_svp_valid_dest(dest) == B_FALSE)
		return (ENOTSUP);

	svp = umem_alloc(sizeof (svp_t), UMEM_DEFAULT);
	if (svp == NULL)
		return (ENOMEM);

	if ((ret = mutex_init(&svp->svp_lock, USYNC_THREAD, NULL)) != 0) {
		umem_free(svp, sizeof (svp_t));
		return (ret);
	}

	/* XXX Validate hostname */
	if ((ret = nvlist_lookup_string(nvp, varpd_svp_props[0],
	    &hstr)) != 0) {
		if (ret != ENOENT) {
			if (mutex_destroy(&svp->svp_lock) != 0)
				abort();
			umem_free(svp, sizeof (svp_t));
		}
		svp->svp_host = NULL;
	} else {
		size_t blen = strlen(hstr) + 1;
		svp->svp_host = umem_alloc(blen, UMEM_DEFAULT);
		(void) strlcpy(svp->svp_host, hstr, blen);
	}

	if ((ret = nvlist_lookup_uint16(nvp, varpd_svp_props[1],
	    &svp->svp_port)) != 0) {
		if (ret != ENOENT) {
			if (mutex_destroy(&svp->svp_lock) != 0)
				abort();
			if (svp->svp_host != NULL)
				umem_free(svp->svp_host,
				    strlen(svp->svp_host) + 1);
			umem_free(svp, sizeof (svp_t));
		}
		svp->svp_port = 0;
	}

	if ((ret = nvlist_lookup_string(nvp, varpd_svp_props[2],
	    &ipstr)) != 0) {
		if (ret != ENOENT) {
			if (mutex_destroy(&svp->svp_lock) != 0)
				abort();
			if (svp->svp_host != NULL)
				umem_free(svp->svp_host,
				    strlen(svp->svp_host) + 1);
			umem_free(svp, sizeof (svp_t));
			return (ret);
		}
		svp->svp_huip = B_FALSE;
	} else {
		ret = inet_pton(AF_INET6, ipstr, &svp->svp_uip);
		if (ret == -1) {
			assert(errno == EAFNOSUPPORT);
			abort();
		}

		if (ret == 0) {
			if (mutex_destroy(&svp->svp_lock) != 0)
				abort();
			if (svp->svp_host != NULL)
				umem_free(svp->svp_host,
				    strlen(svp->svp_host) + 1);
			umem_free(svp, sizeof (svp_t));
			return (EINVAL);
		}
		svp->svp_huip = B_TRUE;
	}

	if ((ret = nvlist_lookup_uint16(nvp, varpd_svp_props[3],
	    &svp->svp_uport)) != 0) {
		if (ret != ENOENT) {
			if (mutex_destroy(&svp->svp_lock) != 0)
				abort();
			if (svp->svp_host != NULL)
				umem_free(svp->svp_host,
				    strlen(svp->svp_host) + 1);
			umem_free(svp, sizeof (svp_t));
		}
		svp->svp_uport = 0;
	}

	svp->svp_hdl = hdl;
	*outp = svp;
	return (0);
}

static void
varpd_svp_arp(void *arg, varpd_arp_handle_t vah, int type,
    const struct sockaddr *sock, uint8_t *out)
{
	libvarpd_
}

static const varpd_plugin_ops_t varpd_svp_ops = {
	0,
	varpd_svp_create,
	varpd_svp_start,
	varpd_svp_stop,
	varpd_svp_destroy,
	NULL,
	varpd_svp_lookup,
	varpd_svp_nprops,
	varpd_svp_propinfo,
	varpd_svp_getprop,
	varpd_svp_setprop,
	varpd_svp_save,
	varpd_svp_restore,
	varpd_svp_arp,
	NULL
};

#pragma init(varpd_svp_init)
static void
varpd_svp_init(void)
{
	int err;
	varpd_plugin_register_t *vpr;

	/* XXX Revisit failure semantics here */
	vpr = libvarpd_plugin_alloc(VARPD_CURRENT_VERSION, &err);
	if (vpr == NULL)
		return;

	vpr->vpr_mode = OVERLAY_TARGET_DYNAMIC;
	vpr->vpr_name = "svp";
	vpr->vpr_ops = &varpd_svp_ops;

	(void) libvarpd_plugin_register(vpr);
	libvarpd_plugin_free(vpr);
}
