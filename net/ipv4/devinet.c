// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	NET3	IP device support routines.
 *
 *	Derived from the IP parts of dev.c 1.0.19
 * 		Authors:	Ross Biro
 *				Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *				Mark Evans, <evansmp@uhura.aston.ac.uk>
 *
 *	Additional Authors:
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 *	Changes:
 *		Alexey Kuznetsov:	pa_* fields are replaced with ifaddr
 *					lists.
 *		Cyrus Durgin:		updated for kmod
 *		Matthias Andree:	in devinet_ioctl, compare label and
 *					address (4.4BSD alias style support),
 *					fall back to comparing just the label
 *					if no match found.
 */


#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/if_addr.h>
#include <linux/if_ether.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/inetdevice.h>
#include <linux/igmp.h>
#include "igmp_internal.h"
#include <linux/slab.h>
#include <linux/hash.h>
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif
#include <linux/kmod.h>
#include <linux/netconf.h>

#include <net/arp.h>
#include <net/ip.h>
#include <net/route.h>
#include <net/ip_fib.h>
#include <net/rtnetlink.h>
#include <net/net_namespace.h>
#include <net/addrconf.h>

#define IPV6ONLY_FLAGS	\
		(IFA_F_NODAD | IFA_F_OPTIMISTIC | IFA_F_DADFAILED | \
		 IFA_F_HOMEADDRESS | IFA_F_TENTATIVE | \
		 IFA_F_MANAGETEMPADDR | IFA_F_STABLE_PRIVACY)

static struct ipv4_devconf ipv4_devconf = {
	.data = {
		[IPV4_DEVCONF_ACCEPT_REDIRECTS - 1] = 1,
		[IPV4_DEVCONF_SEND_REDIRECTS - 1] = 1,
		[IPV4_DEVCONF_SECURE_REDIRECTS - 1] = 1,
		[IPV4_DEVCONF_SHARED_MEDIA - 1] = 1,
		[IPV4_DEVCONF_IGMPV2_UNSOLICITED_REPORT_INTERVAL - 1] = 10000 /*ms*/,
		[IPV4_DEVCONF_IGMPV3_UNSOLICITED_REPORT_INTERVAL - 1] =  1000 /*ms*/,
		[IPV4_DEVCONF_ARP_EVICT_NOCARRIER - 1] = 1,
	},
};

static struct ipv4_devconf ipv4_devconf_dflt = {
	.data = {
		[IPV4_DEVCONF_ACCEPT_REDIRECTS - 1] = 1,
		[IPV4_DEVCONF_SEND_REDIRECTS - 1] = 1,
		[IPV4_DEVCONF_SECURE_REDIRECTS - 1] = 1,
		[IPV4_DEVCONF_SHARED_MEDIA - 1] = 1,
		[IPV4_DEVCONF_ACCEPT_SOURCE_ROUTE - 1] = 1,
		[IPV4_DEVCONF_IGMPV2_UNSOLICITED_REPORT_INTERVAL - 1] = 10000 /*ms*/,
		[IPV4_DEVCONF_IGMPV3_UNSOLICITED_REPORT_INTERVAL - 1] =  1000 /*ms*/,
		[IPV4_DEVCONF_ARP_EVICT_NOCARRIER - 1] = 1,
	},
};

#define IPV4_DEVCONF_DFLT(net, attr) \
	IPV4_DEVCONF((*net->ipv4.devconf_dflt), attr)

static const struct nla_policy ifa_ipv4_policy[IFA_MAX+1] = {
	[IFA_LOCAL]     	= { .type = NLA_U32 },
	[IFA_ADDRESS]   	= { .type = NLA_U32 },
	[IFA_BROADCAST] 	= { .type = NLA_U32 },
	[IFA_LABEL]     	= { .type = NLA_STRING, .len = IFNAMSIZ - 1 },
	[IFA_CACHEINFO]		= { .len = sizeof(struct ifa_cacheinfo) },
	[IFA_FLAGS]		= { .type = NLA_U32 },
	[IFA_RT_PRIORITY]	= { .type = NLA_U32 },
	[IFA_TARGET_NETNSID]	= { .type = NLA_S32 },
	[IFA_PROTO]		= { .type = NLA_U8 },
};

#define IN4_ADDR_HSIZE_SHIFT	8
#define IN4_ADDR_HSIZE		(1U << IN4_ADDR_HSIZE_SHIFT)

static u32 inet_addr_hash(const struct net *net, __be32 addr)
{
	u32 val = __ipv4_addr_hash(addr, net_hash_mix(net));

	return hash_32(val, IN4_ADDR_HSIZE_SHIFT);
}

static void inet_hash_insert(struct net *net, struct in_ifaddr *ifa)
{
	u32 hash = inet_addr_hash(net, ifa->ifa_local);

	ASSERT_RTNL();
	hlist_add_head_rcu(&ifa->addr_lst, &net->ipv4.inet_addr_lst[hash]);
}

static void inet_hash_remove(struct in_ifaddr *ifa)
{
	ASSERT_RTNL();
	hlist_del_init_rcu(&ifa->addr_lst);
}

/**
 * __ip_dev_find - find the first device with a given source address.
 * @net: the net namespace
 * @addr: the source address
 * @devref: if true, take a reference on the found device
 *
 * If a caller uses devref=false, it should be protected by RCU, or RTNL
 */
struct net_device *__ip_dev_find(struct net *net, __be32 addr, bool devref)
{
	struct net_device *result = NULL;
	struct in_ifaddr *ifa;

	rcu_read_lock();
	ifa = inet_lookup_ifaddr_rcu(net, addr);
	if (!ifa) {
		struct flowi4 fl4 = { .daddr = addr };
		struct fib_result res = { 0 };
		struct fib_table *local;

		/* Fallback to FIB local table so that communication
		 * over loopback subnets work.
		 */
		local = fib_get_table(net, RT_TABLE_LOCAL);
		if (local &&
		    !fib_table_lookup(local, &fl4, &res, FIB_LOOKUP_NOREF) &&
		    res.type == RTN_LOCAL)
			result = FIB_RES_DEV(res);
	} else {
		result = ifa->ifa_dev->dev;
	}
	if (result && devref)
		dev_hold(result);
	rcu_read_unlock();
	return result;
}
EXPORT_SYMBOL(__ip_dev_find);

/* called under RCU lock */
struct in_ifaddr *inet_lookup_ifaddr_rcu(struct net *net, __be32 addr)
{
	u32 hash = inet_addr_hash(net, addr);
	struct in_ifaddr *ifa;

	hlist_for_each_entry_rcu(ifa, &net->ipv4.inet_addr_lst[hash], addr_lst)
		if (ifa->ifa_local == addr)
			return ifa;

	return NULL;
}

static void rtmsg_ifa(int event, struct in_ifaddr *, struct nlmsghdr *, u32);

static BLOCKING_NOTIFIER_HEAD(inetaddr_chain);
static BLOCKING_NOTIFIER_HEAD(inetaddr_validator_chain);
static void inet_del_ifa(struct in_device *in_dev,
			 struct in_ifaddr __rcu **ifap,
			 int destroy);
#ifdef CONFIG_SYSCTL
static int devinet_sysctl_register(struct in_device *idev);
static void devinet_sysctl_unregister(struct in_device *idev);
#else
static int devinet_sysctl_register(struct in_device *idev)
{
	return 0;
}
static void devinet_sysctl_unregister(struct in_device *idev)
{
}
#endif

/* Locks all the inet devices. */

static struct in_ifaddr *inet_alloc_ifa(struct in_device *in_dev)
{
	struct in_ifaddr *ifa;

	ifa = kzalloc(sizeof(*ifa), GFP_KERNEL_ACCOUNT);
	if (!ifa)
		return NULL;

	in_dev_hold(in_dev);
	ifa->ifa_dev = in_dev;

	INIT_HLIST_NODE(&ifa->addr_lst);

	return ifa;
}

static void inet_rcu_free_ifa(struct rcu_head *head)
{
	struct in_ifaddr *ifa = container_of(head, struct in_ifaddr, rcu_head);

	in_dev_put(ifa->ifa_dev);
	kfree(ifa);
}

static void inet_free_ifa(struct in_ifaddr *ifa)
{
	/* Our reference to ifa->ifa_dev must be freed ASAP
	 * to release the reference to the netdev the same way.
	 * in_dev_put() -> in_dev_finish_destroy() -> netdev_put()
	 */
	call_rcu_hurry(&ifa->rcu_head, inet_rcu_free_ifa);
}

static void in_dev_free_rcu(struct rcu_head *head)
{
	struct in_device *idev = container_of(head, struct in_device, rcu_head);

	kfree(rcu_dereference_protected(idev->mc_hash, 1));
	kfree(idev);
}

void in_dev_finish_destroy(struct in_device *idev)
{
	struct net_device *dev = idev->dev;

	WARN_ON(idev->ifa_list);
	WARN_ON(idev->mc_list);
#ifdef NET_REFCNT_DEBUG
	pr_debug("%s: %p=%s\n", __func__, idev, dev ? dev->name : "NIL");
#endif
	netdev_put(dev, &idev->dev_tracker);
	if (!idev->dead)
		pr_err("Freeing alive in_device %p\n", idev);
	else
		call_rcu(&idev->rcu_head, in_dev_free_rcu);
}
EXPORT_SYMBOL(in_dev_finish_destroy);

static struct in_device *inetdev_init(struct net_device *dev)
{
	struct in_device *in_dev;
	int err = -ENOMEM;

	ASSERT_RTNL();

	in_dev = kzalloc(sizeof(*in_dev), GFP_KERNEL);
	if (!in_dev)
		goto out;
	memcpy(&in_dev->cnf, dev_net(dev)->ipv4.devconf_dflt,
			sizeof(in_dev->cnf));
	in_dev->cnf.sysctl = NULL;
	in_dev->dev = dev;
	in_dev->arp_parms = neigh_parms_alloc(dev, &arp_tbl);
	if (!in_dev->arp_parms)
		goto out_kfree;
	if (IPV4_DEVCONF(in_dev->cnf, FORWARDING))
		netif_disable_lro(dev);
	/* Reference in_dev->dev */
	netdev_hold(dev, &in_dev->dev_tracker, GFP_KERNEL);
	/* Account for reference dev->ip_ptr (below) */
	refcount_set(&in_dev->refcnt, 1);

	if (dev != blackhole_netdev) {
		err = devinet_sysctl_register(in_dev);
		if (err) {
			in_dev->dead = 1;
			neigh_parms_release(&arp_tbl, in_dev->arp_parms);
			in_dev_put(in_dev);
			in_dev = NULL;
			goto out;
		}
		ip_mc_init_dev(in_dev);
		if (dev->flags & IFF_UP)
			ip_mc_up(in_dev);
	}

	/* we can receive as soon as ip_ptr is set -- do this last */
	rcu_assign_pointer(dev->ip_ptr, in_dev);
out:
	return in_dev ?: ERR_PTR(err);
out_kfree:
	kfree(in_dev);
	in_dev = NULL;
	goto out;
}

static void inetdev_destroy(struct in_device *in_dev)
{
	struct net_device *dev;
	struct in_ifaddr *ifa;

	ASSERT_RTNL();

	dev = in_dev->dev;

	in_dev->dead = 1;

	ip_mc_destroy_dev(in_dev);

	while ((ifa = rtnl_dereference(in_dev->ifa_list)) != NULL) {
		inet_del_ifa(in_dev, &in_dev->ifa_list, 0);
		inet_free_ifa(ifa);
	}

	RCU_INIT_POINTER(dev->ip_ptr, NULL);

	devinet_sysctl_unregister(in_dev);
	neigh_parms_release(&arp_tbl, in_dev->arp_parms);
	arp_ifdown(dev);

	in_dev_put(in_dev);
}

static int __init inet_blackhole_dev_init(void)
{
	int err = 0;

	rtnl_lock();
	if (!inetdev_init(blackhole_netdev))
		err = -ENOMEM;
	rtnl_unlock();

	return err;
}
late_initcall(inet_blackhole_dev_init);

int inet_addr_onlink(struct in_device *in_dev, __be32 a, __be32 b)
{
	const struct in_ifaddr *ifa;

	rcu_read_lock();
	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		if (inet_ifa_match(a, ifa)) {
			if (!b || inet_ifa_match(b, ifa)) {
				rcu_read_unlock();
				return 1;
			}
		}
	}
	rcu_read_unlock();
	return 0;
}

