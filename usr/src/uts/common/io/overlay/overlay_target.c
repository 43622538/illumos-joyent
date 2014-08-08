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
 * Copyright (c) 2014 Joyent, Inc.
 */

/*
 * Overlay devices can operate in one of many modes. They may be a point to
 * point tunnel, they may be on a single multicast group, or they may have
 * dynamic destinations. All of these are programmed via varpd.
 *
 * XXX This all probably won't remain true.
 */

#include <sys/types.h>
#include <sys/ethernet.h>
#include <sys/kmem.h>
#include <sys/policy.h>
#include <sys/sysmacros.h>
#include <sys/stream.h>
#include <sys/strsun.h>
#include <sys/strsubr.h>
#include <sys/mac_provider.h>
#include <sys/mac_client.h>
#include <sys/mac_client_priv.h>
#include <sys/vlan.h>

#include <sys/overlay_impl.h>

/*
 * XXX This is total straw man, but at least it's a prime number. Here we're
 * going to have to go through and do a lot of evaluation and understanding as
 * to how these target caches should grow and shrink, as well as, memory
 * pressure and evictions. This just gives us a starting point that'll be 'good
 * enough'.
 */
#define	OVERLAY_HSIZE	823

/*
 * We use this data structure to keep track of what requests have been actively
 * allocated to a given instance so we know what to put back on the pending
 * list.
 */
typedef struct overlay_target_hdl {
	minor_t oth_minor;		/* RO */
	zoneid_t oth_zoneid;		/* RO */
	list_node_t oth_link;		/* overlay_target_lock */
	kmutex_t oth_lock;
	list_t	oth_outstanding;	/* oth_lock */
} overlay_target_hdl_t;

typedef int (*overlay_target_copyin_f)(void *, void **, size_t *, int);
typedef int (*overlay_target_ioctl_f)(overlay_target_hdl_t *, void *);
typedef int (*overlay_target_copyout_f)(void *, void *, size_t, int);

typedef struct overaly_target_ioctl {
	int		oti_cmd;	/* ioctl id */
	boolean_t	oti_write;	/* ioctl requires FWRITE */
	boolean_t	oti_ncopyout;	/* copyout data? */
	overlay_target_copyin_f oti_copyin;	/* copyin func */
	overlay_target_ioctl_f oti_func; /* function to call */
	overlay_target_copyout_f oti_copyout;	/* copyin func */
	size_t		oti_size;	/* size of user level structure */
} overlay_target_ioctl_t;

static kmem_cache_t *overlay_target_cache;
static kmem_cache_t *overlay_entry_cache;
static id_space_t *overlay_thdl_idspace;
static list_t overlay_thdl_list;
static void *overlay_thdl_state;
/*
 * XXX This all needs to become zone/netstack aware.
 */
static kmutex_t overlay_target_lock;
static kcondvar_t overlay_target_condvar;
static list_t overlay_target_list;

static int
overlay_target_cache_constructor(void *buf, void *arg, int kmflgs)
{
	overlay_target_t *ott = buf;

	mutex_init(&ott->ott_lock, NULL, MUTEX_DRIVER, NULL);
	return (0);
}

static void
overlay_target_cache_destructor(void *buf, void *arg)
{
	overlay_target_t *ott = buf;
	mutex_destroy(&ott->ott_lock);
}

static int
overlay_entry_cache_constructor(void *buf, void *arg, int kmflgs)
{
	overlay_target_entry_t *ote = buf;

	bzero(ote, sizeof (overlay_target_entry_t));
	mutex_init(&ote->ote_lock, NULL, MUTEX_DRIVER, NULL);
	return (0);
}

static void
overlay_entry_cache_destructor(void *buf, void *arg)
{
	overlay_target_entry_t *ote = buf;

	mutex_destroy(&ote->ote_lock);
}

static uint64_t
overlay_mac_hash(const void *v)
{
	return (0);
}

static int
overlay_mac_cmp(const void *a, const void *b)
{
	return (bcmp(a, b, ETHERADDRL));
}

static void
overlay_target_entry_dtor(void *arg)
{
}

