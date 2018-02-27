/*
 * A tool for functional testing netmap transmission and reception.
 *
 * Copyright (C) 2018 Vincenzo Maffione. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <net/ethernet.h>
#include <net/if.h>
#include <net/netmap.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#define ETH_ADDR_LEN 6

struct Global {
	struct nm_desc *nmd;
	const char *ifname;
	unsigned wait_link_secs; /* wait for link */
	unsigned timeout_secs;   /* receive timeout */

#define MAX_PKT_SIZE 65536
	char pktm[MAX_PKT_SIZE]; /* packet model */
	unsigned pktm_len;       /* packet model length */
	char pktr[MAX_PKT_SIZE]; /* packet received */
	unsigned pktr_len;       /* length of received packet */

	char src_mac[ETH_ADDR_LEN];
	char dst_mac[ETH_ADDR_LEN];
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t src_port;
	uint16_t dst_port;
	char filler;
};

static void
fill_packet_field(struct Global *g, unsigned offset, const char *content,
		  unsigned content_len)
{
	if (offset + content_len > sizeof(g->pktm)) {
		printf("Packet layout overflow: %u + %u > %lu\n", offset,
		       content_len, sizeof(g->pktm));
		exit(EXIT_FAILURE);
	}

	memcpy(g->pktm + offset, content, content_len);
}

static void
fill_packet_8bit(struct Global *g, unsigned offset, uint8_t val)
{
	fill_packet_field(g, offset, (const char *)&val, sizeof(val));
}

static void
fill_packet_16bit(struct Global *g, unsigned offset, uint16_t val)
{
	val = htons(val);
	fill_packet_field(g, offset, (const char *)&val, sizeof(val));
}

static void
fill_packet_32bit(struct Global *g, unsigned offset, uint32_t val)
{
	val = htonl(val);
	fill_packet_field(g, offset, (const char *)&val, sizeof(val));
}

