/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/in.h>
#include <linux/fou.h>
#include <linux/if_arp.h>
#include <linux/if_tunnel.h>
#include <linux/ip.h>
#include <linux/ip6_tunnel.h>

#include "conf-parser.h"
#include "hexdecoct.h"
#include "missing_network.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "parse-util.h"
#include "siphash24.h"
#include "string-table.h"
#include "string-util.h"
#include "tunnel.h"
#include "util.h"

#define DEFAULT_TNL_HOP_LIMIT   64
#define IP6_FLOWINFO_FLOWLABEL  htobe32(0x000FFFFF)
#define IP6_TNL_F_ALLOW_LOCAL_REMOTE 0x40

static const char* const ip6tnl_mode_table[_NETDEV_IP6_TNL_MODE_MAX] = {
        [NETDEV_IP6_TNL_MODE_IP6IP6] = "ip6ip6",
        [NETDEV_IP6_TNL_MODE_IPIP6] = "ipip6",
        [NETDEV_IP6_TNL_MODE_ANYIP6] = "any",
};

DEFINE_STRING_TABLE_LOOKUP(ip6tnl_mode, Ip6TnlMode);
DEFINE_CONFIG_PARSE_ENUM(config_parse_ip6tnl_mode, ip6tnl_mode, Ip6TnlMode, "Failed to parse ip6 tunnel Mode");

#define HASH_KEY SD_ID128_MAKE(74,c4,de,12,f3,d9,41,34,bb,3d,c1,a4,42,93,50,87)

int dhcp4_pd_create_6rd_tunnel_name(Link *link, char **ret) {
        _cleanup_free_ char *ifname_alloc = NULL;
        uint8_t ipv4masklen, sixrd_prefixlen, *buf, *p;
        struct in_addr ipv4address;
        struct in6_addr sixrd_prefix;
        char ifname[IFNAMSIZ];
        uint64_t result;
        size_t sz;
        int r;

        assert(link);
        assert(link->dhcp_lease);

        r = sd_dhcp_lease_get_address(link->dhcp_lease, &ipv4address);
        if (r < 0)
                return log_link_debug_errno(link, r, "Failed to get DHCPv4 address: %m");

        r = sd_dhcp_lease_get_6rd(link->dhcp_lease, &ipv4masklen, &sixrd_prefixlen, &sixrd_prefix, NULL, NULL);
        if (r < 0)
                return log_link_debug_errno(link, r, "Failed to get 6rd option: %m");

        sz = sizeof(uint8_t) * 2 + sizeof(struct in6_addr) + sizeof(struct in_addr);
        buf = newa(uint8_t, sz);
        p = buf;
        p = mempcpy(p, &ipv4masklen, sizeof(uint8_t));
        p = mempcpy(p, &ipv4address, sizeof(struct in_addr));
        p = mempcpy(p, &sixrd_prefixlen, sizeof(uint8_t));
        p = mempcpy(p, &sixrd_prefix, sizeof(struct in6_addr));

        result = siphash24(buf, sz, HASH_KEY.bytes);
        memcpy(ifname, "6rd-", STRLEN("6rd-"));
        ifname[STRLEN("6rd-")    ] = urlsafe_base64char(result >> 54);
        ifname[STRLEN("6rd-") + 1] = urlsafe_base64char(result >> 48);
        ifname[STRLEN("6rd-") + 2] = urlsafe_base64char(result >> 42);
        ifname[STRLEN("6rd-") + 3] = urlsafe_base64char(result >> 36);
        ifname[STRLEN("6rd-") + 4] = urlsafe_base64char(result >> 30);
        ifname[STRLEN("6rd-") + 5] = urlsafe_base64char(result >> 24);
        ifname[STRLEN("6rd-") + 6] = urlsafe_base64char(result >> 18);
        ifname[STRLEN("6rd-") + 7] = urlsafe_base64char(result >> 12);
        ifname[STRLEN("6rd-") + 8] = urlsafe_base64char(result >> 6);
        ifname[STRLEN("6rd-") + 9] = urlsafe_base64char(result);
        ifname[STRLEN("6rd-") + 10] = '\0';
        assert_cc(STRLEN("6rd-") + 10 <= IFNAMSIZ);

        ifname_alloc = strdup(ifname);
        if (!ifname_alloc)
                return log_oom_debug();

        *ret = TAKE_PTR(ifname_alloc);
        return 0;
}