static void __inet_del_ifa(struct in_device *in_dev,
			   struct in_ifaddr __rcu **ifap,
			   int destroy, struct nlmsghdr *nlh, u32 portid)
{
	struct in_ifaddr *promote = NULL;
	struct in_ifaddr *ifa, *ifa1;
	struct in_ifaddr __rcu **last_prim;
	struct in_ifaddr *prev_prom = NULL;
	int do_promote = IN_DEV_PROMOTE_SECONDARIES(in_dev);

	ASSERT_RTNL();

	ifa1 = rtnl_dereference(*ifap);
	last_prim = ifap;
	if (in_dev->dead)
		goto no_promotions;

	/* 1. Deleting primary ifaddr forces deletion all secondaries
	 * unless alias promotion is set
	 **/

	if (!(ifa1->ifa_flags & IFA_F_SECONDARY)) {
		struct in_ifaddr __rcu **ifap1 = &ifa1->ifa_next;

		while ((ifa = rtnl_dereference(*ifap1)) != NULL) {
			if (!(ifa->ifa_flags & IFA_F_SECONDARY) &&
			    ifa1->ifa_scope <= ifa->ifa_scope)
				last_prim = &ifa->ifa_next;

			if (!(ifa->ifa_flags & IFA_F_SECONDARY) ||
			    ifa1->ifa_mask != ifa->ifa_mask ||
			    !inet_ifa_match(ifa1->ifa_address, ifa)) {
				ifap1 = &ifa->ifa_next;
				prev_prom = ifa;
				continue;
			}

			if (!do_promote) {
				inet_hash_remove(ifa);
				*ifap1 = ifa->ifa_next;

				rtmsg_ifa(RTM_DELADDR, ifa, nlh, portid);
				blocking_notifier_call_chain(&inetaddr_chain,
						NETDEV_DOWN, ifa);
				inet_free_ifa(ifa);
			} else {
				promote = ifa;
				break;
			}
		}
	}

	/* On promotion all secondaries from subnet are changing
	 * the primary IP, we must remove all their routes silently
	 * and later to add them back with new prefsrc. Do this
	 * while all addresses are on the device list.
	 */
	for (ifa = promote; ifa; ifa = rtnl_dereference(ifa->ifa_next)) {
		if (ifa1->ifa_mask == ifa->ifa_mask &&
		    inet_ifa_match(ifa1->ifa_address, ifa))
			fib_del_ifaddr(ifa, ifa1);
	}

no_promotions:
	/* 2. Unlink it */

	*ifap = ifa1->ifa_next;
	inet_hash_remove(ifa1);

	/* 3. Announce address deletion */

	/* Send message first, then call notifier.
	   At first sight, FIB update triggered by notifier
	   will refer to already deleted ifaddr, that could confuse
	   netlink listeners. It is not true: look, gated sees
	   that route deleted and if it still thinks that ifaddr
	   is valid, it will try to restore deleted routes... Grr.
	   So that, this order is correct.
	 */
	rtmsg_ifa(RTM_DELADDR, ifa1, nlh, portid);
	blocking_notifier_call_chain(&inetaddr_chain, NETDEV_DOWN, ifa1);

	if (promote) {
		struct in_ifaddr *next_sec;

		next_sec = rtnl_dereference(promote->ifa_next);
		if (prev_prom) {
			struct in_ifaddr *last_sec;

			rcu_assign_pointer(prev_prom->ifa_next, next_sec);

			last_sec = rtnl_dereference(*last_prim);
			rcu_assign_pointer(promote->ifa_next, last_sec);
			rcu_assign_pointer(*last_prim, promote);
		}

		promote->ifa_flags &= ~IFA_F_SECONDARY;
		rtmsg_ifa(RTM_NEWADDR, promote, nlh, portid);
		blocking_notifier_call_chain(&inetaddr_chain,
				NETDEV_UP, promote);
		for (ifa = next_sec; ifa;
		     ifa = rtnl_dereference(ifa->ifa_next)) {
			if (ifa1->ifa_mask != ifa->ifa_mask ||
			    !inet_ifa_match(ifa1->ifa_address, ifa))
					continue;
			fib_add_ifaddr(ifa);
		}

	}
	if (destroy)
		inet_free_ifa(ifa1);
}

static void inet_del_ifa(struct in_device *in_dev,
			 struct in_ifaddr __rcu **ifap,
			 int destroy)
{
	__inet_del_ifa(in_dev, ifap, destroy, NULL, 0);
}

static int __inet_insert_ifa(struct in_ifaddr *ifa, struct nlmsghdr *nlh,
			     u32 portid, struct netlink_ext_ack *extack)
{
	struct in_ifaddr __rcu **last_primary, **ifap;
	struct in_device *in_dev = ifa->ifa_dev;
	struct net *net = dev_net(in_dev->dev);
	struct in_validator_info ivi;
	struct in_ifaddr *ifa1;
	int ret;

	ASSERT_RTNL();

	ifa->ifa_flags &= ~IFA_F_SECONDARY;
	last_primary = &in_dev->ifa_list;

	/* Don't set IPv6 only flags to IPv4 addresses */
	ifa->ifa_flags &= ~IPV6ONLY_FLAGS;

	ifap = &in_dev->ifa_list;
	ifa1 = rtnl_dereference(*ifap);

	while (ifa1) {
		if (!(ifa1->ifa_flags & IFA_F_SECONDARY) &&
		    ifa->ifa_scope <= ifa1->ifa_scope)
			last_primary = &ifa1->ifa_next;
		if (ifa1->ifa_mask == ifa->ifa_mask &&
		    inet_ifa_match(ifa1->ifa_address, ifa)) {
			if (ifa1->ifa_local == ifa->ifa_local) {
				inet_free_ifa(ifa);
				return -EEXIST;
			}
			if (ifa1->ifa_scope != ifa->ifa_scope) {
				NL_SET_ERR_MSG(extack, "ipv4: Invalid scope value");
				inet_free_ifa(ifa);
				return -EINVAL;
			}
			ifa->ifa_flags |= IFA_F_SECONDARY;
		}

		ifap = &ifa1->ifa_next;
		ifa1 = rtnl_dereference(*ifap);
	}

	/* Allow any devices that wish to register ifaddr validtors to weigh
	 * in now, before changes are committed.  The rntl lock is serializing
	 * access here, so the state should not change between a validator call
	 * and a final notify on commit.  This isn't invoked on promotion under
	 * the assumption that validators are checking the address itself, and
	 * not the flags.
	 */
	ivi.ivi_addr = ifa->ifa_address;
	ivi.ivi_dev = ifa->ifa_dev;
	ivi.extack = extack;
	ret = blocking_notifier_call_chain(&inetaddr_validator_chain,
					   NETDEV_UP, &ivi);
	ret = notifier_to_errno(ret);
	if (ret) {
		inet_free_ifa(ifa);
		return ret;
	}

	if (!(ifa->ifa_flags & IFA_F_SECONDARY))
		ifap = last_primary;

	rcu_assign_pointer(ifa->ifa_next, *ifap);
	rcu_assign_pointer(*ifap, ifa);

	inet_hash_insert(dev_net(in_dev->dev), ifa);

	cancel_delayed_work(&net->ipv4.addr_chk_work);
	queue_delayed_work(system_power_efficient_wq, &net->ipv4.addr_chk_work, 0);

	/* Send message first, then call notifier.
	   Notifier will trigger FIB update, so that
	   listeners of netlink will know about new ifaddr */
	rtmsg_ifa(RTM_NEWADDR, ifa, nlh, portid);
	blocking_notifier_call_chain(&inetaddr_chain, NETDEV_UP, ifa);

	return 0;
}

static int inet_insert_ifa(struct in_ifaddr *ifa)
{
	if (!ifa->ifa_local) {
		inet_free_ifa(ifa);
		return 0;
	}

	return __inet_insert_ifa(ifa, NULL, 0, NULL);
}

static int inet_set_ifa(struct net_device *dev, struct in_ifaddr *ifa)
{
	struct in_device *in_dev = __in_dev_get_rtnl_net(dev);

	ipv4_devconf_setall(in_dev);
	neigh_parms_data_state_setall(in_dev->arp_parms);

	if (ipv4_is_loopback(ifa->ifa_local))
		ifa->ifa_scope = RT_SCOPE_HOST;
	return inet_insert_ifa(ifa);
}

/* Caller must hold RCU or RTNL :
 * We dont take a reference on found in_device
 */
struct in_device *inetdev_by_index(struct net *net, int ifindex)
{
	struct net_device *dev;
	struct in_device *in_dev = NULL;

	rcu_read_lock();
	dev = dev_get_by_index_rcu(net, ifindex);
	if (dev)
		in_dev = rcu_dereference_rtnl(dev->ip_ptr);
	rcu_read_unlock();
	return in_dev;
}
EXPORT_SYMBOL(inetdev_by_index);

/* Called only from RTNL semaphored context. No locks. */

struct in_ifaddr *inet_ifa_byprefix(struct in_device *in_dev, __be32 prefix,
				    __be32 mask)
{
	struct in_ifaddr *ifa;

	ASSERT_RTNL();

	in_dev_for_each_ifa_rtnl(ifa, in_dev) {
		if (ifa->ifa_mask == mask && inet_ifa_match(prefix, ifa))
			return ifa;
	}
	return NULL;
}

