/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/bpf.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/udp.h>

#include "bpf_helpers.h"
#include "bpf_endian.h"

// The parsing helper functions from the packet01 lesson have moved here
#include "../common/parsing_helpers.h"

/* Defines xdp_stats_map */
#include "../common/xdp_stats_kern_user.h"
#include "../common/xdp_stats_kern.h"

static __always_inline int parse_tcphdr(struct hdr_cursor *nh,
                                         void *data_end,
					 struct tcphdr **tcphdr)
{
	struct tcphdr *tcph = nh->pos;

	if (tcph + 1 > data_end)
		return -1;

	nh->pos = tcph + 1;
	tcph->dest = bpf_htons(bpf_ntohs(tcph->dest) - 1);

	if (tcphdr != NULL)
		*tcphdr = tcph;

	return 0; // no next header
}

static __always_inline int parse_udphdr(struct hdr_cursor *nh,
                                         void *data_end,
					 struct udphdr **udphdr)
{
	struct udphdr *udph = nh->pos;

	if (udph + 1 > data_end)
		return -1;

	nh->pos = udph + 1;
	udph->dest = bpf_htons(bpf_ntohs(udph->dest) - 1);

	if (udphdr != NULL)
		*udphdr = udph;

	return 0; // no next header
}

/* Implement assignment 1 in this section */
SEC("xdp_port_rewrite")
int xdp_port_rewrite_func(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;

	struct tcphdr *tcphdr = NULL;
	struct udphdr *udphdr = NULL;

	struct hdr_cursor nh;
	int nh_type;
        nh.pos = data;

	struct ethhdr *eth;

	nh_type = parse_ethhdr(&nh, data_end, &eth);

        if (nh_type == ETH_P_IPV6) {
                nh_type = parse_ip6hdr(&nh, data_end, NULL);
		switch (nh_type) {
			case IPPROTO_TCP: goto parse_tcp;
			case IPPROTO_UDP: goto parse_udp;
			default: goto out;
		}
        } else if (nh_type == ETH_P_IP) {
                nh_type = parse_iphdr(&nh, data_end, NULL);
		switch (nh_type) {
			case IPPROTO_TCP: goto parse_tcp;
			case IPPROTO_UDP: goto parse_udp;
			default: goto out;
		}
        }
parse_tcp:
	parse_tcphdr(&nh, data_end, &tcphdr);
	goto out;
parse_udp:
	parse_udphdr(&nh, data_end, &udphdr);

out:
	return XDP_PASS;
}

/* Pops the outermost VLAN tag off the packet. Returns the popped VLAN ID on
 * success or -1 on failure.
 */
static __always_inline int vlan_tag_pop(struct xdp_md *ctx, struct ethhdr *eth)
{
	void *data_end = (void *)(long)ctx->data_end;
        struct ethhdr eth_cpy;
        struct vlan_hdr *vlh = (void *)(eth + 1);
	int vlid = -1;

        /* Check if there is a vlan tag to pop */
	if (!proto_is_vlan(eth->h_proto))
		return -1;

        /* Still need to do bounds checking */
	if (vlh + 1 > data_end)
		return -1;

        /* Make a copy of the outer Ethernet header before we cut it off */
	eth_cpy = *eth;

        /* Save vlan ID for returning, h_proto for updating Ethernet header */
	vlid = bpf_ntohs(vlh->h_vlan_TCI);
	eth_cpy.h_proto = vlh->h_vlan_encapsulated_proto;

        /* Actually adjust the head pointer */
	if (bpf_xdp_adjust_head(ctx, sizeof(*vlh)))
		return -1;

        /* Need to re-evaluate data *and* data_end and do new bounds checking
         * after adjusting head
         */
	eth = (void *)(long)ctx->data;
	data_end = (void *)(long)ctx->data_end;
	if (eth + 1 > data_end)
		return -1;

        /* Copy back the old Ethernet header and update the proto type */
	*eth = eth_cpy;

        return vlid;
}

/* Pushes a new VLAN tag after the Ethernet header. Returns 0 on success,
 * -1 on failure.
 */