int dhcp4_pd_create_6rd_tunnel(Link *link, link_netlink_message_handler_t callback) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        uint8_t ipv4masklen, sixrd_prefixlen;
        struct in_addr ipv4address, relay_prefix;
        struct in6_addr sixrd_prefix;
        int r;

        assert(link);
        assert(link->ifindex > 0);
        assert(link->manager);
        assert(link->dhcp_lease);
        assert(link->dhcp4_6rd_tunnel_name);
        assert(callback);

        r = sd_dhcp_lease_get_address(link->dhcp_lease, &ipv4address);
        if (r < 0)
                return log_link_debug_errno(link, r, "Failed to get DHCPv4 address: %m");

        r = sd_dhcp_lease_get_6rd(link->dhcp_lease, &ipv4masklen, &sixrd_prefixlen, &sixrd_prefix, NULL, NULL);
        if (r < 0)
                return log_link_debug_errno(link, r, "Failed to get 6rd option: %m");

        r = sd_rtnl_message_new_link(link->manager->rtnl, &m, RTM_NEWLINK, 0);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not allocate RTM_NEWLINK message: %m");

        r = sd_netlink_message_append_string(m, IFLA_IFNAME, link->dhcp4_6rd_tunnel_name);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IFNAME, attribute: %m");

        r = sd_netlink_message_open_container(m, IFLA_LINKINFO);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_LINKINFO attribute: %m");

        r = sd_netlink_message_open_container_union(m, IFLA_INFO_DATA, "sit");
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_INFO_DATA attribute: %m");

        r = sd_netlink_message_append_u32(m, IFLA_IPTUN_LINK, link->ifindex);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IPTUN_LINK attribute: %m");

        r = sd_netlink_message_append_in_addr(m, IFLA_IPTUN_LOCAL, &ipv4address);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IPTUN_LOCAL attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_IPTUN_TTL, 64);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IPTUN_TTL attribute: %m");

        r = sd_netlink_message_append_in6_addr(m, IFLA_IPTUN_6RD_PREFIX, &sixrd_prefix);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IPTUN_6RD_PREFIX attribute: %m");

        r = sd_netlink_message_append_u16(m, IFLA_IPTUN_6RD_PREFIXLEN, sixrd_prefixlen);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IPTUN_6RD_PREFIXLEN attribute: %m");

        relay_prefix = ipv4address;
        (void) in4_addr_mask(&relay_prefix, ipv4masklen);
        r = sd_netlink_message_append_u32(m, IFLA_IPTUN_6RD_RELAY_PREFIX, relay_prefix.s_addr);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IPTUN_6RD_RELAY_PREFIX attribute: %m");

        r = sd_netlink_message_append_u16(m, IFLA_IPTUN_6RD_RELAY_PREFIXLEN, ipv4masklen);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_IPTUN_6RD_RELAY_PREFIXLEN attribute: %m");

        r = sd_netlink_message_close_container(m);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_INFO_DATA attribute: %m");

        r = sd_netlink_message_close_container(m);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not append IFLA_LINKINFO attribute: %m");

        r = netlink_call_async(link->manager->rtnl, NULL, m, callback,
                               link_netlink_destroy_callback, link);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not send rtnetlink message: %m");

        link_ref(link);

        return 0;
}