void
overlay_target_init(void)
{
	int ret;
	ret = ddi_soft_state_init(&overlay_thdl_state,
	    sizeof (overlay_target_hdl_t), 1);
	VERIFY(ret == 0);
	overlay_target_cache = kmem_cache_create("overlay_target",
	    sizeof (overlay_target_t), 0, overlay_target_cache_constructor,
	    overlay_target_cache_destructor, NULL, NULL, NULL, 0);
	overlay_entry_cache = kmem_cache_create("overlay_entry",
	    sizeof (overlay_target_entry_t), 0, overlay_entry_cache_constructor,
	    overlay_entry_cache_destructor, NULL, NULL, NULL, 0);
	mutex_init(&overlay_target_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&overlay_target_condvar, NULL, CV_DRIVER, NULL);
	list_create(&overlay_target_list, sizeof (overlay_target_entry_t),
	    offsetof(overlay_target_entry_t, ote_qlink));
	list_create(&overlay_thdl_list, sizeof (overlay_target_hdl_t),
	    offsetof(overlay_target_hdl_t, oth_link));
	overlay_thdl_idspace = id_space_create("overlay_target_minors",
	    1, INT32_MAX);
}

void
overlay_target_fini(void)
{
	id_space_destroy(overlay_thdl_idspace);
	list_destroy(&overlay_thdl_list);
	list_destroy(&overlay_target_list);
	cv_destroy(&overlay_target_condvar);
	mutex_destroy(&overlay_target_lock);
	kmem_cache_destroy(overlay_entry_cache);
	kmem_cache_destroy(overlay_target_cache);
	ddi_soft_state_fini(&overlay_thdl_state);
}

void
overlay_target_free(overlay_dev_t *odd)
{
	if (odd->odd_target == NULL)
		return;

	kmem_cache_free(overlay_target_cache, odd->odd_target);
}

int
overlay_target_busy()
{
	int ret;

	mutex_enter(&overlay_target_lock);
	ret = !list_is_empty(&overlay_thdl_list);
	mutex_exit(&overlay_target_lock);

	return (ret);
}

static void
overlay_target_queue(overlay_target_entry_t *entry)
{
	mutex_enter(&overlay_target_lock);
	list_insert_tail(&overlay_target_list, entry);
	cv_signal(&overlay_target_condvar);
	mutex_exit(&overlay_target_lock);
}

/*
 * XXX This is assuming a non-gre style bits
 */