/* Compute the checksum of the given ip header. */
static uint32_t
checksum(const void *data, uint16_t len, uint32_t sum /* host endianness */)
{
	const uint8_t *addr = data;
	uint32_t i;

	/* Checksum all the pairs of bytes first... */
	for (i = 0; i < (len & ~1U); i += 2) {
		sum += (u_int16_t)ntohs(*((u_int16_t *)(addr + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	/*
	 * If there's a single byte left over, checksum it, too.
	 * Network byte order is big-endian, so the remaining byte is
	 * the high byte.
	 */
	if (i < len) {
		sum += addr[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	return sum;
}

static uint16_t
wrapsum(uint32_t sum /* host endianness */)
{
	sum = ~sum & 0xFFFF;
	return sum; /* host endianness */
}

static void
build_packet(struct Global *g)
{
	unsigned ofs = 0;
	unsigned ethofs;
	unsigned ipofs;
	unsigned udpofs;
	unsigned pldofs;

	memset(g->pktm, 0, sizeof(g->pktm));
	printf("%s: starting at ofs %u\n", __func__, ofs);

	ethofs = ofs;
	(void)ethofs;
	/* Ethernet destination and source MAC address plus ethertype. */
	fill_packet_field(g, ofs, g->dst_mac, ETH_ADDR_LEN);
	ofs += ETH_ADDR_LEN;
	fill_packet_field(g, ofs, g->src_mac, ETH_ADDR_LEN);
	ofs += ETH_ADDR_LEN;
	fill_packet_16bit(g, ofs, ETHERTYPE_IP);
	ofs += 2;
	printf("%s: eth done, ofs %u\n", __func__, ofs);

	ipofs = ofs;
	/* First byte of IP header. */
	fill_packet_8bit(g, ofs,
			 (IPVERSION << 4) | ((sizeof(struct iphdr)) >> 2));
	ofs += 1;
	/* Skip QoS byte. */
	ofs += 1;
	/* Total length. */
	fill_packet_16bit(g, ofs, g->pktm_len - ipofs);
	ofs += 2;
	/* Skip identification field. */
	ofs += 2;
	/* Offset (and flags) field. */
	fill_packet_16bit(g, ofs, IP_DF);
	ofs += 2;
	/* TTL. */
	fill_packet_8bit(g, ofs, IPDEFTTL);
	ofs += 1;
	/* Protocol. */
	fill_packet_8bit(g, ofs, IPPROTO_UDP);
	ofs += 1;
	/* Skip checksum for now. */
	ofs += 2;
	/* Source IP address. */
	fill_packet_32bit(g, ofs, g->src_ip);
	ofs += 4;
	/* Dst IP address. */
	fill_packet_32bit(g, ofs, g->dst_ip);
	ofs += 4;
	/* Now put the checksum. */
	fill_packet_16bit(
		g, ipofs + 10,
		wrapsum(checksum(g->pktm + ipofs, sizeof(struct iphdr), 0)));
	printf("%s: ip done, ofs %u\n", __func__, ofs);

	udpofs = ofs;
	/* UDP source port. */
	fill_packet_16bit(g, ofs, g->src_port);
	ofs += 2;
	/* UDP source port. */
	fill_packet_16bit(g, ofs, g->dst_port);
	ofs += 2;
	/* UDP length (UDP header + data). */
	fill_packet_16bit(g, ofs, g->pktm_len - udpofs);
	ofs += 2;
	/* Skip the UDP checksum for now. */
	ofs += 2;
	printf("%s: udp done, ofs %u\n", __func__, ofs);

	/* Fill UDP payload. */
	pldofs = ofs;
	for (; ofs < g->pktm_len; ofs++) {
		fill_packet_8bit(g, ofs, g->filler);
	}
	printf("%s: payload done, ofs %u\n", __func__, ofs);

	/* Put the UDP checksum now.
	 * Magic: taken from sbin/dhclient/packet.c */
	fill_packet_16bit(
		g, udpofs + 6,
		wrapsum(checksum(
			/* udp header */ g->pktm + udpofs,
			sizeof(struct udphdr),
			checksum(/* udp payload */ g->pktm + pldofs,
				 g->pktm_len - pldofs,
				 checksum(/* pseudo header */ g->pktm + ipofs +
						  12,
					  2 * sizeof(g->src_ip),
					  IPPROTO_UDP + (uint32_t)(g->pktm_len -
								   udpofs))))));
}

/* Transmit a single packet using any TX ring. */
static int
tx_one(struct Global *g)
{
	struct nm_desc *nmd = g->nmd;
	unsigned int i;

	for (;;) {
		for (i = nmd->first_tx_ring; i <= nmd->last_tx_ring; i++) {
			struct netmap_ring *ring = NETMAP_TXRING(nmd->nifp, i);
			struct netmap_slot *slot = &ring->slot[ring->head];
			char *buf = NETMAP_BUF(ring, slot->buf_idx);

			if (nm_ring_empty(ring)) {
				continue;
			}
			if (g->pktm_len > ring->nr_buf_size) {
				/* Sanity check. */
				printf("Error: len (%u) > netmap_buf_size "
				       "(%u)\n",
				       g->pktm_len, ring->nr_buf_size);
				exit(EXIT_FAILURE);
			}
			memcpy(buf, g->pktm, g->pktm_len);
			slot->len   = g->pktm_len;
			slot->flags = NS_REPORT;
			ring->head = ring->cur = nm_ring_next(ring, ring->head);
			ioctl(nmd->fd, NIOCTXSYNC, NULL);
			printf("packet (%u bytes) transmitted to TX ring #%d\n",
			       slot->len, i);
			return 0;
		}

		/* Retry after a short while. */
		usleep(100000);
		ioctl(nmd->fd, NIOCTXSYNC, NULL);
	}

	/* never reached */
	return 0;
}

/* Receive a single packet from any RX ring. */
static int
rx_one(struct Global *g)
{
	struct nm_desc *nmd = g->nmd;
	unsigned elapsed_ms = 0;
	unsigned wait_ms    = 100;
	unsigned int i;

	for (;;) {
		for (i = nmd->first_rx_ring; i <= nmd->last_rx_ring; i++) {
			struct netmap_ring *ring = NETMAP_RXRING(nmd->nifp, i);
			unsigned int head	= ring->head;
			unsigned int frags       = 0;

			if (nm_ring_empty(ring)) {
				continue;
			}

			g->pktr_len = 0;
			for (;;) {
				struct netmap_slot *slot = &ring->slot[head];
				char *buf = NETMAP_BUF(ring, slot->buf_idx);

				if (g->pktr_len + slot->len > sizeof(g->pktr)) {
					/* Sanity check. */
					printf("Error: received packet too "
					       "large "
					       "(>= %u bytes) ",
					       g->pktr_len + slot->len);
					exit(EXIT_FAILURE);
				}
				memcpy(g->pktr + g->pktr_len, buf, slot->len);
				g->pktr_len += slot->len;
				head = nm_ring_next(ring, head);
				frags++;
				if (!(slot->flags & NS_MOREFRAG)) {
					break;
				}
			}
			ring->head = ring->cur = head;
			ioctl(nmd->fd, NIOCRXSYNC, NULL);
			printf("packet (%u bytes, %u frags) received from RX "
			       "ring #%d\n",
			       g->pktr_len, frags, i);
			return 0;
		}

		if (elapsed_ms / 1000 > g->timeout_secs) {
			printf("%s: Timeout\n", __func__);
			return -1;
		}

		/* Retry after a short while. */
		usleep(wait_ms * 1000);
		elapsed_ms += wait_ms;
		ioctl(nmd->fd, NIOCRXSYNC, NULL);
	}

	return 0;
}

static int
rx_check(struct Global *g)
{
	unsigned i;

	if (g->pktr_len != g->pktm_len) {
		printf("Received packet length (%u) different from "
		       "expected (%u bytes)\n",
		       g->pktr_len, g->pktm_len);
		return -1;
	}

	for (i = 0; i < g->pktr_len; i++) {
		if (g->pktr[i] != g->pktm[i]) {
			printf("Received packet differs from model at "
			       "offset %u (%x!=%x)\n",
			       i, g->pktr[i], g->pktm[i]);
			return -1;
		}
	}

	return 0;
}

static struct Global _g;

static void
usage(void)
{
	printf("usage: ./functional [-h]\n"
	       "-i NETMAP_PORT\n"
	       "[-l PACKET_LEN (=6)]\n"
	       "[-w WAIT_LINK_SECS (=0)]\n");
}

int
main(int argc, char **argv)
{
	struct Global *g = &_g;
	int opt;
	int i;

	g->ifname	 = NULL;
	g->nmd		  = NULL;
	g->wait_link_secs = 0;
	g->pktm_len       = 60;
	for (i = 0; i < ETH_ADDR_LEN; i++)
		g->src_mac[i] = 0x00;
	for (i = 0; i < ETH_ADDR_LEN; i++)
		g->dst_mac[i] = 0xFF;
	g->src_ip = 0x0A000005; /* 10.0.0.5 */
	g->dst_ip = 0x0A000007; /* 10.0.0.7 */
	g->filler = 'a';

	while ((opt = getopt(argc, argv, "hi:w:l:")) != -1) {
		switch (opt) {
		case 'h':
			usage();
			return 0;

		case 'i':
			g->ifname = optarg;
			break;

		case 'w':
			g->wait_link_secs = atoi(optarg);
			break;

		case 'l':
			g->pktm_len = atoi(optarg);
			break;

		default:
			printf("    Unrecognized option %c\n", opt);
			usage();
			return -1;
		}
	}

	if (!g->ifname) {
		printf("Missing ifname\n");
		usage();
		return -1;
	}

	g->nmd = nm_open(g->ifname, NULL, 0, NULL);
	if (g->nmd == NULL) {
		printf("Failed to nm_open(%s)\n", g->ifname);
		return -1;
	}
	if (g->wait_link_secs > 0) {
		sleep(g->wait_link_secs);
	}

	build_packet(g);

	tx_one(g);
	if (rx_one(g)) {
		return -1;
	}
	if (rx_check(g)) {
		return -1;
	}

	nm_close(g->nmd);

	return 0;
}
