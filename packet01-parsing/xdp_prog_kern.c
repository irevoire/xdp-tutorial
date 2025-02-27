/* SPDX-License-Identifier: GPL-2.0 */
#include <stddef.h>
#include <linux/bpf.h>
#include <linux/in.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ipv6.h>
#include <linux/icmpv6.h>
#include "bpf_helpers.h"
#include "bpf_endian.h"
/* Defines xdp_stats_map from packet04 */
#include "../common/xdp_stats_kern_user.h"
#include "../common/xdp_stats_kern.h"

/* Header cursor to keep track of current parsing position */
struct hdr_cursor {
	void *pos;
};

struct vlan_hdr {
	__be16	h_vlan_TCI;
	__be16	h_vlan_encapsulated_proto;
};

/* Packet parsing helpers.
 *
 * Each helper parses a packet header, including doing bounds checking, and
 * returns the type of its contents if successful, and -1 otherwise.
 *
 * For Ethernet and IP headers, the content type is the type of the payload
 * (h_proto for Ethernet, nexthdr for IPv6), for ICMP it is the ICMP type field.
 * All return values are in host byte order.
 */
static __always_inline int parse_ethhdr(struct hdr_cursor *nh,
					void *data_end,
					struct ethhdr **ethhdr)
{
	struct ethhdr *eth = nh->pos;
	int hdrsize = sizeof(*eth);

	if (nh->pos + hdrsize > data_end)
		return -1;

	nh->pos += hdrsize;
	if (ethhdr != NULL)
		*ethhdr = eth;

	return bpf_ntohs(eth->h_proto);
}

static __always_inline int parse_vlanhdr(struct hdr_cursor *nh,
					void *data_end,
					struct vlan_hdr **vlanhdr)
{
	struct vlan_hdr *vlan = nh->pos;
	int hdrsize = sizeof(*vlan);

	if (nh->pos + hdrsize > data_end)
		return -1;

	nh->pos += hdrsize;
	if (vlanhdr != NULL)
		*vlanhdr = vlan;

	return bpf_ntohs(vlan->h_vlan_encapsulated_proto);
}


/* Assignment 2: Implement and use this */
static __always_inline int parse_ip6hdr(struct hdr_cursor *nh,
					void *data_end,
					struct ipv6hdr **ip6hdr)
{
	struct ipv6hdr *ip = nh->pos;
	int hdrsize = sizeof(*ip);

	if (nh->pos + hdrsize > data_end)
		return -1;

	nh->pos += hdrsize;
	if (ip6hdr != NULL)
		*ip6hdr = ip;

	return bpf_ntohs(ip->nexthdr);
}

/* Assignment 3: Implement and use this */
static __always_inline int parse_icmp6hdr(struct hdr_cursor *nh,
					  void *data_end,
					  struct icmp6hdr **icmp6hdr)
{
	struct icmp6hdr *icmp = nh->pos;
	int hdrsize = sizeof(*icmp);

	if (nh->pos + hdrsize > data_end)
		return -1;

	nh->pos += hdrsize;
	if (icmp6hdr != NULL)
		*icmp6hdr = icmp;

	return 0; // there is no next_header in icmp
}

SEC("xdp_packet_parser")
int  xdp_parser_func(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;

	struct icmp6hdr *icmp6;

	/* Default action XDP_PASS, imply everything we couldn't parse, or that
	 * we don't want to deal with, we just pass up the stack and let the
	 * kernel deal with it.
	 */
	__u32 action = XDP_PASS; /* Default action */

        /* These keep track of the next header type and iterator pointer */
	struct hdr_cursor nh;
	int nh_type;

	/* Start next header cursor position at data start */
	nh.pos = data;

	/* Packet parsing in steps: Get each header one at a time, aborting if
	 * parsing fails. Each helper function does sanity checking (is the
	 * header type in the packet correct?), and bounds checking.
	 */
	nh_type = parse_ethhdr(&nh, data_end, NULL);
	switch (nh_type) {
		case ETH_P_IPV6: break;
		case ETH_P_8021Q: /* FALLTHROUGH */
		case ETH_P_8021AD:
				 nh_type = parse_vlanhdr(&nh, data_end, NULL);
				 break;
		default: goto out;
	}

	/* Assignment additions go below here */
	nh_type = parse_ip6hdr(&nh, data_end, NULL);
	if (nh_type != 0x3A00) // ICMPv6 TODO find a definition of this somewhere
		goto out;

	nh_type = parse_icmp6hdr(&nh, data_end, &icmp6);
	if (nh_type != 0)
		goto out;
	if (!!(bpf_ntohs(icmp6->icmp6_sequence) & 1))
		action = XDP_DROP;

out:
	return xdp_stats_record_action(ctx, action); /* read via xdp_stats */
}

char _license[] SEC("license") = "GPL";