static int netdev_ipip_sit_fill_message_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        Tunnel *t;
        int r;

        assert(netdev);

        if (netdev->kind == NETDEV_KIND_IPIP)
                t = IPIP(netdev);
        else
                t = SIT(netdev);

        assert(m);
        assert(t);

        if (link || t->assign_to_loopback) {
                r = sd_netlink_message_append_u32(m, IFLA_IPTUN_LINK, link ? link->ifindex : LOOPBACK_IFINDEX);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_LINK attribute: %m");
        }

        r = sd_netlink_message_append_in_addr(m, IFLA_IPTUN_LOCAL, &t->local.in);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_LOCAL attribute: %m");

        r = sd_netlink_message_append_in_addr(m, IFLA_IPTUN_REMOTE, &t->remote.in);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_REMOTE attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_IPTUN_TTL, t->ttl);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_TTL attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_IPTUN_PMTUDISC, t->pmtudisc);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_PMTUDISC attribute: %m");

        if (t->fou_tunnel) {
                r = sd_netlink_message_append_u16(m, IFLA_IPTUN_ENCAP_TYPE, t->fou_encap_type);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_ENCAP_TYPE attribute: %m");

                r = sd_netlink_message_append_u16(m, IFLA_IPTUN_ENCAP_SPORT, htobe16(t->encap_src_port));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_ENCAP_SPORT attribute: %m");

                r = sd_netlink_message_append_u16(m, IFLA_IPTUN_ENCAP_DPORT, htobe16(t->fou_destination_port));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_ENCAP_DPORT attribute: %m");
        }

        if (netdev->kind == NETDEV_KIND_SIT) {
                if (t->sixrd_prefixlen > 0) {
                        r = sd_netlink_message_append_in6_addr(m, IFLA_IPTUN_6RD_PREFIX, &t->sixrd_prefix);
                        if (r < 0)
                                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_6RD_PREFIX attribute: %m");

                        /* u16 is deliberate here, even though we're passing a netmask that can never be >128. The kernel is
                         * expecting to receive the prefixlen as a u16.
                         */
                        r = sd_netlink_message_append_u16(m, IFLA_IPTUN_6RD_PREFIXLEN, t->sixrd_prefixlen);
                        if (r < 0)
                                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_6RD_PREFIXLEN attribute: %m");
                }

                if (t->isatap >= 0) {
                        uint16_t flags = 0;

                        SET_FLAG(flags, SIT_ISATAP, t->isatap);

                        r = sd_netlink_message_append_u16(m, IFLA_IPTUN_FLAGS, flags);
                        if (r < 0)
                                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_FLAGS attribute: %m");
                }
        }

        return r;
}