static int ip_mc_autojoin_config(struct net *net, bool join,
				 const struct in_ifaddr *ifa)
{
#if defined(CONFIG_IP_MULTICAST)
	struct ip_mreqn mreq = {
		.imr_multiaddr.s_addr = ifa->ifa_address,
		.imr_ifindex = ifa->ifa_dev->dev->ifindex,
	};
	struct sock *sk = net->ipv4.mc_autojoin_sk;
	int ret;

	ASSERT_RTNL_NET(net);

	lock_sock(sk);
	if (join)
		ret = ip_mc_join_group(sk, &mreq);
	else
		ret = ip_mc_leave_group(sk, &mreq);
	release_sock(sk);

	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

static int inet_rtm_deladdr(struct sk_buff *skb, struct nlmsghdr *nlh,
			    struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(skb->sk);
	struct in_ifaddr __rcu **ifap;
	struct nlattr *tb[IFA_MAX+1];
	struct in_device *in_dev;
	struct ifaddrmsg *ifm;
	struct in_ifaddr *ifa;
	int err;

	err = nlmsg_parse_deprecated(nlh, sizeof(*ifm), tb, IFA_MAX,
				     ifa_ipv4_policy, extack);
	if (err < 0)
		goto out;

	ifm = nlmsg_data(nlh);

	rtnl_net_lock(net);

	in_dev = inetdev_by_index(net, ifm->ifa_index);
	if (!in_dev) {
		NL_SET_ERR_MSG(extack, "ipv4: Device not found");
		err = -ENODEV;
		goto unlock;
	}

	for (ifap = &in_dev->ifa_list;
	     (ifa = rtnl_net_dereference(net, *ifap)) != NULL;
	     ifap = &ifa->ifa_next) {
		if (tb[IFA_LOCAL] &&
		    ifa->ifa_local != nla_get_in_addr(tb[IFA_LOCAL]))
			continue;

		if (tb[IFA_LABEL] && nla_strcmp(tb[IFA_LABEL], ifa->ifa_label))
			continue;

		if (tb[IFA_ADDRESS] &&
		    (ifm->ifa_prefixlen != ifa->ifa_prefixlen ||
		    !inet_ifa_match(nla_get_in_addr(tb[IFA_ADDRESS]), ifa)))
			continue;

		if (ipv4_is_multicast(ifa->ifa_address))
			ip_mc_autojoin_config(net, false, ifa);

		__inet_del_ifa(in_dev, ifap, 1, nlh, NETLINK_CB(skb).portid);
		goto unlock;
	}

	NL_SET_ERR_MSG(extack, "ipv4: Address not found");
	err = -EADDRNOTAVAIL;
unlock:
	rtnl_net_unlock(net);
out:
	return err;
}

static void check_lifetime(struct work_struct *work)
{
	unsigned long now, next, next_sec, next_sched;
	struct in_ifaddr *ifa;
	struct hlist_node *n;
	struct net *net;
	int i;

	net = container_of(to_delayed_work(work), struct net, ipv4.addr_chk_work);
	now = jiffies;
	next = round_jiffies_up(now + ADDR_CHECK_FREQUENCY);

	for (i = 0; i < IN4_ADDR_HSIZE; i++) {
		struct hlist_head *head = &net->ipv4.inet_addr_lst[i];
		bool change_needed = false;

		rcu_read_lock();
		hlist_for_each_entry_rcu(ifa, head, addr_lst) {
			unsigned long age, tstamp;
			u32 preferred_lft;
			u32 valid_lft;
			u32 flags;

			flags = READ_ONCE(ifa->ifa_flags);
			if (flags & IFA_F_PERMANENT)
				continue;

			preferred_lft = READ_ONCE(ifa->ifa_preferred_lft);
			valid_lft = READ_ONCE(ifa->ifa_valid_lft);
			tstamp = READ_ONCE(ifa->ifa_tstamp);
			/* We try to batch several events at once. */
			age = (now - tstamp +
			       ADDRCONF_TIMER_FUZZ_MINUS) / HZ;

			if (valid_lft != INFINITY_LIFE_TIME &&
			    age >= valid_lft) {
				change_needed = true;
			} else if (preferred_lft ==
				   INFINITY_LIFE_TIME) {
				continue;
			} else if (age >= preferred_lft) {
				if (time_before(tstamp + valid_lft * HZ, next))
					next = tstamp + valid_lft * HZ;

				if (!(flags & IFA_F_DEPRECATED))
					change_needed = true;
			} else if (time_before(tstamp + preferred_lft * HZ,
					       next)) {
				next = tstamp + preferred_lft * HZ;
			}
		}
		rcu_read_unlock();
		if (!change_needed)
			continue;

		rtnl_net_lock(net);
		hlist_for_each_entry_safe(ifa, n, head, addr_lst) {
			unsigned long age;

			if (ifa->ifa_flags & IFA_F_PERMANENT)
				continue;

			/* We try to batch several events at once. */
			age = (now - ifa->ifa_tstamp +
			       ADDRCONF_TIMER_FUZZ_MINUS) / HZ;

			if (ifa->ifa_valid_lft != INFINITY_LIFE_TIME &&
			    age >= ifa->ifa_valid_lft) {
				struct in_ifaddr __rcu **ifap;
				struct in_ifaddr *tmp;

				ifap = &ifa->ifa_dev->ifa_list;
				tmp = rtnl_net_dereference(net, *ifap);
				while (tmp) {
					if (tmp == ifa) {
						inet_del_ifa(ifa->ifa_dev,
							     ifap, 1);
						break;
					}
					ifap = &tmp->ifa_next;
					tmp = rtnl_net_dereference(net, *ifap);
				}
			} else if (ifa->ifa_preferred_lft !=
				   INFINITY_LIFE_TIME &&
				   age >= ifa->ifa_preferred_lft &&
				   !(ifa->ifa_flags & IFA_F_DEPRECATED)) {
				ifa->ifa_flags |= IFA_F_DEPRECATED;
				rtmsg_ifa(RTM_NEWADDR, ifa, NULL, 0);
			}
		}
		rtnl_net_unlock(net);
	}

	next_sec = round_jiffies_up(next);
	next_sched = next;

	/* If rounded timeout is accurate enough, accept it. */
	if (time_before(next_sec, next + ADDRCONF_TIMER_FUZZ))
		next_sched = next_sec;

	now = jiffies;
	/* And minimum interval is ADDRCONF_TIMER_FUZZ_MAX. */
	if (time_before(next_sched, now + ADDRCONF_TIMER_FUZZ_MAX))
		next_sched = now + ADDRCONF_TIMER_FUZZ_MAX;

	queue_delayed_work(system_power_efficient_wq, &net->ipv4.addr_chk_work,
			   next_sched - now);
}

static void set_ifa_lifetime(struct in_ifaddr *ifa, __u32 valid_lft,
			     __u32 prefered_lft)
{
	unsigned long timeout;
	u32 flags;

	flags = ifa->ifa_flags & ~(IFA_F_PERMANENT | IFA_F_DEPRECATED);

	timeout = addrconf_timeout_fixup(valid_lft, HZ);
	if (addrconf_finite_timeout(timeout))
		WRITE_ONCE(ifa->ifa_valid_lft, timeout);
	else
		flags |= IFA_F_PERMANENT;

	timeout = addrconf_timeout_fixup(prefered_lft, HZ);
	if (addrconf_finite_timeout(timeout)) {
		if (timeout == 0)
			flags |= IFA_F_DEPRECATED;
		WRITE_ONCE(ifa->ifa_preferred_lft, timeout);
	}
	WRITE_ONCE(ifa->ifa_flags, flags);
	WRITE_ONCE(ifa->ifa_tstamp, jiffies);
	if (!ifa->ifa_cstamp)
		WRITE_ONCE(ifa->ifa_cstamp, ifa->ifa_tstamp);
}

static int inet_validate_rtm(struct nlmsghdr *nlh, struct nlattr **tb,
			     struct netlink_ext_ack *extack,
			     __u32 *valid_lft, __u32 *prefered_lft)
{
	struct ifaddrmsg *ifm = nlmsg_data(nlh);
	int err;

	err = nlmsg_parse_deprecated(nlh, sizeof(*ifm), tb, IFA_MAX,
				     ifa_ipv4_policy, extack);
	if (err < 0)
		return err;

	if (ifm->ifa_prefixlen > 32) {
		NL_SET_ERR_MSG(extack, "ipv4: Invalid prefix length");
		return -EINVAL;
	}

	if (!tb[IFA_LOCAL]) {
		NL_SET_ERR_MSG(extack, "ipv4: Local address is not supplied");
		return -EINVAL;
	}

	if (tb[IFA_CACHEINFO]) {
		struct ifa_cacheinfo *ci;

		ci = nla_data(tb[IFA_CACHEINFO]);
		if (!ci->ifa_valid || ci->ifa_prefered > ci->ifa_valid) {
			NL_SET_ERR_MSG(extack, "ipv4: address lifetime invalid");
			return -EINVAL;
		}

		*valid_lft = ci->ifa_valid;
		*prefered_lft = ci->ifa_prefered;
	}

	return 0;
}

static struct in_ifaddr *inet_rtm_to_ifa(struct net *net, struct nlmsghdr *nlh,
					 struct nlattr **tb,
					 struct netlink_ext_ack *extack)
{
	struct ifaddrmsg *ifm = nlmsg_data(nlh);
	struct in_device *in_dev;
	struct net_device *dev;
	struct in_ifaddr *ifa;
	int err;

	dev = __dev_get_by_index(net, ifm->ifa_index);
	err = -ENODEV;
	if (!dev) {
		NL_SET_ERR_MSG(extack, "ipv4: Device not found");
		goto errout;
	}

	in_dev = __in_dev_get_rtnl_net(dev);
	err = -ENOBUFS;
	if (!in_dev)
		goto errout;

	ifa = inet_alloc_ifa(in_dev);
	if (!ifa)
		/*
		 * A potential indev allocation can be left alive, it stays
		 * assigned to its device and is destroy with it.
		 */
		goto errout;

	ipv4_devconf_setall(in_dev);
	neigh_parms_data_state_setall(in_dev->arp_parms);

	if (!tb[IFA_ADDRESS])
		tb[IFA_ADDRESS] = tb[IFA_LOCAL];

	ifa->ifa_prefixlen = ifm->ifa_prefixlen;
	ifa->ifa_mask = inet_make_mask(ifm->ifa_prefixlen);
	ifa->ifa_flags = nla_get_u32_default(tb[IFA_FLAGS], ifm->ifa_flags);
	ifa->ifa_scope = ifm->ifa_scope;
	ifa->ifa_local = nla_get_in_addr(tb[IFA_LOCAL]);
	ifa->ifa_address = nla_get_in_addr(tb[IFA_ADDRESS]);

	if (tb[IFA_BROADCAST])
		ifa->ifa_broadcast = nla_get_in_addr(tb[IFA_BROADCAST]);

	if (tb[IFA_LABEL])
		nla_strscpy(ifa->ifa_label, tb[IFA_LABEL], IFNAMSIZ);
	else
		memcpy(ifa->ifa_label, dev->name, IFNAMSIZ);

	if (tb[IFA_RT_PRIORITY])
		ifa->ifa_rt_priority = nla_get_u32(tb[IFA_RT_PRIORITY]);

	if (tb[IFA_PROTO])
		ifa->ifa_proto = nla_get_u8(tb[IFA_PROTO]);

	return ifa;

errout:
	return ERR_PTR(err);
}

static struct in_ifaddr *find_matching_ifa(struct net *net, struct in_ifaddr *ifa)
{
	struct in_device *in_dev = ifa->ifa_dev;
	struct in_ifaddr *ifa1;

	in_dev_for_each_ifa_rtnl_net(net, ifa1, in_dev) {
		if (ifa1->ifa_mask == ifa->ifa_mask &&
		    inet_ifa_match(ifa1->ifa_address, ifa) &&
		    ifa1->ifa_local == ifa->ifa_local)
			return ifa1;
	}

	return NULL;
}

static int inet_rtm_newaddr(struct sk_buff *skb, struct nlmsghdr *nlh,
			    struct netlink_ext_ack *extack)
{
	__u32 prefered_lft = INFINITY_LIFE_TIME;
	__u32 valid_lft = INFINITY_LIFE_TIME;
	struct net *net = sock_net(skb->sk);
	struct in_ifaddr *ifa_existing;
	struct nlattr *tb[IFA_MAX + 1];
	struct in_ifaddr *ifa;
	int ret;

	ret = inet_validate_rtm(nlh, tb, extack, &valid_lft, &prefered_lft);
	if (ret < 0)
		return ret;

	if (!nla_get_in_addr(tb[IFA_LOCAL]))
		return 0;

	rtnl_net_lock(net);

	ifa = inet_rtm_to_ifa(net, nlh, tb, extack);
	if (IS_ERR(ifa)) {
		ret = PTR_ERR(ifa);
		goto unlock;
	}

	ifa_existing = find_matching_ifa(net, ifa);
	if (!ifa_existing) {
		/* It would be best to check for !NLM_F_CREATE here but
		 * userspace already relies on not having to provide this.
		 */
		set_ifa_lifetime(ifa, valid_lft, prefered_lft);
		if (ifa->ifa_flags & IFA_F_MCAUTOJOIN) {
			ret = ip_mc_autojoin_config(net, true, ifa);
			if (ret < 0) {
				NL_SET_ERR_MSG(extack, "ipv4: Multicast auto join failed");
				inet_free_ifa(ifa);
				goto unlock;
			}
		}

		ret = __inet_insert_ifa(ifa, nlh, NETLINK_CB(skb).portid, extack);
	} else {
		u32 new_metric = ifa->ifa_rt_priority;
		u8 new_proto = ifa->ifa_proto;

		inet_free_ifa(ifa);

		if (nlh->nlmsg_flags & NLM_F_EXCL ||
		    !(nlh->nlmsg_flags & NLM_F_REPLACE)) {
			NL_SET_ERR_MSG(extack, "ipv4: Address already assigned");
			ret = -EEXIST;
			goto unlock;
		}
		ifa = ifa_existing;

		if (ifa->ifa_rt_priority != new_metric) {
			fib_modify_prefix_metric(ifa, new_metric);
			ifa->ifa_rt_priority = new_metric;
		}

		ifa->ifa_proto = new_proto;

		set_ifa_lifetime(ifa, valid_lft, prefered_lft);
		cancel_delayed_work(&net->ipv4.addr_chk_work);
		queue_delayed_work(system_power_efficient_wq,
				   &net->ipv4.addr_chk_work, 0);
		rtmsg_ifa(RTM_NEWADDR, ifa, nlh, NETLINK_CB(skb).portid);
	}

unlock:
	rtnl_net_unlock(net);

	return ret;
}

/*
 *	Determine a default network mask, based on the IP address.
 */

static int inet_abc_len(__be32 addr)
{
	int rc = -1;	/* Something else, probably a multicast. */

	if (ipv4_is_zeronet(addr) || ipv4_is_lbcast(addr))
		rc = 0;
	else {
		__u32 haddr = ntohl(addr);
		if (IN_CLASSA(haddr))
			rc = 8;
		else if (IN_CLASSB(haddr))
			rc = 16;
		else if (IN_CLASSC(haddr))
			rc = 24;
		else if (IN_CLASSE(haddr))
			rc = 32;
	}

	return rc;
}


int devinet_ioctl(struct net *net, unsigned int cmd, struct ifreq *ifr)
{
	struct sockaddr_in sin_orig;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
	struct in_ifaddr __rcu **ifap = NULL;
	struct in_device *in_dev;
	struct in_ifaddr *ifa = NULL;
	struct net_device *dev;
	char *colon;
	int ret = -EFAULT;
	int tryaddrmatch = 0;

	ifr->ifr_name[IFNAMSIZ - 1] = 0;

	/* save original address for comparison */
	memcpy(&sin_orig, sin, sizeof(*sin));

	colon = strchr(ifr->ifr_name, ':');
	if (colon)
		*colon = 0;

	dev_load(net, ifr->ifr_name);

	switch (cmd) {
	case SIOCGIFADDR:	/* Get interface address */
	case SIOCGIFBRDADDR:	/* Get the broadcast address */
	case SIOCGIFDSTADDR:	/* Get the destination address */
	case SIOCGIFNETMASK:	/* Get the netmask for the interface */
		/* Note that these ioctls will not sleep,
		   so that we do not impose a lock.
		   One day we will be forced to put shlock here (I mean SMP)
		 */
		tryaddrmatch = (sin_orig.sin_family == AF_INET);
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		break;

	case SIOCSIFFLAGS:
		ret = -EPERM;
		if (!ns_capable(net->user_ns, CAP_NET_ADMIN))
			goto out;
		break;
	case SIOCSIFADDR:	/* Set interface address (and family) */
	case SIOCSIFBRDADDR:	/* Set the broadcast address */
	case SIOCSIFDSTADDR:	/* Set the destination address */
	case SIOCSIFNETMASK: 	/* Set the netmask for the interface */
		ret = -EPERM;
		if (!ns_capable(net->user_ns, CAP_NET_ADMIN))
			goto out;
		ret = -EINVAL;
		if (sin->sin_family != AF_INET)
			goto out;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	rtnl_net_lock(net);

	ret = -ENODEV;
	dev = __dev_get_by_name(net, ifr->ifr_name);
	if (!dev)
		goto done;

	if (colon)
		*colon = ':';

	in_dev = __in_dev_get_rtnl_net(dev);
	if (in_dev) {
		if (tryaddrmatch) {
			/* Matthias Andree */
			/* compare label and address (4.4BSD style) */
			/* note: we only do this for a limited set of ioctls
			   and only if the original address family was AF_INET.
			   This is checked above. */

			for (ifap = &in_dev->ifa_list;
			     (ifa = rtnl_net_dereference(net, *ifap)) != NULL;
			     ifap = &ifa->ifa_next) {
				if (!strcmp(ifr->ifr_name, ifa->ifa_label) &&
				    sin_orig.sin_addr.s_addr ==
							ifa->ifa_local) {
					break; /* found */
				}
			}
		}
		/* we didn't get a match, maybe the application is
		   4.3BSD-style and passed in junk so we fall back to
		   comparing just the label */
		if (!ifa) {
			for (ifap = &in_dev->ifa_list;
			     (ifa = rtnl_net_dereference(net, *ifap)) != NULL;
			     ifap = &ifa->ifa_next)
				if (!strcmp(ifr->ifr_name, ifa->ifa_label))
					break;
		}
	}

	ret = -EADDRNOTAVAIL;
	if (!ifa && cmd != SIOCSIFADDR && cmd != SIOCSIFFLAGS)
		goto done;

	switch (cmd) {
	case SIOCGIFADDR:	/* Get interface address */
		ret = 0;
		sin->sin_addr.s_addr = ifa->ifa_local;
		break;

	case SIOCGIFBRDADDR:	/* Get the broadcast address */
		ret = 0;
		sin->sin_addr.s_addr = ifa->ifa_broadcast;
		break;

	case SIOCGIFDSTADDR:	/* Get the destination address */
		ret = 0;
		sin->sin_addr.s_addr = ifa->ifa_address;
		break;

	case SIOCGIFNETMASK:	/* Get the netmask for the interface */
		ret = 0;
		sin->sin_addr.s_addr = ifa->ifa_mask;
		break;

	case SIOCSIFFLAGS:
		if (colon) {
			ret = -EADDRNOTAVAIL;
			if (!ifa)
				break;
			ret = 0;
			if (!(ifr->ifr_flags & IFF_UP))
				inet_del_ifa(in_dev, ifap, 1);
			break;
		}

		/* NETDEV_UP/DOWN/CHANGE could touch a peer dev */
		ASSERT_RTNL();
		ret = dev_change_flags(dev, ifr->ifr_flags, NULL);
		break;

	case SIOCSIFADDR:	/* Set interface address (and family) */
		ret = -EINVAL;
		if (inet_abc_len(sin->sin_addr.s_addr) < 0)
			break;

		if (!ifa) {
			ret = -ENOBUFS;
			if (!in_dev)
				break;
			ifa = inet_alloc_ifa(in_dev);
			if (!ifa)
				break;

			if (colon)
				memcpy(ifa->ifa_label, ifr->ifr_name, IFNAMSIZ);
			else
				memcpy(ifa->ifa_label, dev->name, IFNAMSIZ);
		} else {
			ret = 0;
			if (ifa->ifa_local == sin->sin_addr.s_addr)
				break;
			inet_del_ifa(in_dev, ifap, 0);
			ifa->ifa_broadcast = 0;
			ifa->ifa_scope = 0;
		}

		ifa->ifa_address = ifa->ifa_local = sin->sin_addr.s_addr;

		if (!(dev->flags & IFF_POINTOPOINT)) {
			ifa->ifa_prefixlen = inet_abc_len(ifa->ifa_address);
			ifa->ifa_mask = inet_make_mask(ifa->ifa_prefixlen);
			if ((dev->flags & IFF_BROADCAST) &&
			    ifa->ifa_prefixlen < 31)
				ifa->ifa_broadcast = ifa->ifa_address |
						     ~ifa->ifa_mask;
		} else {
			ifa->ifa_prefixlen = 32;
			ifa->ifa_mask = inet_make_mask(32);
		}
		set_ifa_lifetime(ifa, INFINITY_LIFE_TIME, INFINITY_LIFE_TIME);
		ret = inet_set_ifa(dev, ifa);
		break;

	case SIOCSIFBRDADDR:	/* Set the broadcast address */
		ret = 0;
		if (ifa->ifa_broadcast != sin->sin_addr.s_addr) {
			inet_del_ifa(in_dev, ifap, 0);
			ifa->ifa_broadcast = sin->sin_addr.s_addr;
			inet_insert_ifa(ifa);
		}
		break;

	case SIOCSIFDSTADDR:	/* Set the destination address */
		ret = 0;
		if (ifa->ifa_address == sin->sin_addr.s_addr)
			break;
		ret = -EINVAL;
		if (inet_abc_len(sin->sin_addr.s_addr) < 0)
			break;
		ret = 0;
		inet_del_ifa(in_dev, ifap, 0);
		ifa->ifa_address = sin->sin_addr.s_addr;
		inet_insert_ifa(ifa);
		break;

	case SIOCSIFNETMASK: 	/* Set the netmask for the interface */

		/*
		 *	The mask we set must be legal.
		 */
		ret = -EINVAL;
		if (bad_mask(sin->sin_addr.s_addr, 0))
			break;
		ret = 0;
		if (ifa->ifa_mask != sin->sin_addr.s_addr) {
			__be32 old_mask = ifa->ifa_mask;
			inet_del_ifa(in_dev, ifap, 0);
			ifa->ifa_mask = sin->sin_addr.s_addr;
			ifa->ifa_prefixlen = inet_mask_len(ifa->ifa_mask);

			/* See if current broadcast address matches
			 * with current netmask, then recalculate
			 * the broadcast address. Otherwise it's a
			 * funny address, so don't touch it since
			 * the user seems to know what (s)he's doing...
			 */
			if ((dev->flags & IFF_BROADCAST) &&
			    (ifa->ifa_prefixlen < 31) &&
			    (ifa->ifa_broadcast ==
			     (ifa->ifa_local|~old_mask))) {
				ifa->ifa_broadcast = (ifa->ifa_local |
						      ~sin->sin_addr.s_addr);
			}
			inet_insert_ifa(ifa);
		}
		break;
	}
done:
	rtnl_net_unlock(net);
out:
	return ret;
}

int inet_gifconf(struct net_device *dev, char __user *buf, int len, int size)
{
	struct in_device *in_dev = __in_dev_get_rtnl_net(dev);
	const struct in_ifaddr *ifa;
	struct ifreq ifr;
	int done = 0;

	if (WARN_ON(size > sizeof(struct ifreq)))
		goto out;

	if (!in_dev)
		goto out;

	in_dev_for_each_ifa_rtnl_net(dev_net(dev), ifa, in_dev) {
		if (!buf) {
			done += size;
			continue;
		}
		if (len < size)
			break;
		memset(&ifr, 0, sizeof(struct ifreq));
		strcpy(ifr.ifr_name, ifa->ifa_label);

		(*(struct sockaddr_in *)&ifr.ifr_addr).sin_family = AF_INET;
		(*(struct sockaddr_in *)&ifr.ifr_addr).sin_addr.s_addr =
								ifa->ifa_local;

		if (copy_to_user(buf + done, &ifr, size)) {
			done = -EFAULT;
			break;
		}
		len  -= size;
		done += size;
	}
out:
	return done;
}

static __be32 in_dev_select_addr(const struct in_device *in_dev,
				 int scope)
{
	const struct in_ifaddr *ifa;

	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		if (READ_ONCE(ifa->ifa_flags) & IFA_F_SECONDARY)
			continue;
		if (ifa->ifa_scope != RT_SCOPE_LINK &&
		    ifa->ifa_scope <= scope)
			return ifa->ifa_local;
	}

	return 0;
}

__be32 inet_select_addr(const struct net_device *dev, __be32 dst, int scope)
{
	const struct in_ifaddr *ifa;
	__be32 addr = 0;
	unsigned char localnet_scope = RT_SCOPE_HOST;
	struct in_device *in_dev;
	struct net *net;
	int master_idx;

	rcu_read_lock();
	net = dev_net_rcu(dev);
	in_dev = __in_dev_get_rcu(dev);
	if (!in_dev)
		goto no_in_dev;

	if (unlikely(IN_DEV_ROUTE_LOCALNET(in_dev)))
		localnet_scope = RT_SCOPE_LINK;

	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		if (READ_ONCE(ifa->ifa_flags) & IFA_F_SECONDARY)
			continue;
		if (min(ifa->ifa_scope, localnet_scope) > scope)
			continue;
		if (!dst || inet_ifa_match(dst, ifa)) {
			addr = ifa->ifa_local;
			break;
		}
		if (!addr)
			addr = ifa->ifa_local;
	}

	if (addr)
		goto out_unlock;
no_in_dev:
	master_idx = l3mdev_master_ifindex_rcu(dev);

	/* For VRFs, the VRF device takes the place of the loopback device,
	 * with addresses on it being preferred.  Note in such cases the
	 * loopback device will be among the devices that fail the master_idx
	 * equality check in the loop below.
	 */
	if (master_idx &&
	    (dev = dev_get_by_index_rcu(net, master_idx)) &&
	    (in_dev = __in_dev_get_rcu(dev))) {
		addr = in_dev_select_addr(in_dev, scope);
		if (addr)
			goto out_unlock;
	}

	/* Not loopback addresses on loopback should be preferred
	   in this case. It is important that lo is the first interface
	   in dev_base list.
	 */
	for_each_netdev_rcu(net, dev) {
		if (l3mdev_master_ifindex_rcu(dev) != master_idx)
			continue;

		in_dev = __in_dev_get_rcu(dev);
		if (!in_dev)
			continue;

		addr = in_dev_select_addr(in_dev, scope);
		if (addr)
			goto out_unlock;
	}
out_unlock:
	rcu_read_unlock();
	return addr;
}
EXPORT_SYMBOL(inet_select_addr);

