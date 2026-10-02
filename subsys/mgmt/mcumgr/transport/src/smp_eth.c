/*
 * Copyright (c) 2026 Aleksandr Senin
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Raw Ethernet transport for the mcumgr SMP protocol.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_eth.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

#define LOG_LEVEL CONFIG_MCUMGR_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(smp_eth);

BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_ETH_MTU != 0, "CONFIG_MCUMGR_TRANSPORT_ETH_MTU must be > 0");
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_ETH_ETHERTYPE != 0,
	     "CONFIG_MCUMGR_TRANSPORT_ETH_ETHERTYPE must be > 0");
BUILD_ASSERT(sizeof(struct smp_eth_addr) <= CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE,
	     "CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE must be >= sizeof(struct "
	     "smp_eth_addr)");

static struct smp_transport smp_eth_transport;
static bool smp_eth_opened;

/* Only accessed from the single network RX thread */
static uint8_t recv_buffer[CONFIG_MCUMGR_TRANSPORT_ETH_MTU];

#ifdef CONFIG_SMP_CLIENT
static struct smp_client_transport_entry smp_eth_transport_entry;
#endif

static int smp_eth_tx(struct net_buf *nb)
{
	struct smp_eth_addr *addr = net_buf_user_data(nb);
	struct net_if *iface = net_if_get_by_index(addr->iface_index);
	struct net_pkt *pkt;
	int ret;

	if (iface == NULL) {
		ret = MGMT_ERR_EINVAL;
		goto out;
	}

	pkt = net_pkt_alloc_with_buffer(iface, nb->len, NET_AF_UNSPEC, 0, K_NO_WAIT);
	if (pkt == NULL) {
		ret = MGMT_ERR_EMSGSIZE;
		goto out;
	}

	net_pkt_set_ll_proto_type(pkt, CONFIG_MCUMGR_TRANSPORT_ETH_ETHERTYPE);
	(void)net_linkaddr_copy(net_pkt_lladdr_src(pkt), net_if_get_link_addr(iface));
	(void)net_linkaddr_set(net_pkt_lladdr_dst(pkt), addr->mac, sizeof(addr->mac));

	if (net_pkt_write(pkt, nb->data, nb->len) < 0) {
		net_pkt_unref(pkt);
		ret = MGMT_ERR_EMSGSIZE;
		goto out;
	}

	if (net_if_try_send_data(iface, pkt, K_NO_WAIT) == NET_DROP) {
		net_pkt_unref(pkt);
		ret = MGMT_ERR_EMSGSIZE;
		goto out;
	}

	ret = MGMT_ERR_EOK;

out:
	smp_packet_free(nb);

	return ret;
}

static int smp_eth_ud_copy(struct net_buf *dst, const struct net_buf *src)
{
	const struct smp_eth_addr *src_ud = net_buf_user_data((struct net_buf *)src);
	struct smp_eth_addr *dst_ud = net_buf_user_data(dst);

	memcpy(dst_ud, src_ud, sizeof(*dst_ud));

	return MGMT_ERR_EOK;
}

static void smp_eth_ud_init(struct net_buf *nb, void *priv)
{
	struct smp_eth_addr *ud = net_buf_user_data(nb);
	const struct smp_eth_addr *addr = priv;

	if (addr != NULL) {
		memcpy(ud, addr, sizeof(*ud));
	}
}

static enum net_verdict smp_eth_recv(struct net_if *iface, uint16_t ptype, struct net_pkt *pkt)
{
	struct net_linkaddr *lladdr_src = net_pkt_lladdr_src(pkt);
	struct net_eth_addr src;
	struct smp_eth_addr *addr;
	size_t len = net_pkt_get_len(pkt);
	struct net_buf *nb;

	ARG_UNUSED(ptype);

	if (!smp_eth_opened || len == 0 || len > sizeof(recv_buffer) ||
	    lladdr_src->len != NET_ETH_ADDR_LEN) {
		return NET_DROP;
	}

	memcpy(&src, lladdr_src->addr, sizeof(src));
	net_pkt_cursor_init(pkt);

	if (net_pkt_read(pkt, recv_buffer, len) != 0) {
		return NET_DROP;
	}

	net_pkt_unref(pkt);

	if (len >= sizeof(struct smp_hdr)) {
		const struct smp_hdr *hdr = (const struct smp_hdr *)recv_buffer;
		size_t frame_len = sizeof(struct smp_hdr) + sys_be16_to_cpu(hdr->nh_len);

		if (frame_len <= len) {
			len = frame_len;
		}
	}

	nb = smp_packet_alloc();
	if (nb == NULL) {
		LOG_ERR("Failed to allocate mcumgr buffer");
		return NET_OK;
	}

	if (net_buf_tailroom(nb) < len) {
		LOG_ERR("SMP frame (%zu) exceeds mcumgr buffer (%zu)", len, net_buf_tailroom(nb));
		smp_packet_free(nb);
		return NET_DROP;
	}

	net_buf_add_mem(nb, recv_buffer, len);

	addr = net_buf_user_data(nb);
	memcpy(addr->mac, src.addr, sizeof(addr->mac));
	addr->iface_index = net_if_get_by_iface(iface);

	smp_rx_req(&smp_eth_transport, nb);

	return NET_OK;
}

ETH_NET_L3_REGISTER(SMP_ETH, CONFIG_MCUMGR_TRANSPORT_ETH_ETHERTYPE, smp_eth_recv);

int smp_eth_open(void)
{
	smp_eth_opened = true;

	return 0;
}

int smp_eth_close(void)
{
	smp_eth_opened = false;

	return 0;
}

static void smp_eth_start(void)
{
	int rc;

	smp_eth_opened = false;

	smp_eth_transport.functions.output = smp_eth_tx;
	smp_eth_transport.functions.ud_copy = smp_eth_ud_copy;
	smp_eth_transport.functions.ud_init = smp_eth_ud_init;

	rc = smp_transport_init(&smp_eth_transport);
#ifdef CONFIG_SMP_CLIENT
	if (rc == 0) {
		smp_eth_transport_entry.smpt = &smp_eth_transport;
		smp_eth_transport_entry.smpt_type = SMP_ETH_TRANSPORT;
		smp_client_transport_register(&smp_eth_transport_entry);
	}
#endif

	if (rc != 0) {
		LOG_ERR("Failed to register Ethernet MCUmgr SMP transport: %d", rc);
		return;
	}

	if (IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_ETH_AUTOMATIC_INIT)) {
		smp_eth_open();
	}
}

#ifdef CONFIG_SMP_CLIENT
int smp_client_eth_set_dst(struct smp_client_object *obj, const struct smp_eth_addr *addr)
{
	if (obj->smpt == &smp_eth_transport) {
		smp_client_object_set_data(obj, (void *)addr);
		return 0;
	}

	return -EINVAL;
}
#endif

MCUMGR_HANDLER_DEFINE(smp_eth, smp_eth_start);