static int netdev_gre_erspan_fill_message_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        uint32_t ikey = 0;
        uint32_t okey = 0;
        uint16_t iflags = 0;
        uint16_t oflags = 0;
        Tunnel *t;
        int r;

        assert(netdev);
        assert(m);

        switch (netdev->kind) {
        case NETDEV_KIND_GRE:
                t = GRE(netdev);
                break;
        case NETDEV_KIND_ERSPAN:
                t = ERSPAN(netdev);
                break;
        case NETDEV_KIND_GRETAP:
                t = GRETAP(netdev);
                break;
        default:
                assert_not_reached();
        }

        assert(t);

        if (link || t->assign_to_loopback) {
                r = sd_netlink_message_append_u32(m, IFLA_GRE_LINK, link ? link->ifindex : LOOPBACK_IFINDEX);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_LINK attribute: %m");
        }

        if (netdev->kind == NETDEV_KIND_ERSPAN) {
                r = sd_netlink_message_append_u32(m, IFLA_GRE_ERSPAN_INDEX, t->erspan_index);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_ERSPAN_INDEX attribute: %m");
        }

        r = sd_netlink_message_append_in_addr(m, IFLA_GRE_LOCAL, &t->local.in);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_LOCAL attribute: %m");

        r = sd_netlink_message_append_in_addr(m, IFLA_GRE_REMOTE, &t->remote.in);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_REMOTE attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_GRE_TTL, t->ttl);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_TTL attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_GRE_TOS, t->tos);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_TOS attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_GRE_PMTUDISC, t->pmtudisc);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_PMTUDISC attribute: %m");

        if (t->key != 0) {
                ikey = okey = htobe32(t->key);
                iflags |= GRE_KEY;
                oflags |= GRE_KEY;
        }

        if (t->ikey != 0) {
                ikey = htobe32(t->ikey);
                iflags |= GRE_KEY;
        }

        if (t->okey != 0) {
                okey = htobe32(t->okey);
                oflags |= GRE_KEY;
        }

        if (t->gre_erspan_sequence > 0) {
                iflags |= GRE_SEQ;
                oflags |= GRE_SEQ;
        } else if (t->gre_erspan_sequence == 0) {
                iflags &= ~GRE_SEQ;
                oflags &= ~GRE_SEQ;
        }

        r = sd_netlink_message_append_u32(m, IFLA_GRE_IKEY, ikey);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_IKEY attribute: %m");

        r = sd_netlink_message_append_u32(m, IFLA_GRE_OKEY, okey);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_OKEY attribute: %m");

        r = sd_netlink_message_append_u16(m, IFLA_GRE_IFLAGS, iflags);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_IFLAGS attribute: %m");

        r = sd_netlink_message_append_u16(m, IFLA_GRE_OFLAGS, oflags);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_OFLAGS, attribute: %m");

        if (t->fou_tunnel) {
                r = sd_netlink_message_append_u16(m, IFLA_GRE_ENCAP_TYPE, t->fou_encap_type);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_ENCAP_TYPE attribute: %m");

                r = sd_netlink_message_append_u16(m, IFLA_GRE_ENCAP_SPORT, htobe16(t->encap_src_port));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_ENCAP_SPORT attribute: %m");

                r = sd_netlink_message_append_u16(m, IFLA_GRE_ENCAP_DPORT, htobe16(t->fou_destination_port));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_ENCAP_DPORT attribute: %m");
        }

        return r;
}

static int netdev_ip6gre_fill_message_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        uint32_t ikey = 0;
        uint32_t okey = 0;
        uint16_t iflags = 0;
        uint16_t oflags = 0;
        Tunnel *t;
        int r;

        assert(netdev);

        if (netdev->kind == NETDEV_KIND_IP6GRE)
                t = IP6GRE(netdev);
        else
                t = IP6GRETAP(netdev);

        assert(t);
        assert(m);

        if (link || t->assign_to_loopback) {
                r = sd_netlink_message_append_u32(m, IFLA_GRE_LINK, link ? link->ifindex : LOOPBACK_IFINDEX);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_LINK attribute: %m");
        }

        r = sd_netlink_message_append_in6_addr(m, IFLA_GRE_LOCAL, &t->local.in6);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_LOCAL attribute: %m");

        r = sd_netlink_message_append_in6_addr(m, IFLA_GRE_REMOTE, &t->remote.in6);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_REMOTE attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_GRE_TTL, t->ttl);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_TTL attribute: %m");

        if (t->ipv6_flowlabel != _NETDEV_IPV6_FLOWLABEL_INVALID) {
                r = sd_netlink_message_append_u32(m, IFLA_GRE_FLOWINFO, t->ipv6_flowlabel);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_FLOWINFO attribute: %m");
        }

        r = sd_netlink_message_append_u32(m, IFLA_GRE_FLAGS, t->flags);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_FLAGS attribute: %m");

        if (t->key != 0) {
                ikey = okey = htobe32(t->key);
                iflags |= GRE_KEY;
                oflags |= GRE_KEY;
        }

        if (t->ikey != 0) {
                ikey = htobe32(t->ikey);
                iflags |= GRE_KEY;
        }

        if (t->okey != 0) {
                okey = htobe32(t->okey);
                oflags |= GRE_KEY;
        }

        r = sd_netlink_message_append_u32(m, IFLA_GRE_IKEY, ikey);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_IKEY attribute: %m");

        r = sd_netlink_message_append_u32(m, IFLA_GRE_OKEY, okey);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_OKEY attribute: %m");

        r = sd_netlink_message_append_u16(m, IFLA_GRE_IFLAGS, iflags);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_IFLAGS attribute: %m");

        r = sd_netlink_message_append_u16(m, IFLA_GRE_OFLAGS, oflags);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_GRE_OFLAGS, attribute: %m");

        return r;
}