static __be32 confirm_addr_indev(struct in_device *in_dev, __be32 dst,
			      __be32 local, int scope)
{
	unsigned char localnet_scope = RT_SCOPE_HOST;
	const struct in_ifaddr *ifa;
	__be32 addr = 0;
	int same = 0;

	if (unlikely(IN_DEV_ROUTE_LOCALNET(in_dev)))
		localnet_scope = RT_SCOPE_LINK;

	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		unsigned char min_scope = min(ifa->ifa_scope, localnet_scope);

		if (!addr &&
		    (local == ifa->ifa_local || !local) &&
		    min_scope <= scope) {
			addr = ifa->ifa_local;
			if (same)
				break;
		}
		if (!same) {
			same = (!local || inet_ifa_match(local, ifa)) &&
				(!dst || inet_ifa_match(dst, ifa));
			if (same && addr) {
				if (local || !dst)
					break;
				/* Is the selected addr into dst subnet? */
				if (inet_ifa_match(addr, ifa))
					break;
				/* No, then can we use new local src? */
				if (min_scope <= scope) {
					addr = ifa->ifa_local;
					break;
				}
				/* search for large dst subnet for addr */
				same = 0;
			}
		}
	}

	return same ? addr : 0;
}

/*
 * Confirm that local IP address exists using wildcards:
 * - net: netns to check, cannot be NULL
 * - in_dev: only on this interface, NULL=any interface
 * - dst: only in the same subnet as dst, 0=any dst
 * - local: address, 0=autoselect the local address
 * - scope: maximum allowed scope value for the local address
 */
__be32 inet_confirm_addr(struct net *net, struct in_device *in_dev,
			 __be32 dst, __be32 local, int scope)
{
	__be32 addr = 0;
	struct net_device *dev;

	if (in_dev)
		return confirm_addr_indev(in_dev, dst, local, scope);

	rcu_read_lock();
	for_each_netdev_rcu(net, dev) {
		in_dev = __in_dev_get_rcu(dev);
		if (in_dev) {
			addr = confirm_addr_indev(in_dev, dst, local, scope);
			if (addr)
				break;
		}
	}
	rcu_read_unlock();

	return addr;
}
EXPORT_SYMBOL(inet_confirm_addr);