int
overlay_target_lookup(overlay_dev_t *odd, mblk_t *mp, struct sockaddr *sock,
    socklen_t *slenp)
{
	int ret;
	struct sockaddr_in6 *v6;
	overlay_target_t *ott;

	ASSERT(odd->odd_target != NULL);

	ott = odd->odd_target;
	if (ott->ott_dest != (OVERLAY_PLUGIN_D_IP | OVERLAY_PLUGIN_D_PORT))
		panic("implement me rm...");

	v6 = (struct sockaddr_in6 *)sock;
	bzero(v6, sizeof (struct sockaddr_in6));
	v6->sin6_family = AF_INET6;

	/* XXX Can we go lockless here, aka RO when in a mux? */
	if (ott->ott_mode == OVERLAY_TARGET_POINT) {
		mutex_enter(&ott->ott_lock);
		bcopy(&ott->ott_u.ott_point.otp_ip, &v6->sin6_addr,
		    sizeof (struct in6_addr));
		v6->sin6_port = htons(ott->ott_u.ott_point.otp_port);
		mutex_exit(&ott->ott_lock);
		*slenp = sizeof (struct sockaddr_in6);
		ret = OVERLAY_TARGET_OK;
	} else {
		mac_header_info_t mhi;
		overlay_target_entry_t *entry;

		ASSERT(ott->ott_mode == OVERLAY_TARGET_DYNAMIC);
		if (mac_header_info(odd->odd_mh, mp, &mhi) != 0)
			return (OVERLAY_TARGET_DROP);
		mutex_enter(&ott->ott_lock);
		entry = refhash_lookup(ott->ott_u.ott_dhash, mhi.mhi_daddr);
		if (entry == NULL) {
			/*
			 * XXX Create a new entry here and send it off to lookup
			 */
			entry = kmem_cache_alloc(overlay_entry_cache,
			    KM_NOSLEEP | KM_NORMALPRI);
			if (entry == NULL) {
				mutex_exit(&ott->ott_lock);
				return (OVERLAY_TARGET_DROP);
			}
			bcopy(mhi.mhi_daddr, entry->ote_addr, ETHERADDRL);
			entry->ote_chead = entry->ote_ctail = mp;
			entry->ote_mbsize = msgsize(mp);
			entry->ote_flags |= OVERLAY_ENTRY_F_PENDING;
			entry->ote_ott = ott;
			entry->ote_odd = odd;
			refhash_insert(ott->ott_u.ott_dhash, entry);
			mutex_exit(&ott->ott_lock);
			overlay_target_queue(entry);
			return (OVERLAY_TARGET_ASYNC);
		}
		refhash_hold(ott->ott_u.ott_dhash, entry);
		mutex_exit(&ott->ott_lock);

		mutex_enter(&entry->ote_lock);
		if (entry->ote_flags & OVERLAY_ENTRY_F_DROP) {
			ret = OVERLAY_TARGET_DROP;
		} else if (entry->ote_flags & OVERLAY_ENTRY_F_VALID) {
			/* XXX Check valid expiration at some point */
			bcopy(&entry->ote_dest.otp_ip, &v6->sin6_addr,
			    sizeof (struct in6_addr));
			v6->sin6_port = htons(entry->ote_dest.otp_port);
			*slenp = sizeof (struct sockaddr_in6);
			ret = OVERLAY_TARGET_OK;
		} else if (entry->ote_flags & OVERLAY_ENTRY_F_VARPD) {
			/* XXX actually implement this */
			ret = OVERLAY_TARGET_DROP;
		} else {
			size_t mlen = msgsize(mp);

			if (mlen + entry->ote_mbsize > 1024 * 1024) {
				ret = OVERLAY_TARGET_DROP;
			} else if (entry->ote_flags &
			    OVERLAY_ENTRY_F_PENDING) {
				ASSERT(entry->ote_ctail->b_next == NULL);
				entry->ote_ctail->b_next = mp;
				entry->ote_ctail = mp;
				entry->ote_mbsize += mlen;
				ret = OVERLAY_TARGET_ASYNC;
			} else {
				entry->ote_chead = mp;
				entry->ote_ctail = mp;
				entry->ote_mbsize += mlen;
				entry->ote_flags |= OVERLAY_ENTRY_F_PENDING;
				overlay_target_queue(entry);
				ret = OVERLAY_TARGET_ASYNC;
			}
		}
		mutex_exit(&entry->ote_lock);

		mutex_enter(&ott->ott_lock);
		refhash_rele(ott->ott_u.ott_dhash, entry);
		mutex_exit(&ott->ott_lock);
	}

	return (ret);
}