static int netdev_vti_fill_message_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        uint32_t ikey, okey;
        Tunnel *t;
        int r;

        assert(netdev);
        assert(m);

        if (netdev->kind == NETDEV_KIND_VTI)
                t = VTI(netdev);
        else
                t = VTI6(netdev);

        assert(t);

        if (link || t->assign_to_loopback) {
                r = sd_netlink_message_append_u32(m, IFLA_VTI_LINK, link ? link->ifindex : LOOPBACK_IFINDEX);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_VTI_LINK attribute: %m");
        }

        if (t->key != 0)
                ikey = okey = htobe32(t->key);
        else {
                ikey = htobe32(t->ikey);
                okey = htobe32(t->okey);
        }

        r = sd_netlink_message_append_u32(m, IFLA_VTI_IKEY, ikey);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_VTI_IKEY attribute: %m");

        r = sd_netlink_message_append_u32(m, IFLA_VTI_OKEY, okey);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_VTI_OKEY attribute: %m");

        r = netlink_message_append_in_addr_union(m, IFLA_VTI_LOCAL, t->family, &t->local);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_VTI_LOCAL attribute: %m");

        r = netlink_message_append_in_addr_union(m, IFLA_VTI_REMOTE, t->family, &t->remote);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_VTI_REMOTE attribute: %m");

        return r;
}

static int netdev_ip6tnl_fill_message_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        Tunnel *t = IP6TNL(netdev);
        uint8_t proto;
        int r;

        assert(netdev);
        assert(m);
        assert(t);

        if (link || t->assign_to_loopback) {
                r = sd_netlink_message_append_u32(m, IFLA_IPTUN_LINK, link ? link->ifindex : LOOPBACK_IFINDEX);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_LINK attribute: %m");
        }

        r = sd_netlink_message_append_in6_addr(m, IFLA_IPTUN_LOCAL, &t->local.in6);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_LOCAL attribute: %m");

        r = sd_netlink_message_append_in6_addr(m, IFLA_IPTUN_REMOTE, &t->remote.in6);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_REMOTE attribute: %m");

        r = sd_netlink_message_append_u8(m, IFLA_IPTUN_TTL, t->ttl);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_TTL attribute: %m");

        if (t->ipv6_flowlabel != _NETDEV_IPV6_FLOWLABEL_INVALID) {
                r = sd_netlink_message_append_u32(m, IFLA_IPTUN_FLOWINFO, t->ipv6_flowlabel);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_FLOWINFO attribute: %m");
        }

        if (t->copy_dscp)
                t->flags |= IP6_TNL_F_RCV_DSCP_COPY;

        if (t->allow_localremote >= 0)
                SET_FLAG(t->flags, IP6_TNL_F_ALLOW_LOCAL_REMOTE, t->allow_localremote);

        if (t->encap_limit != 0) {
                r = sd_netlink_message_append_u8(m, IFLA_IPTUN_ENCAP_LIMIT, t->encap_limit);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_ENCAP_LIMIT attribute: %m");
        }

        r = sd_netlink_message_append_u32(m, IFLA_IPTUN_FLAGS, t->flags);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_FLAGS attribute: %m");

        switch (t->ip6tnl_mode) {
        case NETDEV_IP6_TNL_MODE_IP6IP6:
                proto = IPPROTO_IPV6;
                break;
        case NETDEV_IP6_TNL_MODE_IPIP6:
                proto = IPPROTO_IPIP;
                break;
        case NETDEV_IP6_TNL_MODE_ANYIP6:
        default:
                proto = 0;
                break;
        }

        r = sd_netlink_message_append_u8(m, IFLA_IPTUN_PROTO, proto);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPTUN_PROTO attribute: %m");

        return r;
}

