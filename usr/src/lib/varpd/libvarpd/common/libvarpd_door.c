/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.  *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright (c) 2014 Joyent, Inc.
 */

/*
 * varpd door server logic
 */

#include <door.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stropts.h>
#include <stdlib.h>
#include <strings.h>
#include <libvarpd_impl.h>

typedef int (libvarpd_door_f)(varpd_impl_t *, varpd_client_arg_t *, ucred_t *);

static int
libvarpd_door_f_create(varpd_impl_t *vip, varpd_client_arg_t *vcap,
    ucred_t *credp)
{
	int ret;
	varpd_instance_handle_t ihdl;
	varpd_client_create_arg_t *vccap = &vcap->vca_un.vca_create;

	vccap->vcca_plugin[LIBVARPD_PROP_NAMELEN-1] = '\0';
	ret = libvarpd_instance_create((varpd_handle_t)vip, vccap->vcca_linkid,
	    vccap->vcca_plugin, &ihdl);
	if (ret == 0)
		vccap->vcca_id = libvarpd_instance_id(ihdl);

	return (ret);
}

static int
libvarpd_door_f_activate(varpd_impl_t *vip, varpd_client_arg_t *vcap,
    ucred_t *credp)
{
	varpd_instance_handle_t ihp;
	varpd_client_instance_arg_t *vciap = &vcap->vca_un.vca_instance;

	ihp = libvarpd_instance_lookup((varpd_handle_t)vip, vciap->vcia_id);
	if (ihp == NULL)
		return (ENOENT);
	return (libvarpd_instance_activate(ihp));
}

static int
libvarpd_door_f_destroy(varpd_impl_t *vip, varpd_client_arg_t *vcap,
    ucred_t *credp)
{
	varpd_instance_handle_t ihp;
	varpd_client_instance_arg_t *vciap = &vcap->vca_un.vca_instance;

	ihp = libvarpd_instance_lookup((varpd_handle_t)vip, vciap->vcia_id);
	if (ihp == NULL)
		return (ENOENT);
	libvarpd_instance_destroy(ihp);
	return (0);
}

static int
libvarpd_door_f_nprops(varpd_impl_t *vip, varpd_client_arg_t *vcap,
    ucred_t *credp)
{
	varpd_instance_handle_t ihp;
	varpd_client_nprops_arg_t *vcnap = &vcap->vca_un.vca_nprops;

	ihp = libvarpd_instance_lookup((varpd_handle_t)vip, vcnap->vcna_id);
	if (ihp == NULL)
		return (ENOENT);

	return (libvarpd_prop_nprops(ihp, &vcnap->vcna_nprops));
}

static int
libvarpd_door_f_propinfo(varpd_impl_t *vip, varpd_client_arg_t *vcap,
    ucred_t *credp)
{
	int ret;
	varpd_instance_handle_t ihp;
	varpd_prop_handle_t phdl;
	varpd_client_propinfo_arg_t *vcfap = &vcap->vca_un.vca_info;

	ihp = libvarpd_instance_lookup((varpd_handle_t)vip, vcfap->vcfa_id);
	if (ihp == NULL)
		return (ENOENT);
	ret = libvarpd_prop_handle_alloc((varpd_handle_t)vip, ihp, &phdl);
	if (ret != 0)
		return (ret);

	if (vcfap->vcfa_propid != UINT_MAX) {
		ret = libvarpd_prop_info_fill(phdl, vcfap->vcfa_propid);
		if (ret != 0) {
			libvarpd_prop_handle_free(phdl);
			return (ret);
		}
	} else {
		uint_t i, nprop;
		const char *name;

		vcfap->vcfa_name[LIBVARPD_PROP_NAMELEN-1] = '\0';
		ret = libvarpd_prop_nprops(ihp, &nprop);
		if (ret != 0) {
			libvarpd_prop_handle_free(phdl);
			return (ret);
		}
		for (i = 0; i < nprop; i++) {
			ret = libvarpd_prop_info_fill(phdl, i);
			if (ret != 0) {
				libvarpd_prop_handle_free(phdl);
				return (ret);
			}
			ret = libvarpd_prop_info(phdl, &name, NULL, NULL, NULL,
			    NULL, NULL);
			if (ret != 0) {
				libvarpd_prop_handle_free(phdl);
				return (ret);
			}
			if (strcmp(vcfap->vcfa_name, name) == 0)
				break;
		}

		if (i == nprop) {
			libvarpd_prop_handle_free(phdl);
			return (ENOENT);
		}
		vcfap->vcfa_propid = i;
	}
	libvarpd_prop_door_convert(phdl, vcfap);
	libvarpd_prop_handle_free(phdl);
	return (0);
}