static int
overlay_target_info(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_info_t *oti = arg;

	odd = overlay_hold_by_dlid(oti->oti_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	oti->oti_flags = 0;
	oti->oti_needs = odd->odd_plugin->ovp_dest;
	if (odd->odd_flags & OVERLAY_F_DEGRADED)
		oti->oti_flags |= OVERLAY_TARG_INFO_F_DEGRADED;
	if (odd->odd_flags & OVERLAY_F_ACTIVATED)
		oti->oti_flags |= OVERLAY_TARG_INFO_F_ACTIVE;
	mutex_exit(&odd->odd_lock);
	overlay_hold_rele(odd);
	return (0);
}

static int
overlay_target_associate(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_target_t *ott;
	overlay_targ_associate_t *ota = arg;

	odd = overlay_hold_by_dlid(ota->ota_linkid);
	if (odd == NULL)
		return (ENOENT);

	if (ota->ota_id == 0) {
		overlay_hold_rele(odd);
		return (EINVAL);
	}

	if (ota->ota_mode != OVERLAY_TARGET_POINT &&
	    ota->ota_mode != OVERLAY_TARGET_DYNAMIC) {
		overlay_hold_rele(odd);
		return (EINVAL);
	}

	if (ota->ota_provides != odd->odd_plugin->ovp_dest) {
		overlay_hold_rele(odd);
		return (EINVAL);
	}

	if (ota->ota_mode == OVERLAY_TARGET_POINT) {
		/* XXX What checks make sense for Ethernet? */
		if (ota->ota_provides & OVERLAY_PLUGIN_D_IP) {
			if (IN6_IS_ADDR_UNSPECIFIED(&ota->ota_point.otp_ip) ||
			    IN6_IS_ADDR_V4COMPAT(&ota->ota_point.otp_ip) ||
			    IN6_IS_ADDR_V4MAPPED_ANY(&ota->ota_point.otp_ip)) {
				overlay_hold_rele(odd);
				return (EINVAL);
			}
		}

		if (ota->ota_provides & OVERLAY_PLUGIN_D_PORT) {
			if (ota->ota_point.otp_port == 0) {
				overlay_hold_rele(odd);
				return (EINVAL);
			}
		}
	}

	ott = kmem_cache_alloc(overlay_target_cache, KM_SLEEP);
	ott->ott_mode = ota->ota_mode;
	ott->ott_dest = ota->ota_provides;
	ott->ott_id = ota->ota_id;

	if (ott->ott_mode == OVERLAY_TARGET_POINT) {
		bcopy(&ota->ota_point, &ott->ott_u.ott_point,
		    sizeof (overlay_target_point_t));
	} else {
		ott->ott_u.ott_dhash = refhash_create(OVERLAY_HSIZE,
		    overlay_mac_hash, overlay_mac_cmp,
		    overlay_target_entry_dtor, sizeof (overlay_target_entry_t),
		    offsetof(overlay_target_entry_t, ote_reflink),
		    offsetof(overlay_target_entry_t, ote_addr), KM_SLEEP);

	}
	mutex_enter(&odd->odd_lock);
	if (odd->odd_flags & OVERLAY_F_VARPD) {
		mutex_exit(&odd->odd_lock);
		kmem_cache_free(overlay_target_cache, ott);
		overlay_hold_rele(odd);
		return (EEXIST);
	}

	odd->odd_flags |= OVERLAY_F_VARPD;
	odd->odd_target = ott;
	mutex_exit(&odd->odd_lock);
	overlay_hold_rele(odd);

	return (0);
}


static int
overlay_target_degrade(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_id_t *otid = arg;

	odd = overlay_hold_by_dlid(otid->otid_linkid);
	if (odd == NULL)
		return (ENOENT);

	overlay_fm_degrade(odd);
	overlay_hold_rele(odd);
	return (0);
}

static int
overlay_target_restore(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_id_t *otid = arg;

	odd = overlay_hold_by_dlid(otid->otid_linkid);
	if (odd == NULL)
		return (ENOENT);

	overlay_fm_restore(odd);
	overlay_hold_rele(odd);
	return (0);
}

static int
overlay_target_disassociate(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_dev_t *odd;
	overlay_targ_id_t *otid = arg;

	odd = overlay_hold_by_dlid(otid->otid_linkid);
	if (odd == NULL)
		return (ENOENT);

	mutex_enter(&odd->odd_lock);
	odd->odd_flags &= ~OVERLAY_F_VARPD;
	mutex_exit(&odd->odd_lock);

	overlay_hold_rele(odd);
	return (0);

}

static int
overlay_target_lookup_request(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_targ_lookup_t *otl = arg;
	overlay_target_entry_t *entry;
	clock_t ret, timeout;
	mac_header_info_t mhi;

	timeout = ddi_get_lbolt() + drv_usectohz(MICROSEC);
	mutex_enter(&overlay_target_lock);
	while (list_is_empty(&overlay_target_list)) {
		ret = cv_timedwait(&overlay_target_condvar,
		    &overlay_target_lock, timeout);
		if (ret == -1) {
			mutex_exit(&overlay_target_lock);
			return (ETIME);
		}
	}
	entry = list_remove_head(&overlay_target_list);
	mutex_exit(&overlay_target_lock);
	mutex_enter(&entry->ote_lock);
	ASSERT(!(entry->ote_flags & OVERLAY_ENTRY_F_VALID));
	ASSERT(entry->ote_chead != NULL);

	/* XXX We probably shouldn't assume it's valid */
	(void) mac_header_info(entry->ote_odd->odd_mh, entry->ote_chead, &mhi);

	otl->otl_dlid = entry->ote_odd->odd_linkid;
	otl->otl_reqid = (uintptr_t)entry;
	otl->otl_varpdid = entry->ote_ott->ott_id;
	otl->otl_vnetid = entry->ote_odd->odd_vid;

	otl->otl_hdrsize = mhi.mhi_hdrsize;
	otl->otl_pktsize = msgsize(entry->ote_chead) - otl->otl_hdrsize;
	bcopy(mhi.mhi_daddr, otl->otl_dstaddr, ETHERADDRL);
	bcopy(mhi.mhi_saddr, otl->otl_srcaddr, ETHERADDRL);
	otl->otl_dsttype = mhi.mhi_dsttype;
	otl->otl_sap = mhi.mhi_bindsap;
	otl->otl_vlan = VLAN_ID(mhi.mhi_tci);
	mutex_exit(&entry->ote_lock);

	mutex_enter(&thdl->oth_lock);
	list_insert_tail(&thdl->oth_outstanding, entry);
	mutex_exit(&thdl->oth_lock);

	return (0);
}

static int
overlay_target_lookup_respond(overlay_target_hdl_t *thdl, void *arg)
{
	const overlay_targ_resp_t *otr = arg;
	overlay_target_entry_t *entry;
	mblk_t *mp;

	mutex_enter(&thdl->oth_lock);
	for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
	    entry = list_next(&thdl->oth_outstanding, entry)) {
		if ((uintptr_t)entry == otr->otr_reqid)
			break;
	}

	if (entry == NULL) {
		mutex_exit(&thdl->oth_lock);
		return (EINVAL);
	}
	list_remove(&thdl->oth_outstanding, entry);
	mutex_exit(&thdl->oth_lock);

	mutex_enter(&entry->ote_lock);
	bcopy(&otr->otr_answer, &entry->ote_dest,
	    sizeof (overlay_target_point_t));
	entry->ote_flags &= ~OVERLAY_ENTRY_F_PENDING;
	entry->ote_flags |= OVERLAY_ENTRY_F_VALID;
	mp = entry->ote_chead;
	entry->ote_chead = NULL;
	entry->ote_ctail = NULL;
	entry->ote_mbsize = 0;
	entry->ote_vtime = gethrtime();
	mutex_exit(&entry->ote_lock);

	/*
	 * XXX Should we drain this in situ or some other way? For now, just
	 * call tx and then drop anything we get back...
	 */
	mp = overlay_m_tx(entry->ote_odd, mp);
	freemsgchain(mp);

	return (0);
}