static int netdev_tunnel_verify(NetDev *netdev, const char *filename) {
        Tunnel *t = NULL;

        assert(netdev);
        assert(filename);

        switch (netdev->kind) {
        case NETDEV_KIND_IPIP:
                t = IPIP(netdev);
                break;
        case NETDEV_KIND_SIT:
                t = SIT(netdev);
                break;
        case NETDEV_KIND_GRE:
                t = GRE(netdev);
                break;
        case NETDEV_KIND_GRETAP:
                t = GRETAP(netdev);
                break;
        case NETDEV_KIND_IP6GRE:
                t = IP6GRE(netdev);
                break;
        case NETDEV_KIND_IP6GRETAP:
                t = IP6GRETAP(netdev);
                break;
        case NETDEV_KIND_VTI:
                t = VTI(netdev);
                break;
        case NETDEV_KIND_VTI6:
                t = VTI6(netdev);
                break;
        case NETDEV_KIND_IP6TNL:
                t = IP6TNL(netdev);
                break;
        case NETDEV_KIND_ERSPAN:
                t = ERSPAN(netdev);
                break;
        default:
                assert_not_reached();
        }

        assert(t);

        if (IN_SET(netdev->kind, NETDEV_KIND_VTI, NETDEV_KIND_IPIP, NETDEV_KIND_SIT, NETDEV_KIND_GRE) &&
            !IN_SET(t->family, AF_UNSPEC, AF_INET))
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "vti/ipip/sit/gre tunnel without a local/remote IPv4 address configured in %s. Ignoring", filename);

        if (IN_SET(netdev->kind, NETDEV_KIND_GRETAP, NETDEV_KIND_ERSPAN) &&
            (t->family != AF_INET || !in_addr_is_set(t->family, &t->remote)))
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "gretap/erspan tunnel without a remote IPv4 address configured in %s. Ignoring", filename);

        if ((IN_SET(netdev->kind, NETDEV_KIND_VTI6, NETDEV_KIND_IP6TNL) && t->family != AF_INET6) ||
            (netdev->kind == NETDEV_KIND_IP6GRE && !IN_SET(t->family, AF_UNSPEC, AF_INET6)))
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "vti6/ip6tnl/ip6gre tunnel without a local/remote IPv6 address configured in %s. Ignoring", filename);

        if (netdev->kind == NETDEV_KIND_IP6GRETAP &&
            (t->family != AF_INET6 || !in_addr_is_set(t->family, &t->remote)))
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "ip6gretap tunnel without a remote IPv6 address configured in %s. Ignoring", filename);

        if (netdev->kind == NETDEV_KIND_IP6TNL &&
            t->ip6tnl_mode == _NETDEV_IP6_TNL_MODE_INVALID)
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "ip6tnl without mode configured in %s. Ignoring", filename);

        if (t->fou_tunnel && t->fou_destination_port <= 0)
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "FooOverUDP missing port configured in %s. Ignoring", filename);

        if (netdev->kind == NETDEV_KIND_ERSPAN && (t->erspan_index >= (1 << 20) || t->erspan_index == 0))
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL), "Invalid erspan index %d. Ignoring", t->erspan_index);

        /* netlink_message_append_in_addr_union() is used for vti/vti6. So, t->family cannot be AF_UNSPEC. */
        if (netdev->kind == NETDEV_KIND_VTI)
                t->family = AF_INET;

        return 0;
}