/*
 *	Device notifier
 */

int register_inetaddr_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&inetaddr_chain, nb);
}
EXPORT_SYMBOL(register_inetaddr_notifier);

int unregister_inetaddr_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&inetaddr_chain, nb);
}
EXPORT_SYMBOL(unregister_inetaddr_notifier);

int register_inetaddr_validator_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&inetaddr_validator_chain, nb);
}
EXPORT_SYMBOL(register_inetaddr_validator_notifier);

int unregister_inetaddr_validator_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&inetaddr_validator_chain,
	    nb);
}
EXPORT_SYMBOL(unregister_inetaddr_validator_notifier);

/* Rename ifa_labels for a device name change. Make some effort to preserve
 * existing alias numbering and to create unique labels if possible.
*/
static void inetdev_changename(struct net_device *dev, struct in_device *in_dev)
{
	struct in_ifaddr *ifa;
	int named = 0;

	in_dev_for_each_ifa_rtnl(ifa, in_dev) {
		char old[IFNAMSIZ], *dot;

		memcpy(old, ifa->ifa_label, IFNAMSIZ);
		memcpy(ifa->ifa_label, dev->name, IFNAMSIZ);
		if (named++ == 0)
			goto skip;
		dot = strchr(old, ':');
		if (!dot) {
			sprintf(old, ":%d", named);
			dot = old;
		}
		if (strlen(dot) + strlen(dev->name) < IFNAMSIZ)
			strcat(ifa->ifa_label, dot);
		else
			strcpy(ifa->ifa_label + (IFNAMSIZ - strlen(dot) - 1), dot);
skip:
		rtmsg_ifa(RTM_NEWADDR, ifa, NULL, 0);
	}
}

static void inetdev_send_gratuitous_arp(struct net_device *dev,
					struct in_device *in_dev)

{
	const struct in_ifaddr *ifa;

	in_dev_for_each_ifa_rtnl(ifa, in_dev) {
		arp_send(ARPOP_REQUEST, ETH_P_ARP,
			 ifa->ifa_local, dev,
			 ifa->ifa_local, NULL,
			 dev->dev_addr, NULL);
	}
}

/* Called only under RTNL semaphore */

static int inetdev_event(struct notifier_block *this, unsigned long event,
			 void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct in_device *in_dev = __in_dev_get_rtnl(dev);

	ASSERT_RTNL();

	if (!in_dev) {
		if (event == NETDEV_REGISTER) {
			in_dev = inetdev_init(dev);
			if (IS_ERR(in_dev))
				return notifier_from_errno(PTR_ERR(in_dev));
			if (dev->flags & IFF_LOOPBACK) {
				IN_DEV_CONF_SET(in_dev, NOXFRM, 1);
				IN_DEV_CONF_SET(in_dev, NOPOLICY, 1);
			}
		} else if (event == NETDEV_CHANGEMTU) {
			/* Re-enabling IP */
			if (inetdev_valid_mtu(dev->mtu))
				in_dev = inetdev_init(dev);
		}
		goto out;
	}

	switch (event) {
	case NETDEV_REGISTER:
		pr_debug("%s: bug\n", __func__);
		RCU_INIT_POINTER(dev->ip_ptr, NULL);
		break;
	case NETDEV_UP:
		if (!inetdev_valid_mtu(dev->mtu))
			break;
		if (dev->flags & IFF_LOOPBACK) {
			struct in_ifaddr *ifa = inet_alloc_ifa(in_dev);

			if (ifa) {
				ifa->ifa_local =
				  ifa->ifa_address = htonl(INADDR_LOOPBACK);
				ifa->ifa_prefixlen = 8;
				ifa->ifa_mask = inet_make_mask(8);
				ifa->ifa_scope = RT_SCOPE_HOST;
				memcpy(ifa->ifa_label, dev->name, IFNAMSIZ);
				set_ifa_lifetime(ifa, INFINITY_LIFE_TIME,
						 INFINITY_LIFE_TIME);
				ipv4_devconf_setall(in_dev);
				neigh_parms_data_state_setall(in_dev->arp_parms);
				inet_insert_ifa(ifa);
			}
		}
		ip_mc_up(in_dev);
		fallthrough;
	case NETDEV_CHANGEADDR:
		if (!IN_DEV_ARP_NOTIFY(in_dev))
			break;
		fallthrough;
	case NETDEV_NOTIFY_PEERS:
		/* Send gratuitous ARP to notify of link change */
		inetdev_send_gratuitous_arp(dev, in_dev);
		break;
	case NETDEV_DOWN:
		ip_mc_down(in_dev);
		break;
	case NETDEV_PRE_TYPE_CHANGE:
		ip_mc_unmap(in_dev);
		break;
	case NETDEV_POST_TYPE_CHANGE:
		ip_mc_remap(in_dev);
		break;
	case NETDEV_CHANGEMTU:
		if (inetdev_valid_mtu(dev->mtu))
			break;
		/* disable IP when MTU is not enough */
		fallthrough;
	case NETDEV_UNREGISTER:
		inetdev_destroy(in_dev);
		break;
	case NETDEV_CHANGENAME:
		/* Do not notify about label change, this event is
		 * not interesting to applications using netlink.
		 */
		inetdev_changename(dev, in_dev);

		devinet_sysctl_unregister(in_dev);
		devinet_sysctl_register(in_dev);
		break;
	}
out:
	return NOTIFY_DONE;
}

static struct notifier_block ip_netdev_notifier = {
	.notifier_call = inetdev_event,
};

static size_t inet_nlmsg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct ifaddrmsg))
	       + nla_total_size(4) /* IFA_ADDRESS */
	       + nla_total_size(4) /* IFA_LOCAL */
	       + nla_total_size(4) /* IFA_BROADCAST */
	       + nla_total_size(IFNAMSIZ) /* IFA_LABEL */
	       + nla_total_size(4)  /* IFA_FLAGS */
	       + nla_total_size(1)  /* IFA_PROTO */
	       + nla_total_size(4)  /* IFA_RT_PRIORITY */
	       + nla_total_size(sizeof(struct ifa_cacheinfo)); /* IFA_CACHEINFO */
}

static inline u32 cstamp_delta(unsigned long cstamp)
{
	return (cstamp - INITIAL_JIFFIES) * 100UL / HZ;
}

static int put_cacheinfo(struct sk_buff *skb, unsigned long cstamp,
			 unsigned long tstamp, u32 preferred, u32 valid)
{
	struct ifa_cacheinfo ci;

	ci.cstamp = cstamp_delta(cstamp);
	ci.tstamp = cstamp_delta(tstamp);
	ci.ifa_prefered = preferred;
	ci.ifa_valid = valid;

	return nla_put(skb, IFA_CACHEINFO, sizeof(ci), &ci);
}

static int inet_fill_ifaddr(struct sk_buff *skb, const struct in_ifaddr *ifa,
			    struct inet_fill_args *args)
{
	struct ifaddrmsg *ifm;
	struct nlmsghdr  *nlh;
	unsigned long tstamp;
	u32 preferred, valid;
	u32 flags;

	nlh = nlmsg_put(skb, args->portid, args->seq, args->event, sizeof(*ifm),
			args->flags);
	if (!nlh)
		return -EMSGSIZE;

	ifm = nlmsg_data(nlh);
	ifm->ifa_family = AF_INET;
	ifm->ifa_prefixlen = ifa->ifa_prefixlen;

	flags = READ_ONCE(ifa->ifa_flags);
	/* Warning : ifm->ifa_flags is an __u8, it holds only 8 bits.
	 * The 32bit value is given in IFA_FLAGS attribute.
	 */
	ifm->ifa_flags = (__u8)flags;

	ifm->ifa_scope = ifa->ifa_scope;
	ifm->ifa_index = ifa->ifa_dev->dev->ifindex;

	if (args->netnsid >= 0 &&
	    nla_put_s32(skb, IFA_TARGET_NETNSID, args->netnsid))
		goto nla_put_failure;

	tstamp = READ_ONCE(ifa->ifa_tstamp);
	if (!(flags & IFA_F_PERMANENT)) {
		preferred = READ_ONCE(ifa->ifa_preferred_lft);
		valid = READ_ONCE(ifa->ifa_valid_lft);
		if (preferred != INFINITY_LIFE_TIME) {
			long tval = (jiffies - tstamp) / HZ;

			if (preferred > tval)
				preferred -= tval;
			else
				preferred = 0;
			if (valid != INFINITY_LIFE_TIME) {
				if (valid > tval)
					valid -= tval;
				else
					valid = 0;
			}
		}
	} else {
		preferred = INFINITY_LIFE_TIME;
		valid = INFINITY_LIFE_TIME;
	}
	if ((ifa->ifa_address &&
	     nla_put_in_addr(skb, IFA_ADDRESS, ifa->ifa_address)) ||
	    (ifa->ifa_local &&
	     nla_put_in_addr(skb, IFA_LOCAL, ifa->ifa_local)) ||
	    (ifa->ifa_broadcast &&
	     nla_put_in_addr(skb, IFA_BROADCAST, ifa->ifa_broadcast)) ||
	    (ifa->ifa_label[0] &&
	     nla_put_string(skb, IFA_LABEL, ifa->ifa_label)) ||
	    (ifa->ifa_proto &&
	     nla_put_u8(skb, IFA_PROTO, ifa->ifa_proto)) ||
	    nla_put_u32(skb, IFA_FLAGS, flags) ||
	    (ifa->ifa_rt_priority &&
	     nla_put_u32(skb, IFA_RT_PRIORITY, ifa->ifa_rt_priority)) ||
	    put_cacheinfo(skb, READ_ONCE(ifa->ifa_cstamp), tstamp,
			  preferred, valid))
		goto nla_put_failure;

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static int inet_valid_dump_ifaddr_req(const struct nlmsghdr *nlh,
				      struct inet_fill_args *fillargs,
				      struct net **tgt_net, struct sock *sk,
				      struct netlink_callback *cb)
{
	struct netlink_ext_ack *extack = cb->extack;
	struct nlattr *tb[IFA_MAX+1];
	struct ifaddrmsg *ifm;
	int err, i;

	ifm = nlmsg_payload(nlh, sizeof(*ifm));
	if (!ifm) {
		NL_SET_ERR_MSG(extack, "ipv4: Invalid header for address dump request");
		return -EINVAL;
	}

	if (ifm->ifa_prefixlen || ifm->ifa_flags || ifm->ifa_scope) {
		NL_SET_ERR_MSG(extack, "ipv4: Invalid values in header for address dump request");
		return -EINVAL;
	}

	fillargs->ifindex = ifm->ifa_index;
	if (fillargs->ifindex) {
		cb->answer_flags |= NLM_F_DUMP_FILTERED;
		fillargs->flags |= NLM_F_DUMP_FILTERED;
	}

	err = nlmsg_parse_deprecated_strict(nlh, sizeof(*ifm), tb, IFA_MAX,
					    ifa_ipv4_policy, extack);
	if (err < 0)
		return err;

	for (i = 0; i <= IFA_MAX; ++i) {
		if (!tb[i])
			continue;

		if (i == IFA_TARGET_NETNSID) {
			struct net *net;

			fillargs->netnsid = nla_get_s32(tb[i]);

			net = rtnl_get_net_ns_capable(sk, fillargs->netnsid);
			if (IS_ERR(net)) {
				fillargs->netnsid = -1;
				NL_SET_ERR_MSG(extack, "ipv4: Invalid target network namespace id");
				return PTR_ERR(net);
			}
			*tgt_net = net;
		} else {
			NL_SET_ERR_MSG(extack, "ipv4: Unsupported attribute in dump request");
			return -EINVAL;
		}
	}

	return 0;
}

static int in_dev_dump_ifmcaddr(struct in_device *in_dev, struct sk_buff *skb,
				struct netlink_callback *cb, int *s_ip_idx,
				struct inet_fill_args *fillargs)
{
	struct ip_mc_list *im;
	int ip_idx = 0;
	int err;