static int
overlay_target_lookup_drop(overlay_target_hdl_t *thdl, void *arg)
{
	const overlay_targ_resp_t *otr = arg;
	overlay_target_entry_t *entry;
	mblk_t *mp;
	boolean_t queue = B_FALSE;

	mutex_enter(&thdl->oth_lock);
	for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
	    entry = list_next(&thdl->oth_outstanding, entry)) {
		if ((uintptr_t)entry == otr->otr_reqid)
			break;
	}

	if (entry == NULL) {
		mutex_exit(&thdl->oth_lock);
		return (EINVAL);
	}
	list_remove(&thdl->oth_outstanding, entry);
	mutex_exit(&thdl->oth_lock);

	mutex_enter(&entry->ote_lock);
	mp = entry->ote_chead;
	ASSERT(mp != NULL);
	entry->ote_chead = mp->b_next;
	mp->b_next = NULL;
	if (entry->ote_ctail == mp)
		entry->ote_ctail = entry->ote_chead;
	entry->ote_mbsize -= msgsize(mp);
	if (entry->ote_chead != NULL)
		queue = B_TRUE;
	else
		entry->ote_flags &= ~OVERLAY_ENTRY_F_PENDING;
	mutex_exit(&entry->ote_lock);

	if (queue == B_TRUE)
		overlay_target_queue(entry);
	freemsg(mp);

	return (0);
}