int config_parse_tunnel_address(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Tunnel *t = userdata;
        union in_addr_union *addr = data, buffer;
        int r, f;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        /* This is used to parse addresses on both local and remote ends of the tunnel.
         * Address families must match.
         *
         * "any" is a special value which means that the address is unspecified.
         */

        if (streq(rvalue, "any")) {
                *addr = IN_ADDR_NULL;

                /* As a special case, if both the local and remote addresses are
                 * unspecified, also clear the address family. */
                if (!in_addr_is_set(t->family, &t->local) &&
                    !in_addr_is_set(t->family, &t->remote))
                        t->family = AF_UNSPEC;
                return 0;
        }

        r = in_addr_from_string_auto(rvalue, &f, &buffer);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Tunnel address \"%s\" invalid, ignoring assignment: %m", rvalue);
                return 0;
        }

        if (t->family != AF_UNSPEC && t->family != f) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Tunnel addresses incompatible, ignoring assignment: %s", rvalue);
                return 0;
        }

        t->family = f;
        *addr = buffer;
        return 0;
}

int config_parse_tunnel_key(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        union in_addr_union buffer;
        Tunnel *t = userdata;
        uint32_t k;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = in_addr_from_string(AF_INET, rvalue, &buffer);
        if (r < 0) {
                r = safe_atou32(rvalue, &k);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to parse tunnel key ignoring assignment: %s", rvalue);
                        return 0;
                }
        } else
                k = be32toh(buffer.in.s_addr);

        if (streq(lvalue, "Key"))
                t->key = k;
        else if (streq(lvalue, "InputKey"))
                t->ikey = k;
        else
                t->okey = k;

        return 0;
}

int config_parse_ipv6_flowlabel(
                const char* unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        IPv6FlowLabel *ipv6_flowlabel = data;
        Tunnel *t = userdata;
        int k = 0;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(ipv6_flowlabel);

        if (streq(rvalue, "inherit")) {
                *ipv6_flowlabel = IP6_FLOWINFO_FLOWLABEL;
                t->flags |= IP6_TNL_F_USE_ORIG_FLOWLABEL;
        } else {
                r = config_parse_int(unit, filename, line, section, section_line, lvalue, ltype, rvalue, &k, userdata);
                if (r < 0)
                        return r;

                if (k > 0xFFFFF)
                        log_syntax(unit, LOG_WARNING, filename, line, 0, "Failed to parse IPv6 flowlabel option, ignoring: %s", rvalue);
                else {
                        *ipv6_flowlabel = htobe32(k) & IP6_FLOWINFO_FLOWLABEL;
                        t->flags &= ~IP6_TNL_F_USE_ORIG_FLOWLABEL;
                }
        }

        return 0;
}

int config_parse_encap_limit(
                const char* unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Tunnel *t = userdata;
        int k = 0;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        if (streq(rvalue, "none"))
                t->flags |= IP6_TNL_F_IGN_ENCAP_LIMIT;
        else {
                r = safe_atoi(rvalue, &k);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r, "Failed to parse Tunnel Encapsulation Limit option, ignoring: %s", rvalue);
                        return 0;
                }

                if (k > 255 || k < 0)
                        log_syntax(unit, LOG_WARNING, filename, line, 0, "Invalid Tunnel Encapsulation value, ignoring: %d", k);
                else {
                        t->encap_limit = k;
                        t->flags &= ~IP6_TNL_F_IGN_ENCAP_LIMIT;
                }
        }

        return 0;
}

int config_parse_6rd_prefix(
                const char* unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Tunnel *t = userdata;
        union in_addr_union p;
        uint8_t l;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        r = in_addr_prefix_from_string(rvalue, AF_INET6, &p, &l);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Failed to parse 6rd prefix \"%s\", ignoring: %m", rvalue);
                return 0;
        }
        if (l == 0) {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "6rd prefix length of \"%s\" must be greater than zero, ignoring", rvalue);
                return 0;
        }

        t->sixrd_prefix = p.in6;
        t->sixrd_prefixlen = l;

        return 0;
}

static void ipip_sit_init(NetDev *n) {
        Tunnel *t;

        assert(n);

        switch (n->kind) {
        case NETDEV_KIND_IPIP:
                t = IPIP(n);
                break;
        case NETDEV_KIND_SIT:
                t = SIT(n);
                break;
        default:
                assert_not_reached();
        }

        assert(t);

        t->pmtudisc = true;
        t->fou_encap_type = NETDEV_FOO_OVER_UDP_ENCAP_DIRECT;
        t->isatap = -1;
}