	for (im = rcu_dereference(in_dev->mc_list);
	     im;
	     im = rcu_dereference(im->next_rcu)) {
		if (ip_idx < *s_ip_idx) {
			ip_idx++;
			continue;
		}
		err = inet_fill_ifmcaddr(skb, in_dev->dev, im, fillargs);
		if (err < 0)
			goto done;

		nl_dump_check_consistent(cb, nlmsg_hdr(skb));
		ip_idx++;
	}
	err = 0;
	ip_idx = 0;
done:
	*s_ip_idx = ip_idx;
	return err;
}

static int in_dev_dump_ifaddr(struct in_device *in_dev, struct sk_buff *skb,
			      struct netlink_callback *cb, int *s_ip_idx,
			      struct inet_fill_args *fillargs)
{
	struct in_ifaddr *ifa;
	int ip_idx = 0;
	int err;

	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		if (ip_idx < *s_ip_idx) {
			ip_idx++;
			continue;
		}
		err = inet_fill_ifaddr(skb, ifa, fillargs);
		if (err < 0)
			goto done;

		nl_dump_check_consistent(cb, nlmsg_hdr(skb));
		ip_idx++;
	}
	err = 0;
	ip_idx = 0;
done:
	*s_ip_idx = ip_idx;

	return err;
}

static int in_dev_dump_addr(struct in_device *in_dev, struct sk_buff *skb,
			    struct netlink_callback *cb, int *s_ip_idx,
			    struct inet_fill_args *fillargs)
{
	switch (fillargs->event) {
	case RTM_NEWADDR:
		return in_dev_dump_ifaddr(in_dev, skb, cb, s_ip_idx, fillargs);
	case RTM_GETMULTICAST:
		return in_dev_dump_ifmcaddr(in_dev, skb, cb, s_ip_idx,
					    fillargs);
	default:
		return -EINVAL;
	}
}

/* Combine dev_addr_genid and dev_base_seq to detect changes.
 */
static u32 inet_base_seq(const struct net *net)
{
	u32 res = atomic_read(&net->ipv4.dev_addr_genid) +
		  READ_ONCE(net->dev_base_seq);

	/* Must not return 0 (see nl_dump_check_consistent()).
	 * Chose a value far away from 0.
	 */
	if (!res)
		res = 0x80000000;
	return res;
}

static int inet_dump_addr(struct sk_buff *skb, struct netlink_callback *cb,
			  int event)
{
	const struct nlmsghdr *nlh = cb->nlh;
	struct inet_fill_args fillargs = {
		.portid = NETLINK_CB(cb->skb).portid,
		.seq = nlh->nlmsg_seq,
		.event = event,
		.flags = NLM_F_MULTI,
		.netnsid = -1,
	};
	struct net *net = sock_net(skb->sk);
	struct net *tgt_net = net;
	struct {
		unsigned long ifindex;
		int ip_idx;
	} *ctx = (void *)cb->ctx;
	struct in_device *in_dev;
	struct net_device *dev;
	int err = 0;

	rcu_read_lock();
	if (cb->strict_check) {
		err = inet_valid_dump_ifaddr_req(nlh, &fillargs, &tgt_net,
						 skb->sk, cb);
		if (err < 0)
			goto done;

		if (fillargs.ifindex) {
			dev = dev_get_by_index_rcu(tgt_net, fillargs.ifindex);
			if (!dev) {
				err = -ENODEV;
				goto done;
			}
			in_dev = __in_dev_get_rcu(dev);
			if (!in_dev)
				goto done;
			err = in_dev_dump_addr(in_dev, skb, cb, &ctx->ip_idx,
					       &fillargs);
			goto done;
		}
	}

	cb->seq = inet_base_seq(tgt_net);

	for_each_netdev_dump(tgt_net, dev, ctx->ifindex) {
		in_dev = __in_dev_get_rcu(dev);
		if (!in_dev)
			continue;
		err = in_dev_dump_addr(in_dev, skb, cb, &ctx->ip_idx,
				       &fillargs);
		if (err < 0)
			goto done;
	}
done:
	if (fillargs.netnsid >= 0)
		put_net(tgt_net);
	rcu_read_unlock();
	return err;
}

static int inet_dump_ifaddr(struct sk_buff *skb, struct netlink_callback *cb)
{
	return inet_dump_addr(skb, cb, RTM_NEWADDR);
}

static int inet_dump_ifmcaddr(struct sk_buff *skb, struct netlink_callback *cb)
{
	return inet_dump_addr(skb, cb, RTM_GETMULTICAST);
}

static void rtmsg_ifa(int event, struct in_ifaddr *ifa, struct nlmsghdr *nlh,
		      u32 portid)
{
	struct inet_fill_args fillargs = {
		.portid = portid,
		.seq = nlh ? nlh->nlmsg_seq : 0,
		.event = event,
		.flags = 0,
		.netnsid = -1,
	};
	struct sk_buff *skb;
	int err = -ENOBUFS;
	struct net *net;

	net = dev_net(ifa->ifa_dev->dev);
	skb = nlmsg_new(inet_nlmsg_size(), GFP_KERNEL);
	if (!skb)
		goto errout;

	err = inet_fill_ifaddr(skb, ifa, &fillargs);
	if (err < 0) {
		/* -EMSGSIZE implies BUG in inet_nlmsg_size() */
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}
	rtnl_notify(skb, net, portid, RTNLGRP_IPV4_IFADDR, nlh, GFP_KERNEL);
	return;
errout:
	rtnl_set_sk_err(net, RTNLGRP_IPV4_IFADDR, err);
}

static size_t inet_get_link_af_size(const struct net_device *dev,
				    u32 ext_filter_mask)
{
	struct in_device *in_dev = rcu_dereference_rtnl(dev->ip_ptr);

	if (!in_dev)
		return 0;

	return nla_total_size(IPV4_DEVCONF_MAX * 4); /* IFLA_INET_CONF */
}

static int inet_fill_link_af(struct sk_buff *skb, const struct net_device *dev,
			     u32 ext_filter_mask)
{
	struct in_device *in_dev = rcu_dereference_rtnl(dev->ip_ptr);
	struct nlattr *nla;
	int i;

	if (!in_dev)
		return -ENODATA;

	nla = nla_reserve(skb, IFLA_INET_CONF, IPV4_DEVCONF_MAX * 4);
	if (!nla)
		return -EMSGSIZE;

	for (i = 0; i < IPV4_DEVCONF_MAX; i++)
		((u32 *) nla_data(nla))[i] = READ_ONCE(in_dev->cnf.data[i]);

	return 0;
}

static const struct nla_policy inet_af_policy[IFLA_INET_MAX+1] = {
	[IFLA_INET_CONF]	= { .type = NLA_NESTED },
};

static int inet_validate_link_af(const struct net_device *dev,
				 const struct nlattr *nla,
				 struct netlink_ext_ack *extack)
{
	struct nlattr *a, *tb[IFLA_INET_MAX+1];
	int err, rem;

	if (dev && !__in_dev_get_rtnl(dev))
		return -EAFNOSUPPORT;

	err = nla_parse_nested_deprecated(tb, IFLA_INET_MAX, nla,
					  inet_af_policy, extack);
	if (err < 0)
		return err;

	if (tb[IFLA_INET_CONF]) {
		nla_for_each_nested(a, tb[IFLA_INET_CONF], rem) {
			int cfgid = nla_type(a);

			if (nla_len(a) < 4)
				return -EINVAL;

			if (cfgid <= 0 || cfgid > IPV4_DEVCONF_MAX)
				return -EINVAL;
		}
	}

	return 0;
}

static int inet_set_link_af(struct net_device *dev, const struct nlattr *nla,
			    struct netlink_ext_ack *extack)
{
	struct in_device *in_dev = __in_dev_get_rtnl(dev);
	struct nlattr *a, *tb[IFLA_INET_MAX+1];
	int rem;

	if (!in_dev)
		return -EAFNOSUPPORT;

	if (nla_parse_nested_deprecated(tb, IFLA_INET_MAX, nla, NULL, NULL) < 0)
		return -EINVAL;

	if (tb[IFLA_INET_CONF]) {
		nla_for_each_nested(a, tb[IFLA_INET_CONF], rem)
			ipv4_devconf_set(in_dev, nla_type(a), nla_get_u32(a));
	}

	return 0;
}

static int inet_netconf_msgsize_devconf(int type)
{
	int size = NLMSG_ALIGN(sizeof(struct netconfmsg))
		   + nla_total_size(4);	/* NETCONFA_IFINDEX */
	bool all = false;

	if (type == NETCONFA_ALL)
		all = true;

	if (all || type == NETCONFA_FORWARDING)
		size += nla_total_size(4);
	if (all || type == NETCONFA_RP_FILTER)
		size += nla_total_size(4);
	if (all || type == NETCONFA_MC_FORWARDING)
		size += nla_total_size(4);
	if (all || type == NETCONFA_BC_FORWARDING)
		size += nla_total_size(4);
	if (all || type == NETCONFA_PROXY_NEIGH)
		size += nla_total_size(4);
	if (all || type == NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN)
		size += nla_total_size(4);

	return size;
}