static int
libvarpd_door_f_getprop(varpd_impl_t *vip, varpd_client_arg_t *vcap,
    ucred_t *credp)
{
	int ret;
	uint32_t size;
	varpd_instance_handle_t ihp;
	varpd_prop_handle_t phdl;
	varpd_client_prop_arg_t *vcpap = &vcap->vca_un.vca_prop;

	ihp = libvarpd_instance_lookup((varpd_handle_t)vip, vcpap->vcpa_id);
	if (ihp == NULL)
		return (ENOENT);
	ret = libvarpd_prop_handle_alloc((varpd_handle_t)vip, ihp, &phdl);
	if (ret != 0)
		return (ret);

	ret = libvarpd_prop_info_fill(phdl, vcpap->vcpa_propid);
	if (ret != 0) {
		libvarpd_prop_handle_free(phdl);
		return (ret);
	}

	ret = libvarpd_prop_get(phdl, vcpap->vcpa_buf, &size);
	if (ret == 0)
		vcpap->vcpa_bufsize = size;
	libvarpd_prop_handle_free(phdl);
	return (0);
}

static int
libvarpd_door_f_setprop(varpd_impl_t *vip, varpd_client_arg_t *vcap,
    ucred_t *credp)
{
	int ret;
	varpd_instance_handle_t ihp;
	varpd_prop_handle_t phdl;
	varpd_client_prop_arg_t *vcpap = &vcap->vca_un.vca_prop;

	ihp = libvarpd_instance_lookup((varpd_handle_t)vip, vcpap->vcpa_id);
	if (ihp == NULL)
		return (ENOENT);
	ret = libvarpd_prop_handle_alloc((varpd_handle_t)vip, ihp, &phdl);
	if (ret != 0)
		return (ret);

	ret = libvarpd_prop_info_fill(phdl, vcpap->vcpa_propid);
	if (ret != 0) {
		libvarpd_prop_handle_free(phdl);
		return (ret);
	}

	ret = libvarpd_prop_set(phdl, vcpap->vcpa_buf, vcpap->vcpa_bufsize);
	libvarpd_prop_handle_free(phdl);
	return (ret);
}
static libvarpd_door_f *libvarpd_door_table[] = {
	libvarpd_door_f_create,
	libvarpd_door_f_activate,
	libvarpd_door_f_destroy,
	libvarpd_door_f_nprops,
	libvarpd_door_f_propinfo,
	libvarpd_door_f_getprop,
	libvarpd_door_f_setprop
};

static void
libvarpd_door_server(void *cookie, char *argp, size_t argsz, door_desc_t *dp,
    uint_t ndesc)
{
	int ret;
	varpd_client_eresp_t err;
	ucred_t *credp = NULL;
	varpd_impl_t *vip = cookie;
	varpd_client_arg_t *vcap = (varpd_client_arg_t *)argp;

	err.vce_command = VARPD_CLIENT_INVALID;
	if (argsz != sizeof (varpd_client_arg_t)) {
		err.vce_errno = EINVAL;
		goto errout;
	}

	if ((ret = door_ucred(&credp)) != 0) {
		err.vce_errno = ret;
		goto errout;
	}

	if (vcap->vca_command <= 0 || vcap->vca_command >= VARPD_CLIENT_MAX) {
		err.vce_errno = EINVAL;
		goto errout;
	}

	vcap->vca_errno = 0;
	ret = libvarpd_door_table[vcap->vca_command - 1](vip, vcap, credp);
	if (ret != 0)
		vcap->vca_errno = ret;

	ucred_free(credp);
	(void) door_return(argp, argsz, NULL, 0);
	return;

errout:
	/* XXX Should we do something here? */
	(void) door_return((char *)&err, sizeof (err), NULL, 0);
}

int
libvarpd_door_server_create(varpd_handle_t vhp, const char *path)
{
	int fd, ret;
	varpd_impl_t *vip = (varpd_impl_t *)vhp;

	if (vip->vdi_doorfd >= 0)
		return (EEXIST);

	vip->vdi_doorfd = door_create(libvarpd_door_server, vip,
	    DOOR_REFUSE_DESC | DOOR_NO_CANCEL);
	if (vip->vdi_doorfd == -1)
		return (errno);

	if ((fd = open(path, O_CREAT | O_RDWR)) == -1) {
		ret = errno;
		if (door_revoke(vip->vdi_doorfd) != 0)
			abort();
		return (errno);
	}
	/* XXX Really? */
	if (fchown(fd, UID_DLADM, GID_NETADM) != 0) {
		ret = errno;
		if (door_revoke(vip->vdi_doorfd) != 0)
			abort();
		return (ret);
	}

	(void) fdetach(path);
	if (fattach(vip->vdi_doorfd, path) != 0) {
		ret = errno;
		if (door_revoke(vip->vdi_doorfd) != 0)
			abort();
		return (ret);
	}

	return (0);
}

void
libvarpd_door_server_destroy(varpd_handle_t vhp)
{
	varpd_impl_t *vip = (varpd_impl_t *)vhp;
	if (vip->vdi_doorfd == -1)
		return;
	if (door_revoke(vip->vdi_doorfd) != 0)
		abort();
	vip->vdi_doorfd = -1;
}