static void vti_init(NetDev *n) {
        Tunnel *t;

        assert(n);

        if (n->kind == NETDEV_KIND_VTI)
                t = VTI(n);
        else
                t = VTI6(n);

        assert(t);

        t->pmtudisc = true;
}

static void gre_erspan_init(NetDev *n) {
        Tunnel *t;

        assert(n);

        switch (n->kind) {
        case NETDEV_KIND_GRE:
                t = GRE(n);
                break;
        case NETDEV_KIND_ERSPAN:
                t = ERSPAN(n);
                break;
        case NETDEV_KIND_GRETAP:
                t = GRETAP(n);
                break;
        default:
                assert_not_reached();
        }

        assert(t);

        t->pmtudisc = true;
        t->gre_erspan_sequence = -1;
        t->fou_encap_type = NETDEV_FOO_OVER_UDP_ENCAP_DIRECT;
}

static void ip6gre_init(NetDev *n) {
        Tunnel *t;

        assert(n);

        if (n->kind == NETDEV_KIND_IP6GRE)
                t = IP6GRE(n);
        else
                t = IP6GRETAP(n);

        assert(t);

        t->ttl = DEFAULT_TNL_HOP_LIMIT;
}

static void ip6tnl_init(NetDev *n) {
        Tunnel *t = IP6TNL(n);

        assert(n);
        assert(t);

        t->ttl = DEFAULT_TNL_HOP_LIMIT;
        t->encap_limit = IPV6_DEFAULT_TNL_ENCAP_LIMIT;
        t->ip6tnl_mode = _NETDEV_IP6_TNL_MODE_INVALID;
        t->ipv6_flowlabel = _NETDEV_IPV6_FLOWLABEL_INVALID;
        t->allow_localremote = -1;
}

const NetDevVTable ipip_vtable = {
        .object_size = sizeof(Tunnel),
        .init = ipip_sit_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_ipip_sit_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_TUNNEL,
};

const NetDevVTable sit_vtable = {
        .object_size = sizeof(Tunnel),
        .init = ipip_sit_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_ipip_sit_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_SIT,
};

const NetDevVTable vti_vtable = {
        .object_size = sizeof(Tunnel),
        .init = vti_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_vti_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_TUNNEL,
};

const NetDevVTable vti6_vtable = {
        .object_size = sizeof(Tunnel),
        .init = vti_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_vti_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_TUNNEL6,
};

const NetDevVTable gre_vtable = {
        .object_size = sizeof(Tunnel),
        .init = gre_erspan_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_gre_erspan_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_IPGRE,
};

const NetDevVTable gretap_vtable = {
        .object_size = sizeof(Tunnel),
        .init = gre_erspan_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_gre_erspan_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_ETHER,
        .generate_mac = true,
};

const NetDevVTable ip6gre_vtable = {
        .object_size = sizeof(Tunnel),
        .init = ip6gre_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_ip6gre_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_IP6GRE,
};

const NetDevVTable ip6gretap_vtable = {
        .object_size = sizeof(Tunnel),
        .init = ip6gre_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_ip6gre_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_ETHER,
        .generate_mac = true,
};

const NetDevVTable ip6tnl_vtable = {
        .object_size = sizeof(Tunnel),
        .init = ip6tnl_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_ip6tnl_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_TUNNEL6,
};

const NetDevVTable erspan_vtable = {
        .object_size = sizeof(Tunnel),
        .init = gre_erspan_init,
        .sections = NETDEV_COMMON_SECTIONS "Tunnel\0",
        .fill_message_create = netdev_gre_erspan_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .config_verify = netdev_tunnel_verify,
        .iftype = ARPHRD_ETHER,
        .generate_mac = true,
};