static int inet_netconf_fill_devconf(struct sk_buff *skb, int ifindex,
				     const struct ipv4_devconf *devconf,
				     u32 portid, u32 seq, int event,
				     unsigned int flags, int type)
{
	struct nlmsghdr  *nlh;
	struct netconfmsg *ncm;
	bool all = false;

	nlh = nlmsg_put(skb, portid, seq, event, sizeof(struct netconfmsg),
			flags);
	if (!nlh)
		return -EMSGSIZE;

	if (type == NETCONFA_ALL)
		all = true;

	ncm = nlmsg_data(nlh);
	ncm->ncm_family = AF_INET;

	if (nla_put_s32(skb, NETCONFA_IFINDEX, ifindex) < 0)
		goto nla_put_failure;

	if (!devconf)
		goto out;

	if ((all || type == NETCONFA_FORWARDING) &&
	    nla_put_s32(skb, NETCONFA_FORWARDING,
			IPV4_DEVCONF_RO(*devconf, FORWARDING)) < 0)
		goto nla_put_failure;
	if ((all || type == NETCONFA_RP_FILTER) &&
	    nla_put_s32(skb, NETCONFA_RP_FILTER,
			IPV4_DEVCONF_RO(*devconf, RP_FILTER)) < 0)
		goto nla_put_failure;
	if ((all || type == NETCONFA_MC_FORWARDING) &&
	    nla_put_s32(skb, NETCONFA_MC_FORWARDING,
			IPV4_DEVCONF_RO(*devconf, MC_FORWARDING)) < 0)
		goto nla_put_failure;
	if ((all || type == NETCONFA_BC_FORWARDING) &&
	    nla_put_s32(skb, NETCONFA_BC_FORWARDING,
			IPV4_DEVCONF_RO(*devconf, BC_FORWARDING)) < 0)
		goto nla_put_failure;
	if ((all || type == NETCONFA_PROXY_NEIGH) &&
	    nla_put_s32(skb, NETCONFA_PROXY_NEIGH,
			IPV4_DEVCONF_RO(*devconf, PROXY_ARP)) < 0)
		goto nla_put_failure;
	if ((all || type == NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN) &&
	    nla_put_s32(skb, NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN,
			IPV4_DEVCONF_RO(*devconf,
					IGNORE_ROUTES_WITH_LINKDOWN)) < 0)
		goto nla_put_failure;

out:
	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

void inet_netconf_notify_devconf(struct net *net, int event, int type,
				 int ifindex, struct ipv4_devconf *devconf)
{
	struct sk_buff *skb;
	int err = -ENOBUFS;

	skb = nlmsg_new(inet_netconf_msgsize_devconf(type), GFP_KERNEL);
	if (!skb)
		goto errout;

	err = inet_netconf_fill_devconf(skb, ifindex, devconf, 0, 0,
					event, 0, type);
	if (err < 0) {
		/* -EMSGSIZE implies BUG in inet_netconf_msgsize_devconf() */
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}
	rtnl_notify(skb, net, 0, RTNLGRP_IPV4_NETCONF, NULL, GFP_KERNEL);
	return;
errout:
	rtnl_set_sk_err(net, RTNLGRP_IPV4_NETCONF, err);
}

static const struct nla_policy devconf_ipv4_policy[NETCONFA_MAX+1] = {
	[NETCONFA_IFINDEX]	= { .len = sizeof(int) },
	[NETCONFA_FORWARDING]	= { .len = sizeof(int) },
	[NETCONFA_RP_FILTER]	= { .len = sizeof(int) },
	[NETCONFA_PROXY_NEIGH]	= { .len = sizeof(int) },
	[NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN]	= { .len = sizeof(int) },
};

static int inet_netconf_valid_get_req(struct sk_buff *skb,
				      const struct nlmsghdr *nlh,
				      struct nlattr **tb,
				      struct netlink_ext_ack *extack)
{
	int i, err;

	if (nlh->nlmsg_len < nlmsg_msg_size(sizeof(struct netconfmsg))) {
		NL_SET_ERR_MSG(extack, "ipv4: Invalid header for netconf get request");
		return -EINVAL;
	}

	if (!netlink_strict_get_check(skb))
		return nlmsg_parse_deprecated(nlh, sizeof(struct netconfmsg),
					      tb, NETCONFA_MAX,
					      devconf_ipv4_policy, extack);

	err = nlmsg_parse_deprecated_strict(nlh, sizeof(struct netconfmsg),
					    tb, NETCONFA_MAX,
					    devconf_ipv4_policy, extack);
	if (err)
		return err;

	for (i = 0; i <= NETCONFA_MAX; i++) {
		if (!tb[i])
			continue;

		switch (i) {
		case NETCONFA_IFINDEX:
			break;
		default:
			NL_SET_ERR_MSG(extack, "ipv4: Unsupported attribute in netconf get request");
			return -EINVAL;
		}
	}

	return 0;
}

static int inet_netconf_get_devconf(struct sk_buff *in_skb,
				    struct nlmsghdr *nlh,
				    struct netlink_ext_ack *extack)
{
	struct net *net = sock_net(in_skb->sk);
	struct nlattr *tb[NETCONFA_MAX + 1];
	const struct ipv4_devconf *devconf;
	struct in_device *in_dev = NULL;
	struct net_device *dev = NULL;
	struct sk_buff *skb;
	int ifindex;
	int err;

	err = inet_netconf_valid_get_req(in_skb, nlh, tb, extack);
	if (err)
		return err;

	if (!tb[NETCONFA_IFINDEX])
		return -EINVAL;

	ifindex = nla_get_s32(tb[NETCONFA_IFINDEX]);
	switch (ifindex) {
	case NETCONFA_IFINDEX_ALL:
		devconf = net->ipv4.devconf_all;
		break;
	case NETCONFA_IFINDEX_DEFAULT:
		devconf = net->ipv4.devconf_dflt;
		break;
	default:
		err = -ENODEV;
		dev = dev_get_by_index(net, ifindex);
		if (dev)
			in_dev = in_dev_get(dev);
		if (!in_dev)
			goto errout;
		devconf = &in_dev->cnf;
		break;
	}

	err = -ENOBUFS;
	skb = nlmsg_new(inet_netconf_msgsize_devconf(NETCONFA_ALL), GFP_KERNEL);
	if (!skb)
		goto errout;

	err = inet_netconf_fill_devconf(skb, ifindex, devconf,
					NETLINK_CB(in_skb).portid,
					nlh->nlmsg_seq, RTM_NEWNETCONF, 0,
					NETCONFA_ALL);
	if (err < 0) {
		/* -EMSGSIZE implies BUG in inet_netconf_msgsize_devconf() */
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}
	err = rtnl_unicast(skb, net, NETLINK_CB(in_skb).portid);
errout:
	if (in_dev)
		in_dev_put(in_dev);
	dev_put(dev);
	return err;
}

static int inet_netconf_dump_devconf(struct sk_buff *skb,
				     struct netlink_callback *cb)
{
	const struct nlmsghdr *nlh = cb->nlh;
	struct net *net = sock_net(skb->sk);
	struct {
		unsigned long ifindex;
		unsigned int all_default;
	} *ctx = (void *)cb->ctx;
	const struct in_device *in_dev;
	struct net_device *dev;
	int err = 0;

	if (cb->strict_check) {
		struct netlink_ext_ack *extack = cb->extack;
		struct netconfmsg *ncm;

		if (nlh->nlmsg_len < nlmsg_msg_size(sizeof(*ncm))) {
			NL_SET_ERR_MSG(extack, "ipv4: Invalid header for netconf dump request");
			return -EINVAL;
		}

		if (nlmsg_attrlen(nlh, sizeof(*ncm))) {
			NL_SET_ERR_MSG(extack, "ipv4: Invalid data after header in netconf dump request");
			return -EINVAL;
		}
	}

	rcu_read_lock();
	for_each_netdev_dump(net, dev, ctx->ifindex) {
		in_dev = __in_dev_get_rcu(dev);
		if (!in_dev)
			continue;
		err = inet_netconf_fill_devconf(skb, dev->ifindex,
						&in_dev->cnf,
						NETLINK_CB(cb->skb).portid,
						nlh->nlmsg_seq,
						RTM_NEWNETCONF, NLM_F_MULTI,
						NETCONFA_ALL);
		if (err < 0)
			goto done;
	}
	if (ctx->all_default == 0) {
		err = inet_netconf_fill_devconf(skb, NETCONFA_IFINDEX_ALL,
						net->ipv4.devconf_all,
						NETLINK_CB(cb->skb).portid,
						nlh->nlmsg_seq,
						RTM_NEWNETCONF, NLM_F_MULTI,
						NETCONFA_ALL);
		if (err < 0)
			goto done;
		ctx->all_default++;
	}
	if (ctx->all_default == 1) {
		err = inet_netconf_fill_devconf(skb, NETCONFA_IFINDEX_DEFAULT,
						net->ipv4.devconf_dflt,
						NETLINK_CB(cb->skb).portid,
						nlh->nlmsg_seq,
						RTM_NEWNETCONF, NLM_F_MULTI,
						NETCONFA_ALL);
		if (err < 0)
			goto done;
		ctx->all_default++;
	}
done:
	rcu_read_unlock();
	return err;
}

#ifdef CONFIG_SYSCTL

static void devinet_copy_dflt_conf(struct net *net, int i)
{
	struct net_device *dev;

	rcu_read_lock();
	for_each_netdev_rcu(net, dev) {
		struct in_device *in_dev;

		in_dev = __in_dev_get_rcu(dev);
		if (in_dev && !test_bit(i, in_dev->cnf.state))
			in_dev->cnf.data[i] = net->ipv4.devconf_dflt->data[i];
	}
	rcu_read_unlock();
}

/* called with RTNL locked */
static void inet_forward_change(struct net *net)
{
	struct net_device *dev;
	int on = IPV4_DEVCONF_ALL(net, FORWARDING);

	IPV4_DEVCONF_ALL(net, ACCEPT_REDIRECTS) = !on;
	IPV4_DEVCONF_DFLT(net, FORWARDING) = on;
	inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
				    NETCONFA_FORWARDING,
				    NETCONFA_IFINDEX_ALL,
				    net->ipv4.devconf_all);
	inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
				    NETCONFA_FORWARDING,
				    NETCONFA_IFINDEX_DEFAULT,
				    net->ipv4.devconf_dflt);

	for_each_netdev(net, dev) {
		struct in_device *in_dev;

		if (on)
			dev_disable_lro(dev);

		in_dev = __in_dev_get_rtnl_net(dev);
		if (in_dev) {
			IN_DEV_CONF_SET(in_dev, FORWARDING, on);
			inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
						    NETCONFA_FORWARDING,
						    dev->ifindex, &in_dev->cnf);
		}
	}
}

static int devinet_conf_ifindex(struct net *net, struct ipv4_devconf *cnf)
{
	if (cnf == net->ipv4.devconf_dflt)
		return NETCONFA_IFINDEX_DEFAULT;
	else if (cnf == net->ipv4.devconf_all)
		return NETCONFA_IFINDEX_ALL;
	else {
		struct in_device *idev
			= container_of(cnf, struct in_device, cnf);
		return idev->dev->ifindex;
	}
}

static int devinet_conf_proc(const struct ctl_table *ctl, int write,
			     void *buffer, size_t *lenp, loff_t *ppos)
{
	int old_value = *(int *)ctl->data;
	int ret = proc_dointvec(ctl, write, buffer, lenp, ppos);
	int new_value = *(int *)ctl->data;

	if (write) {
		struct ipv4_devconf *cnf = ctl->extra1;
		struct net *net = ctl->extra2;
		int i = (int *)ctl->data - cnf->data;
		int ifindex;

		set_bit(i, cnf->state);

		if (cnf == net->ipv4.devconf_dflt)
			devinet_copy_dflt_conf(net, i);
		if (i == IPV4_DEVCONF_ACCEPT_LOCAL - 1 ||
		    i == IPV4_DEVCONF_ROUTE_LOCALNET - 1)
			if ((new_value == 0) && (old_value != 0))
				rt_cache_flush(net);

		if (i == IPV4_DEVCONF_BC_FORWARDING - 1 &&
		    new_value != old_value)
			rt_cache_flush(net);

		if (i == IPV4_DEVCONF_RP_FILTER - 1 &&
		    new_value != old_value) {
			ifindex = devinet_conf_ifindex(net, cnf);
			inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
						    NETCONFA_RP_FILTER,
						    ifindex, cnf);
		}
		if (i == IPV4_DEVCONF_PROXY_ARP - 1 &&
		    new_value != old_value) {
			ifindex = devinet_conf_ifindex(net, cnf);
			inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
						    NETCONFA_PROXY_NEIGH,
						    ifindex, cnf);
		}
		if (i == IPV4_DEVCONF_IGNORE_ROUTES_WITH_LINKDOWN - 1 &&
		    new_value != old_value) {
			ifindex = devinet_conf_ifindex(net, cnf);
			inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
						    NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN,
						    ifindex, cnf);
		}
	}

	return ret;
}

static int devinet_sysctl_forward(const struct ctl_table *ctl, int write,
				  void *buffer, size_t *lenp, loff_t *ppos)
{
	int *valp = ctl->data;
	int val = *valp;
	loff_t pos = *ppos;
	struct net *net = ctl->extra2;
	int ret;

	if (write && !ns_capable(net->user_ns, CAP_NET_ADMIN))
		return -EPERM;

	ret = proc_dointvec(ctl, write, buffer, lenp, ppos);

	if (write && *valp != val) {
		if (valp != &IPV4_DEVCONF_DFLT(net, FORWARDING)) {
			if (!rtnl_net_trylock(net)) {
				/* Restore the original values before restarting */
				*valp = val;
				*ppos = pos;
				return restart_syscall();
			}
			if (valp == &IPV4_DEVCONF_ALL(net, FORWARDING)) {
				inet_forward_change(net);
			} else {
				struct ipv4_devconf *cnf = ctl->extra1;
				struct in_device *idev =
					container_of(cnf, struct in_device, cnf);
				if (*valp)
					dev_disable_lro(idev->dev);
				inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
							    NETCONFA_FORWARDING,
							    idev->dev->ifindex,
							    cnf);
			}
			rtnl_net_unlock(net);
			rt_cache_flush(net);
		} else
			inet_netconf_notify_devconf(net, RTM_NEWNETCONF,
						    NETCONFA_FORWARDING,
						    NETCONFA_IFINDEX_DEFAULT,
						    net->ipv4.devconf_dflt);
	}

	return ret;
}

static int ipv4_doint_and_flush(const struct ctl_table *ctl, int write,
				void *buffer, size_t *lenp, loff_t *ppos)
{
	int *valp = ctl->data;
	int val = *valp;
	int ret = proc_dointvec(ctl, write, buffer, lenp, ppos);
	struct net *net = ctl->extra2;

	if (write && *valp != val)
		rt_cache_flush(net);

	return ret;
}

#define DEVINET_SYSCTL_ENTRY(attr, name, mval, proc) \
	{ \
		.procname	= name, \
		.data		= ipv4_devconf.data + \
				  IPV4_DEVCONF_ ## attr - 1, \
		.maxlen		= sizeof(int), \
		.mode		= mval, \
		.proc_handler	= proc, \
		.extra1		= &ipv4_devconf, \
	}

#define DEVINET_SYSCTL_RW_ENTRY(attr, name) \
	DEVINET_SYSCTL_ENTRY(attr, name, 0644, devinet_conf_proc)

#define DEVINET_SYSCTL_RO_ENTRY(attr, name) \
	DEVINET_SYSCTL_ENTRY(attr, name, 0444, devinet_conf_proc)

#define DEVINET_SYSCTL_COMPLEX_ENTRY(attr, name, proc) \
	DEVINET_SYSCTL_ENTRY(attr, name, 0644, proc)

#define DEVINET_SYSCTL_FLUSHING_ENTRY(attr, name) \
	DEVINET_SYSCTL_COMPLEX_ENTRY(attr, name, ipv4_doint_and_flush)