static int
overlay_target_pkt_copyin(void *ubuf, void **outp, size_t *bsize, int flags)
{
	int ret;
	overlay_targ_pkt_t *pkt;
	overlay_targ_pkt32_t *pkt32;

	pkt = kmem_alloc(sizeof (overlay_targ_pkt_t), KM_SLEEP);
	*outp = pkt;
	*bsize = sizeof (overlay_targ_pkt_t);
	if (ddi_model_convert_from(flags & FMODELS) == DDI_MODEL_ILP32) {
		uintptr_t addr;

		if (ddi_copyin(ubuf, pkt, sizeof (overlay_targ_pkt32_t),
		    flags & FKIOCTL) != 0) {
			kmem_free(pkt, *bsize);
			return (EFAULT);
		}
		pkt32 = (overlay_targ_pkt32_t *)pkt;
		addr = pkt32->otp_buf;
		pkt->otp_buf = (void *)addr;
	} else {
		if (ddi_copyin(ubuf, pkt, *bsize, flags & FKIOCTL) != 0) {
			kmem_free(pkt, *bsize);
			return (EFAULT);
		}
	}
	return (0);
}

static int
overlay_target_pkt_copyout(void *ubuf, void *buf, size_t bufsize, int flags)
{
	if (ddi_model_convert_from(flags & FMODELS) == DDI_MODEL_ILP32) {
		overlay_targ_pkt_t *pkt = buf;
		overlay_targ_pkt32_t *pkt32 = buf;
		uintptr_t addr = (uintptr_t)pkt->otp_buf;
		pkt32->otp_buf = (caddr32_t)addr;
		if (ddi_copyout(buf, ubuf, sizeof (overlay_targ_pkt32_t),
		    flags & FKIOCTL) != 0)
			return (EFAULT);
	} else {
		if (ddi_copyout(buf, ubuf, bufsize, flags & FKIOCTL) != 0)
			return (EFAULT);
	}
	return (0);
}

static int
overlay_target_packet(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_targ_pkt_t *pkt = arg;
	overlay_target_entry_t *entry;
	mblk_t *mp;
	size_t mlen;
	size_t boff;

	mutex_enter(&thdl->oth_lock);
	for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
	    entry = list_next(&thdl->oth_outstanding, entry)) {
		if ((uintptr_t)entry == pkt->otp_reqid)
			break;
	}

	if (entry == NULL) {
		mutex_exit(&thdl->oth_lock);
		return (EINVAL);
	}
	mutex_enter(&entry->ote_lock);
	mutex_exit(&thdl->oth_lock);
	mp = entry->ote_chead;
	mlen = MIN(msgsize(mp), pkt->otp_size);
	pkt->otp_size = mlen;
	boff = 0;
	while (mlen > 0) {
		size_t wlen = MIN(MBLKL(mp), mlen);
		if (ddi_copyout(mp->b_rptr,
		    (void *)((uintptr_t)pkt->otp_buf + boff),
		    wlen, 0) != 0) {
			mutex_exit(&entry->ote_lock);
			return (EFAULT);
		}
		mlen -= wlen;
		boff += wlen;
		mp = mp->b_cont;
	}
	mutex_exit(&entry->ote_lock);
	return (0);
}