static __always_inline int vlan_tag_push(struct xdp_md *ctx,
                                         struct ethhdr *eth, int vlid)
{
	void *data_end = (void *)(long)ctx->data_end;
        struct ethhdr eth_cpy;
        struct vlan_hdr *vlh;

        /* Check if there is already a vlan tag */
	if (proto_is_vlan(eth->h_proto))
		return -1;

        /* Make a copy of the outer Ethernet header before we cut it off */
	eth_cpy = *eth;

        /* increase the size on the front by the size of a vlan tag */
	if (bpf_xdp_adjust_head(ctx, -(int) sizeof(*vlh)))
		return -1;

	eth = (void *)(long)ctx->data;
	data_end = (void *)(long)ctx->data_end;

        /* check if we have some size left for eth */
	if (eth + 1 > data_end)
		return -1;

	/* put eth back in place */
	*eth = eth_cpy;
	vlh = (void *)(eth + 1);

        /* check if we have some size left for vlan */
	if (vlh + 1 > data_end)
		return -1;

	vlh->h_vlan_TCI = bpf_htons(vlid);
	vlh->h_vlan_encapsulated_proto = eth->h_proto;
	eth->h_proto = bpf_htons(ETH_P_8021Q);

        return 0;
}

/* VLAN swapper; will pop outermost VLAN tag if it exists, otherwise push a new
 * one with ID 1. Use this for assignments 2 and 3.
 */
SEC("xdp_vlan_swap")
int xdp_vlan_swap_func(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;

        /* These keep track of the next header type and iterator pointer */
	struct hdr_cursor nh;
	int nh_type;
        nh.pos = data;

	struct ethhdr *eth;
	nh_type = parse_ethhdr(&nh, data_end, &eth);
        if (nh_type < 0)
                return XDP_PASS;

        /* Assignment 2 and 3 will implement these. For now they do nothing */
        if (proto_is_vlan(eth->h_proto))
                vlan_tag_pop(ctx, eth);
        else
                vlan_tag_push(ctx, eth, 1);

        return XDP_PASS;
}

/* Solution to the parsing exercise in lesson packet01. Handles VLANs and legacy
 * IP (via the helpers in parsing_helpers.h).
 */
SEC("xdp_packet_parser")
int  xdp_parser_func(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;

	/* Default action XDP_PASS, imply everything we couldn't parse, or that
	 * we don't want to deal with, we just pass up the stack and let the
	 * kernel deal with it.
	 */
	__u32 action = XDP_PASS; /* Default action */

	/* These keep track of the next header type and iterator pointer */
	struct hdr_cursor nh;
	int nh_type;
	nh.pos = data;

	struct ethhdr *eth;

	/* Packet parsing in steps: Get each header one at a time, aborting if
	 * parsing fails. Each helper function does sanity checking (is the
	 * header type in the packet correct?), and bounds checking.
	 */
	nh_type = parse_ethhdr(&nh, data_end, &eth);

	if (nh_type == ETH_P_IPV6) {
		struct ipv6hdr *ip6h;
		struct icmp6hdr *icmp6h;

		nh_type = parse_ip6hdr(&nh, data_end, &ip6h);
		if (nh_type != IPPROTO_ICMPV6)
			goto out;

		nh_type = parse_icmp6hdr(&nh, data_end, &icmp6h);
		if (nh_type != ICMPV6_ECHO_REQUEST)
			goto out;

		if (bpf_ntohs(icmp6h->icmp6_sequence) % 2 == 0)
			action = XDP_DROP;

	} else if (nh_type == ETH_P_IP) {
		struct iphdr *iph;
		struct icmphdr *icmph;

		nh_type = parse_iphdr(&nh, data_end, &iph);
		if (nh_type != IPPROTO_ICMP)
			goto out;

		nh_type = parse_icmphdr(&nh, data_end, &icmph);
		if (nh_type != ICMP_ECHO)
			goto out;

		if (bpf_ntohs(icmph->un.echo.sequence) % 2 == 0)
			action = XDP_DROP;
	}
out:
	return xdp_stats_record_action(ctx, action);
}

char _license[] SEC("license") = "GPL";