static struct devinet_sysctl_table {
	struct ctl_table_header *sysctl_header;
	struct ctl_table devinet_vars[IPV4_DEVCONF_MAX];
} devinet_sysctl = {
	.devinet_vars = {
		DEVINET_SYSCTL_COMPLEX_ENTRY(FORWARDING, "forwarding",
					     devinet_sysctl_forward),
		DEVINET_SYSCTL_RO_ENTRY(MC_FORWARDING, "mc_forwarding"),
		DEVINET_SYSCTL_RW_ENTRY(BC_FORWARDING, "bc_forwarding"),

		DEVINET_SYSCTL_RW_ENTRY(ACCEPT_REDIRECTS, "accept_redirects"),
		DEVINET_SYSCTL_RW_ENTRY(SECURE_REDIRECTS, "secure_redirects"),
		DEVINET_SYSCTL_RW_ENTRY(SHARED_MEDIA, "shared_media"),
		DEVINET_SYSCTL_RW_ENTRY(RP_FILTER, "rp_filter"),
		DEVINET_SYSCTL_RW_ENTRY(SEND_REDIRECTS, "send_redirects"),
		DEVINET_SYSCTL_RW_ENTRY(ACCEPT_SOURCE_ROUTE,
					"accept_source_route"),
		DEVINET_SYSCTL_RW_ENTRY(ACCEPT_LOCAL, "accept_local"),
		DEVINET_SYSCTL_RW_ENTRY(SRC_VMARK, "src_valid_mark"),
		DEVINET_SYSCTL_RW_ENTRY(PROXY_ARP, "proxy_arp"),
		DEVINET_SYSCTL_RW_ENTRY(MEDIUM_ID, "medium_id"),
		DEVINET_SYSCTL_RW_ENTRY(BOOTP_RELAY, "bootp_relay"),
		DEVINET_SYSCTL_RW_ENTRY(LOG_MARTIANS, "log_martians"),
		DEVINET_SYSCTL_RW_ENTRY(TAG, "tag"),
		DEVINET_SYSCTL_RW_ENTRY(ARPFILTER, "arp_filter"),
		DEVINET_SYSCTL_RW_ENTRY(ARP_ANNOUNCE, "arp_announce"),
		DEVINET_SYSCTL_RW_ENTRY(ARP_IGNORE, "arp_ignore"),
		DEVINET_SYSCTL_RW_ENTRY(ARP_ACCEPT, "arp_accept"),
		DEVINET_SYSCTL_RW_ENTRY(ARP_NOTIFY, "arp_notify"),
		DEVINET_SYSCTL_RW_ENTRY(ARP_EVICT_NOCARRIER,
					"arp_evict_nocarrier"),
		DEVINET_SYSCTL_RW_ENTRY(PROXY_ARP_PVLAN, "proxy_arp_pvlan"),
		DEVINET_SYSCTL_RW_ENTRY(FORCE_IGMP_VERSION,
					"force_igmp_version"),
		DEVINET_SYSCTL_RW_ENTRY(IGMPV2_UNSOLICITED_REPORT_INTERVAL,
					"igmpv2_unsolicited_report_interval"),
		DEVINET_SYSCTL_RW_ENTRY(IGMPV3_UNSOLICITED_REPORT_INTERVAL,
					"igmpv3_unsolicited_report_interval"),
		DEVINET_SYSCTL_RW_ENTRY(IGNORE_ROUTES_WITH_LINKDOWN,
					"ignore_routes_with_linkdown"),
		DEVINET_SYSCTL_RW_ENTRY(DROP_GRATUITOUS_ARP,
					"drop_gratuitous_arp"),

		DEVINET_SYSCTL_FLUSHING_ENTRY(NOXFRM, "disable_xfrm"),
		DEVINET_SYSCTL_FLUSHING_ENTRY(NOPOLICY, "disable_policy"),
		DEVINET_SYSCTL_FLUSHING_ENTRY(PROMOTE_SECONDARIES,
					      "promote_secondaries"),
		DEVINET_SYSCTL_FLUSHING_ENTRY(ROUTE_LOCALNET,
					      "route_localnet"),
		DEVINET_SYSCTL_FLUSHING_ENTRY(DROP_UNICAST_IN_L2_MULTICAST,
					      "drop_unicast_in_l2_multicast"),
		DEVINET_SYSCTL_RW_ENTRY(STEALTH, "stealth"),
	},
};

static int __devinet_sysctl_register(struct net *net, char *dev_name,
				     int ifindex, struct ipv4_devconf *p)
{
	int i;
	struct devinet_sysctl_table *t;
	char path[sizeof("net/ipv4/conf/") + IFNAMSIZ];

	t = kmemdup(&devinet_sysctl, sizeof(*t), GFP_KERNEL_ACCOUNT);
	if (!t)
		goto out;

	for (i = 0; i < ARRAY_SIZE(t->devinet_vars); i++) {
		t->devinet_vars[i].data += (char *)p - (char *)&ipv4_devconf;
		t->devinet_vars[i].extra1 = p;
		t->devinet_vars[i].extra2 = net;
	}

	snprintf(path, sizeof(path), "net/ipv4/conf/%s", dev_name);

	t->sysctl_header = register_net_sysctl(net, path, t->devinet_vars);
	if (!t->sysctl_header)
		goto free;

	p->sysctl = t;

	inet_netconf_notify_devconf(net, RTM_NEWNETCONF, NETCONFA_ALL,
				    ifindex, p);
	return 0;

free:
	kfree(t);
out:
	return -ENOMEM;
}

static void __devinet_sysctl_unregister(struct net *net,
					struct ipv4_devconf *cnf, int ifindex)
{
	struct devinet_sysctl_table *t = cnf->sysctl;

	if (t) {
		cnf->sysctl = NULL;
		unregister_net_sysctl_table(t->sysctl_header);
		kfree(t);
	}

	inet_netconf_notify_devconf(net, RTM_DELNETCONF, 0, ifindex, NULL);
}

static int devinet_sysctl_register(struct in_device *idev)
{
	int err;

	if (!sysctl_dev_name_is_allowed(idev->dev->name))
		return -EINVAL;

	err = neigh_sysctl_register(idev->dev, idev->arp_parms, NULL);
	if (err)
		return err;
	err = __devinet_sysctl_register(dev_net(idev->dev), idev->dev->name,
					idev->dev->ifindex, &idev->cnf);
	if (err)
		neigh_sysctl_unregister(idev->arp_parms);
	return err;
}

static void devinet_sysctl_unregister(struct in_device *idev)
{
	struct net *net = dev_net(idev->dev);

	__devinet_sysctl_unregister(net, &idev->cnf, idev->dev->ifindex);
	neigh_sysctl_unregister(idev->arp_parms);
}

static struct ctl_table ctl_forward_entry[] = {
	{
		.procname	= "ip_forward",
		.data		= &ipv4_devconf.data[
					IPV4_DEVCONF_FORWARDING - 1],
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= devinet_sysctl_forward,
		.extra1		= &ipv4_devconf,
		.extra2		= &init_net,
	},
};
#endif

static __net_init int devinet_init_net(struct net *net)
{
#ifdef CONFIG_SYSCTL
	struct ctl_table_header *forw_hdr;
	struct ctl_table *tbl;
#endif
	struct ipv4_devconf *all, *dflt;
	int err;
	int i;

	err = -ENOMEM;
	net->ipv4.inet_addr_lst = kmalloc_array(IN4_ADDR_HSIZE,
						sizeof(struct hlist_head),
						GFP_KERNEL);
	if (!net->ipv4.inet_addr_lst)
		goto err_alloc_hash;

	all = kmemdup(&ipv4_devconf, sizeof(ipv4_devconf), GFP_KERNEL);
	if (!all)
		goto err_alloc_all;

	dflt = kmemdup(&ipv4_devconf_dflt, sizeof(ipv4_devconf_dflt), GFP_KERNEL);
	if (!dflt)
		goto err_alloc_dflt;

#ifdef CONFIG_SYSCTL
	tbl = kmemdup(ctl_forward_entry, sizeof(ctl_forward_entry), GFP_KERNEL);
	if (!tbl)
		goto err_alloc_ctl;

	tbl[0].data = &all->data[IPV4_DEVCONF_FORWARDING - 1];
	tbl[0].extra1 = all;
	tbl[0].extra2 = net;
#endif

	if (!net_eq(net, &init_net)) {
		switch (net_inherit_devconf()) {
		case 3:
			/* copy from the current netns */
			memcpy(all, current->nsproxy->net_ns->ipv4.devconf_all,
			       sizeof(ipv4_devconf));
			memcpy(dflt,
			       current->nsproxy->net_ns->ipv4.devconf_dflt,
			       sizeof(ipv4_devconf_dflt));
			break;
		case 0:
		case 1:
			/* copy from init_net */
			memcpy(all, init_net.ipv4.devconf_all,
			       sizeof(ipv4_devconf));
			memcpy(dflt, init_net.ipv4.devconf_dflt,
			       sizeof(ipv4_devconf_dflt));
			break;
		case 2:
			/* use compiled values */
			break;
		}
	}

#ifdef CONFIG_SYSCTL
	err = __devinet_sysctl_register(net, "all", NETCONFA_IFINDEX_ALL, all);
	if (err < 0)
		goto err_reg_all;

	err = __devinet_sysctl_register(net, "default",
					NETCONFA_IFINDEX_DEFAULT, dflt);
	if (err < 0)
		goto err_reg_dflt;

	err = -ENOMEM;
	forw_hdr = register_net_sysctl_sz(net, "net/ipv4", tbl,
					  ARRAY_SIZE(ctl_forward_entry));
	if (!forw_hdr)
		goto err_reg_ctl;
	net->ipv4.forw_hdr = forw_hdr;
#endif

	for (i = 0; i < IN4_ADDR_HSIZE; i++)
		INIT_HLIST_HEAD(&net->ipv4.inet_addr_lst[i]);

	INIT_DEFERRABLE_WORK(&net->ipv4.addr_chk_work, check_lifetime);

	net->ipv4.devconf_all = all;
	net->ipv4.devconf_dflt = dflt;
	return 0;

#ifdef CONFIG_SYSCTL
err_reg_ctl:
	__devinet_sysctl_unregister(net, dflt, NETCONFA_IFINDEX_DEFAULT);
err_reg_dflt:
	__devinet_sysctl_unregister(net, all, NETCONFA_IFINDEX_ALL);
err_reg_all:
	kfree(tbl);
err_alloc_ctl:
#endif
	kfree(dflt);
err_alloc_dflt:
	kfree(all);
err_alloc_all:
	kfree(net->ipv4.inet_addr_lst);
err_alloc_hash:
	return err;
}

static __net_exit void devinet_exit_net(struct net *net)
{
#ifdef CONFIG_SYSCTL
	const struct ctl_table *tbl;
#endif

	cancel_delayed_work_sync(&net->ipv4.addr_chk_work);

#ifdef CONFIG_SYSCTL
	tbl = net->ipv4.forw_hdr->ctl_table_arg;
	unregister_net_sysctl_table(net->ipv4.forw_hdr);
	__devinet_sysctl_unregister(net, net->ipv4.devconf_dflt,
				    NETCONFA_IFINDEX_DEFAULT);
	__devinet_sysctl_unregister(net, net->ipv4.devconf_all,
				    NETCONFA_IFINDEX_ALL);
	kfree(tbl);
#endif
	kfree(net->ipv4.devconf_dflt);
	kfree(net->ipv4.devconf_all);
	kfree(net->ipv4.inet_addr_lst);
}

static __net_initdata struct pernet_operations devinet_ops = {
	.init = devinet_init_net,
	.exit = devinet_exit_net,
};

static struct rtnl_af_ops inet_af_ops __read_mostly = {
	.family		  = AF_INET,
	.fill_link_af	  = inet_fill_link_af,
	.get_link_af_size = inet_get_link_af_size,
	.validate_link_af = inet_validate_link_af,
	.set_link_af	  = inet_set_link_af,
};

static const struct rtnl_msg_handler devinet_rtnl_msg_handlers[] __initconst = {
	{.protocol = PF_INET, .msgtype = RTM_NEWADDR, .doit = inet_rtm_newaddr,
	 .flags = RTNL_FLAG_DOIT_PERNET},
	{.protocol = PF_INET, .msgtype = RTM_DELADDR, .doit = inet_rtm_deladdr,
	 .flags = RTNL_FLAG_DOIT_PERNET},
	{.protocol = PF_INET, .msgtype = RTM_GETADDR, .dumpit = inet_dump_ifaddr,
	 .flags = RTNL_FLAG_DUMP_UNLOCKED | RTNL_FLAG_DUMP_SPLIT_NLM_DONE},
	{.protocol = PF_INET, .msgtype = RTM_GETNETCONF,
	 .doit = inet_netconf_get_devconf, .dumpit = inet_netconf_dump_devconf,
	 .flags = RTNL_FLAG_DOIT_UNLOCKED | RTNL_FLAG_DUMP_UNLOCKED},
	{.owner = THIS_MODULE, .protocol = PF_INET, .msgtype = RTM_GETMULTICAST,
	 .dumpit = inet_dump_ifmcaddr, .flags = RTNL_FLAG_DUMP_UNLOCKED},
};

void __init devinet_init(void)
{
	register_pernet_subsys(&devinet_ops);
	register_netdevice_notifier(&ip_netdev_notifier);

	if (rtnl_af_register(&inet_af_ops))
		panic("Unable to register inet_af_ops\n");

	rtnl_register_many(devinet_rtnl_msg_handlers);
}