static int
overlay_target_inject(overlay_target_hdl_t *thdl, void *arg)
{
	overlay_targ_pkt_t *pkt = arg;
	overlay_target_entry_t *entry;
	overlay_dev_t *odd;
	mblk_t *mp;

	/* XXX No support for injecting jumbo frames */
	if (pkt->otp_size > ETHERMAX + VLAN_TAGSZ)
		return (EINVAL);

	mp = allocb(pkt->otp_size, 0);
	if (mp == NULL)
		return (ENOMEM);

	if (ddi_copyin(pkt->otp_buf, mp->b_rptr, pkt->otp_size, 0) != 0) {
		freeb(mp);
		return (EFAULT);
	}
	mp->b_wptr += pkt->otp_size;

	if (pkt->otp_linkid != UINT64_MAX) {
		odd = overlay_hold_by_dlid(pkt->otp_linkid);
		if (odd == NULL) {
			freeb(mp);
			return (ENOENT);
		}
	} else {
		mutex_enter(&thdl->oth_lock);
		for (entry = list_head(&thdl->oth_outstanding); entry != NULL;
		    entry = list_next(&thdl->oth_outstanding, entry)) {
			if ((uintptr_t)entry == pkt->otp_reqid)
				break;
		}

		if (entry == NULL) {
			mutex_exit(&thdl->oth_lock);
			freeb(mp);
			return (ENOENT);
		}
		odd = entry->ote_odd;
		mutex_exit(&thdl->oth_lock);
	}

	mutex_enter(&odd->odd_lock);
	overlay_io_start(odd, OVERLAY_F_IN_RX);
	mutex_exit(&odd->odd_lock);

	mac_rx(odd->odd_mh, NULL, mp);

	mutex_enter(&odd->odd_lock);
	overlay_io_done(odd, OVERLAY_F_IN_RX);
	mutex_exit(&odd->odd_lock);

	return (0);
}

static overlay_target_ioctl_t overlay_target_ioctab[] = {
	{ OVERLAY_TARG_INFO, B_TRUE, B_TRUE,
		NULL, overlay_target_info,
		NULL, sizeof (overlay_targ_info_t)	},
	{ OVERLAY_TARG_ASSOCIATE, B_TRUE, B_FALSE,
		NULL, overlay_target_associate,
		NULL, sizeof (overlay_targ_associate_t)	},
	{ OVERLAY_TARG_DISASSOCIATE, B_TRUE, B_FALSE,
		NULL, overlay_target_disassociate,
		NULL, sizeof (overlay_targ_id_t)	},
	{ OVERLAY_TARG_DEGRADE, B_TRUE, B_FALSE,
		NULL, overlay_target_degrade,
		NULL, sizeof (overlay_targ_id_t)	},
	{ OVERLAY_TARG_RESTORE, B_TRUE, B_FALSE,
		NULL, overlay_target_restore,
		NULL, sizeof (overlay_targ_id_t)	},
	{ OVERLAY_TARG_LOOKUP, B_FALSE, B_TRUE,
		NULL, overlay_target_lookup_request,
		NULL, sizeof (overlay_targ_lookup_t)	},
	{ OVERLAY_TARG_RESPOND, B_TRUE, B_FALSE,
		NULL, overlay_target_lookup_respond,
		NULL, sizeof (overlay_targ_resp_t)	},
	{ OVERLAY_TARG_DROP, B_TRUE, B_FALSE,
		NULL, overlay_target_lookup_drop,
		NULL, sizeof (overlay_targ_resp_t)	},
	{ OVERLAY_TARG_PKT, B_TRUE, B_TRUE,
		overlay_target_pkt_copyin,
		overlay_target_packet,
		overlay_target_pkt_copyout,
		sizeof (overlay_targ_pkt_t)		},
	{ OVERLAY_TARG_INJECT, B_TRUE, B_FALSE,
		overlay_target_pkt_copyin,
		overlay_target_inject,
		NULL, sizeof (overlay_targ_pkt_t)	},
	{ 0 }
};

