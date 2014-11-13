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

/*
 * varpd library
 */

#include <stdlib.h>
#include <errno.h>
#include <umem.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/avl.h>
#include <stddef.h>
#include <stdio.h>
#include <strings.h>

#include <libvarpd_impl.h>

static int
libvarpd_instance_comparator(const void *lp, const void *rp)
{
	const varpd_instance_t *lpp, *rpp;
	lpp = lp;
	rpp = rp;

	if (lpp->vri_id > rpp->vri_id)
		return (1);
	if (lpp->vri_id < rpp->vri_id)
		return (-1);
	return (0);
}

static int
libvarpd_instance_lcomparator(const void *lp, const void *rp)
{
	const varpd_instance_t *lpp, *rpp;
	lpp = lp;
	rpp = rp;

	if (lpp->vri_linkid > rpp->vri_linkid)
		return (1);
	if (lpp->vri_linkid < rpp->vri_linkid)
		return (-1);
	return (0);
}


int
libvarpd_create(varpd_handle_t *vphp)
{
	int ret;
	varpd_impl_t *vip;
	char buf[32];

	if (vphp == NULL)
		return (EINVAL);

	*vphp = NULL;
	vip = umem_alloc(sizeof (varpd_impl_t), UMEM_DEFAULT);
	if (vip == NULL)
		return (errno);

	bzero(vip, sizeof (varpd_impl_t));
	(void) snprintf(buf, sizeof (buf), "varpd_%p", vip);
	vip->vdi_idspace = id_space_create(buf, LIBVARPD_ID_MIN,
	    LIBVARPD_ID_MAX);
	if (vip->vdi_idspace == NULL) {
		int ret = errno;
		umem_free(vip, sizeof (varpd_impl_t));
		return (ret);
	}

	vip->vdi_qcache = umem_cache_create("query", sizeof (varpd_query_t), 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	if (vip->vdi_qcache == NULL) {
		int ret = errno;
		id_space_destroy(vip->vdi_idspace);
		umem_free(vip, sizeof (varpd_impl_t));
		return (ret);
	}

	if ((ret = libvarpd_overlay_init(vip)) != 0) {
		umem_cache_destroy(vip->vdi_qcache);
		id_space_destroy(vip->vdi_idspace);
		umem_free(vip, sizeof (varpd_impl_t));
		return (ret);
	}

	libvarpd_persist_init(vip);

	avl_create(&vip->vdi_plugins, libvarpd_plugin_comparator,
	    sizeof (varpd_plugin_t), offsetof(varpd_plugin_t, vpp_node));

	avl_create(&vip->vdi_instances, libvarpd_instance_comparator,
	    sizeof (varpd_instance_t), offsetof(varpd_instance_t, vri_inode));
	avl_create(&vip->vdi_linstances, libvarpd_instance_lcomparator,
	    sizeof (varpd_instance_t), offsetof(varpd_instance_t, vri_lnode));

	if (mutex_init(&vip->vdi_lock, USYNC_THREAD, NULL) != 0)
		abort();

	if (mutex_init(&vip->vdi_loglock, USYNC_THREAD, NULL) != 0)
		abort();

	vip->vdi_doorfd = -1;
	*vphp = (varpd_handle_t)vip;
	return (0);
}

void
libvarpd_destroy(varpd_handle_t vhp)
{
	varpd_impl_t *vip = (varpd_impl_t *)vhp;

	if (mutex_destroy(&vip->vdi_loglock) != 0)
		abort();
	if (mutex_destroy(&vip->vdi_lock) != 0)
		abort();
	libvarpd_persist_fini(vip);
	libvarpd_overlay_fini(vip);
	umem_cache_destroy(vip->vdi_qcache);
	id_space_destroy(vip->vdi_idspace);
	umem_free(vip, sizeof (varpd_impl_t));
}

FILE *
libvarpd_set_logfile(varpd_handle_t vhp, FILE *fp)
{
	FILE *old;
	varpd_impl_t *vip = (varpd_impl_t *)vhp;

	mutex_lock(&vip->vdi_loglock);
	old = vip->vdi_err;
	vip->vdi_err = fp;
	mutex_unlock(&vip->vdi_loglock);

	return (old);
}

int
libvarpd_instance_create(varpd_handle_t vhp, datalink_id_t linkid,
    const char *pname, varpd_instance_handle_t *outp)
{
	int ret;
	varpd_impl_t *vip = (varpd_impl_t *)vhp;
	varpd_plugin_t *plugin;
	varpd_instance_t *inst, lookup;
	overlay_plugin_dest_t dest;

	/* XXX Really want our own errnos */
	plugin = libvarpd_plugin_lookup(vip, pname);
	if (plugin == NULL)
		return (ENOENT);

	if ((ret = libvarpd_overlay_info(vip, linkid, &dest, NULL)) != 0)
		return (ret);

	inst = umem_alloc(sizeof (varpd_instance_t), UMEM_DEFAULT);
	if (inst == NULL)
		return (ENOMEM);

	inst->vri_id = id_alloc(vip->vdi_idspace);
	inst->vri_linkid = linkid;
	inst->vri_mode = plugin->vpp_mode;
	inst->vri_dest = dest;
	inst->vri_plugin = plugin;
	inst->vri_impl = vip;
	inst->vri_flags = 0;
	if ((ret = plugin->vpp_ops->vpo_create((varpd_provider_handle_t)inst,
	    &inst->vri_private, dest)) != 0) {
		id_free(vip->vdi_idspace, inst->vri_id);
		umem_free(inst, sizeof (varpd_instance_t));
		return (ret);
	}

	if (mutex_init(&inst->vri_lock, USYNC_THREAD, NULL) != 0)
		abort();

	mutex_lock(&vip->vdi_lock);
	lookup.vri_id = inst->vri_id;
	if (avl_find(&vip->vdi_instances, &lookup, NULL) != NULL)
		abort();
	avl_add(&vip->vdi_instances, inst);
	lookup.vri_linkid = inst->vri_linkid;
	if (avl_find(&vip->vdi_linstances, &lookup, NULL) != NULL)
		abort();
	avl_add(&vip->vdi_linstances, inst);
	mutex_unlock(&vip->vdi_lock);
	*outp = (varpd_instance_handle_t)inst;
	return (0);
}

uint64_t
libvarpd_instance_id(varpd_instance_handle_t ihp)
{
	varpd_instance_t *inst = (varpd_instance_t *)ihp;
	return (inst->vri_id);
}

varpd_instance_handle_t
libvarpd_instance_lookup(varpd_handle_t vhp, uint64_t id)
{
	varpd_impl_t *vip = (varpd_impl_t *)vhp;
	varpd_instance_t lookup, *retp;

	lookup.vri_id = id;
	mutex_lock(&vip->vdi_lock);
	retp = avl_find(&vip->vdi_instances, &lookup, NULL);
	mutex_unlock(&vip->vdi_lock);
	return ((varpd_instance_handle_t)retp);
}

/*
 * If this function becomes external to varpd, we need to change it to return a
 * varpd_instance_handle_t.
 */
varpd_instance_t *
libvarpd_instance_lookup_by_dlid(varpd_impl_t *vip, datalink_id_t linkid)
{
	varpd_instance_t lookup, *retp;

	lookup.vri_linkid = linkid;
	mutex_lock(&vip->vdi_lock);
	retp = avl_find(&vip->vdi_linstances, &lookup, NULL);
	mutex_unlock(&vip->vdi_lock);
	return (retp);
}

void
libvarpd_instance_destroy(varpd_instance_handle_t ihp)
{
	abort();
}

int
libvarpd_instance_activate(varpd_instance_handle_t ihp)
{
	int ret;
	varpd_instance_t *inst = (varpd_instance_t *)ihp;

	if (mutex_lock(&inst->vri_lock) != 0)
		abort();

	if (inst->vri_flags & VARPD_INSTANCE_F_ACTIVATED) {
		ret = EEXIST;
		goto out;
	}

	if ((ret = inst->vri_plugin->vpp_ops->vpo_start(inst->vri_private)) !=
	    0)
		goto out;

	if ((ret = libvarpd_persist_instance(inst->vri_impl, inst)) != 0)
		goto out;

	/* XXX We should call stop if this fails */
	if ((ret = libvarpd_overlay_associate(inst)) != 0)
		goto out;

	inst->vri_flags |= VARPD_INSTANCE_F_ACTIVATED;

out:
	if (mutex_unlock(&inst->vri_lock))
		abort();
	return (ret);
}

static void
libvarpd_prefork(void)
{
	libvarpd_plugin_prefork();
}

static void
libvarpd_postfork(void)
{
	libvarpd_plugin_postfork();
}

#pragma init(libvarpd_init)
static void
libvarpd_init(void)
{
	libvarpd_plugin_init();
	if (pthread_atfork(NULL, libvarpd_prefork, libvarpd_postfork) != 0)
		abort();
}

#pragma fini(libvarpd_fini)
static void
libvarpd_fini(void)
{
	libvarpd_plugin_fini();
}