int
overlay_target_open(dev_t *devp, int flags, int otype, cred_t *credp)
{
	minor_t mid;
	overlay_target_hdl_t *thdl;

	if (secpolicy_dl_config(credp) != 0)
		return (EPERM);

	if (getminor(*devp) != 0)
		return (ENXIO);

	if (otype & OTYP_BLK)
		return (EINVAL);

	/* XXX nonblock, excl? */
	if (flags & ~(FREAD | FWRITE))
		return (EINVAL);

	/* XXX Really, you need FREAD and FWRITE? */
	if (!(flags & FREAD) || !(flags & FWRITE))
		return (EINVAL);

	/* XXX Relax this */
	if (crgetzoneid(credp) != GLOBAL_ZONEID)
		return (EPERM);

	mid = id_alloc(overlay_thdl_idspace);
	if (ddi_soft_state_zalloc(overlay_thdl_state, mid) != 0) {
		id_free(overlay_thdl_idspace, mid);
		return (ENXIO);
	}

	thdl = ddi_get_soft_state(overlay_thdl_state, mid);
	VERIFY(thdl != NULL);
	thdl->oth_minor = mid;
	thdl->oth_zoneid = crgetzoneid(credp);
	mutex_init(&thdl->oth_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&thdl->oth_outstanding, sizeof (overlay_target_entry_t),
	    offsetof(overlay_target_entry_t, ote_qlink));
	*devp = makedevice(getmajor(*devp), mid);

	mutex_enter(&overlay_target_lock);
	list_insert_tail(&overlay_thdl_list, thdl);
	mutex_exit(&overlay_target_lock);

	return (0);
}

int
overlay_target_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
	overlay_target_ioctl_t *ioc;
	overlay_target_hdl_t *thdl;

	if (secpolicy_dl_config(credp) != 0)
		return (EPERM);

	if ((thdl = ddi_get_soft_state(overlay_thdl_state,
	    getminor(dev))) == NULL)
		return (ENXIO);

	for (ioc = &overlay_target_ioctab[0]; ioc->oti_cmd != 0; ioc++) {
		int ret;
		caddr_t buf;
		size_t bufsize;

		if (ioc->oti_cmd != cmd)
			continue;

		/* XXX Really the write errno? */
		if (ioc->oti_write == B_TRUE && !(mode & FWRITE))
			return (EBADF);

		if (ioc->oti_copyin == NULL) {
			bufsize = ioc->oti_size;
			buf = kmem_alloc(bufsize, KM_SLEEP);
			if (ddi_copyin((void *)(uintptr_t)arg, buf, bufsize,
			    mode & FKIOCTL) != 0) {
				kmem_free(buf, bufsize);
				return (EFAULT);
			}
		} else {
			if ((ret = ioc->oti_copyin((void *)(uintptr_t)arg,
			    (void **)&buf, &bufsize, mode)) != 0)
				return (ret);
		}

		ret = ioc->oti_func(thdl, buf);
		if (ret == 0 && ioc->oti_size != 0 &&
		    ioc->oti_ncopyout == B_TRUE) {
			if (ioc->oti_copyout == NULL) {
				if (ddi_copyout(buf, (void *)(uintptr_t)arg,
				    bufsize, mode & FKIOCTL) != 0)
					ret = EFAULT;
			} else {
				ret = ioc->oti_copyout((void *)(uintptr_t)arg,
				    buf, bufsize, mode);
			}
		}

		kmem_free(buf, bufsize);
		return (ret);
	}

	return (ENOTTY);
}

int
overlay_target_close(dev_t dev, int flags, int otype, cred_t *credp)
{
	overlay_target_hdl_t *thdl;
	overlay_target_entry_t *entry;
	minor_t mid = getminor(dev);

	if ((thdl = ddi_get_soft_state(overlay_thdl_state, mid)) == NULL)
		return (ENXIO);

	mutex_enter(&overlay_target_lock);
	list_remove(&overlay_thdl_list, thdl);
	mutex_enter(&thdl->oth_lock);
	while ((entry = list_remove_head(&thdl->oth_outstanding)) != NULL)
		list_insert_tail(&overlay_thdl_list, entry);
	mutex_exit(&thdl->oth_lock);
	mutex_exit(&overlay_target_lock);

	list_destroy(&thdl->oth_outstanding);
	mutex_destroy(&thdl->oth_lock);
	mid = thdl->oth_minor;
	ddi_soft_state_free(overlay_thdl_state, mid);
	id_free(overlay_thdl_idspace, mid);

	return (0);
}
