/* AVB test utilities */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWire contributors */
/* SPDX-License-Identifier: MIT */

#ifndef TEST_AVB_UTILS_H
#define TEST_AVB_UTILS_H

#include <pipewire/pipewire.h>

#include "module-avb/internal.h"
#include "module-avb/packets.h"
#include "module-avb/adp.h"
#include "module-avb/acmp.h"
#include "module-avb/mrp.h"
#include "module-avb/msrp.h"
#include "module-avb/mvrp.h"
#include "module-avb/mmrp.h"
#include "module-avb/maap.h"
#include "module-avb/aecp.h"
#include "module-avb/aecp-aem-descriptors.h"
#include "module-avb/descriptors.h"
#include "module-avb/avb-transport-loopback.h"

#define server_emit_message(s,n,m,l) \
	spa_hook_list_call(&(s)->listener_list, struct server_events, message, 0, n, m, l)
#define server_emit_periodic(s,n) \
	spa_hook_list_call(&(s)->listener_list, struct server_events, periodic, 0, n)

/**
 * Create a test AVB server with loopback transport.
 * All protocol handlers are registered. No network access required.
 */
static inline struct server *avb_test_server_new(struct impl *impl)
{
	struct server *server;

	server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;

	server->impl = impl;
	server->ifname = strdup("test0");
	server->avb_mode = AVB_MODE_LEGACY;
	server->transport = &avb_transport_loopback;

	spa_list_append(&impl->servers, &server->link);
	spa_hook_list_init(&server->listener_list);
	spa_list_init(&server->descriptors);

	if (server->transport->setup(server) < 0)
		goto error;

	server->mrp = avb_mrp_new(server);
	if (server->mrp == NULL)
		goto error;

	avb_aecp_register(server);
	server->maap = avb_maap_register(server);
	server->mmrp = avb_mmrp_register(server);
	server->msrp = avb_msrp_register(server);
	server->mvrp = avb_mvrp_register(server);
	avb_adp_register(server);
	avb_acmp_register(server);

	server->domain_attr = avb_msrp_attribute_new(server->msrp,
			AVB_MSRP_ATTRIBUTE_TYPE_DOMAIN);
	server->domain_attr->attr.domain.sr_class_id = AVB_MSRP_CLASS_ID_DEFAULT;
	server->domain_attr->attr.domain.sr_class_priority = AVB_MSRP_PRIORITY_DEFAULT;
	server->domain_attr->attr.domain.sr_class_vid = htons(AVB_DEFAULT_VLAN);

	avb_mrp_attribute_begin(server->domain_attr->mrp, 0);
	avb_mrp_attribute_join(server->domain_attr->mrp, 0, true);

	/* Add a minimal entity descriptor so ADP can advertise.
	 * We skip init_descriptors() because it creates streams that
	 * need a pw_core connection. */
	{
		struct avb_aem_desc_entity entity;
		memset(&entity, 0, sizeof(entity));
		entity.entity_id = htobe64(server->entity_id);
		entity.entity_model_id = htobe64(0x0001000000000001ULL);
		entity.entity_capabilities = htonl(
				AVB_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED |
				AVB_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED |
				AVB_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED);
		entity.talker_stream_sources = htons(1);
		entity.talker_capabilities = htons(
				AVB_ADP_TALKER_CAPABILITY_IMPLEMENTED |
				AVB_ADP_TALKER_CAPABILITY_AUDIO_SOURCE);
		entity.listener_stream_sinks = htons(1);
		entity.listener_capabilities = htons(
				AVB_ADP_LISTENER_CAPABILITY_IMPLEMENTED |
				AVB_ADP_LISTENER_CAPABILITY_AUDIO_SINK);
		entity.configurations_count = htons(1);
		server_add_descriptor(server, AVB_AEM_DESC_ENTITY, 0,
				sizeof(entity), &entity);
	}

	return server;

error:
	free(server->ifname);
	free(server);
	return NULL;
}

static inline void avb_test_server_free(struct server *server)
{
	avdecc_server_free(server);
}

/**
 * Inject a raw packet into the server's protocol dispatch.
 * This simulates receiving a packet from the network.
 */
static inline void avb_test_inject_packet(struct server *server,
		uint64_t now, const void *data, int len)
{
	server_emit_message(server, now, data, len);
}

/**
 * Trigger the periodic callback with a given timestamp.
 * Use this to advance time and test timeout/readvertise logic.
 */
static inline void avb_test_tick(struct server *server, uint64_t now)
{
	server_emit_periodic(server, now);
}

/**
 * Build an ADP entity available packet.
 * Returns the packet size, or -1 on error.
 */
static inline int avb_test_build_adp_entity_available(
		uint8_t *buf, size_t bufsize,
		const uint8_t src_mac[6],
		uint64_t entity_id,
		int valid_time)
{
	struct avb_ethernet_header *h;
	struct avb_packet_adp *p;
	size_t len = sizeof(*h) + sizeof(*p);
	static const uint8_t bmac[6] = AVB_BROADCAST_MAC;

	if (bufsize < len)
		return -1;

	memset(buf, 0, len);

	h = (struct avb_ethernet_header *)buf;
	memcpy(h->dest, bmac, 6);
	memcpy(h->src, src_mac, 6);
	h->type = htons(AVB_TSN_ETH);

	p = (struct avb_packet_adp *)(buf + sizeof(*h));
	AVB_PACKET_SET_SUBTYPE(&p->hdr, AVB_SUBTYPE_ADP);
	AVB_PACKET_SET_LENGTH(&p->hdr, AVB_ADP_CONTROL_DATA_LENGTH);
	AVB_PACKET_ADP_SET_MESSAGE_TYPE(p, AVB_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
	AVB_PACKET_ADP_SET_VALID_TIME(p, valid_time);
	p->entity_id = htobe64(entity_id);

	return len;
}

/**
 * Build an ADP entity departing packet.
 */
static inline int avb_test_build_adp_entity_departing(
		uint8_t *buf, size_t bufsize,
		const uint8_t src_mac[6],
		uint64_t entity_id)
{
	struct avb_ethernet_header *h;
	struct avb_packet_adp *p;
	size_t len = sizeof(*h) + sizeof(*p);
	static const uint8_t bmac[6] = AVB_BROADCAST_MAC;

	if (bufsize < len)
		return -1;

	memset(buf, 0, len);

	h = (struct avb_ethernet_header *)buf;
	memcpy(h->dest, bmac, 6);
	memcpy(h->src, src_mac, 6);
	h->type = htons(AVB_TSN_ETH);

	p = (struct avb_packet_adp *)(buf + sizeof(*h));
	AVB_PACKET_SET_SUBTYPE(&p->hdr, AVB_SUBTYPE_ADP);
	AVB_PACKET_SET_LENGTH(&p->hdr, AVB_ADP_CONTROL_DATA_LENGTH);
	AVB_PACKET_ADP_SET_MESSAGE_TYPE(p, AVB_ADP_MESSAGE_TYPE_ENTITY_DEPARTING);
	p->entity_id = htobe64(entity_id);

	return len;
}

/**
 * Build an ADP entity discover packet.
 */
static inline int avb_test_build_adp_entity_discover(
		uint8_t *buf, size_t bufsize,
		const uint8_t src_mac[6],
		uint64_t entity_id)
{
	struct avb_ethernet_header *h;
	struct avb_packet_adp *p;
	size_t len = sizeof(*h) + sizeof(*p);
	static const uint8_t bmac[6] = AVB_BROADCAST_MAC;

	if (bufsize < len)
		return -1;

	memset(buf, 0, len);

	h = (struct avb_ethernet_header *)buf;
	memcpy(h->dest, bmac, 6);
	memcpy(h->src, src_mac, 6);
	h->type = htons(AVB_TSN_ETH);

	p = (struct avb_packet_adp *)(buf + sizeof(*h));
	AVB_PACKET_SET_SUBTYPE(&p->hdr, AVB_SUBTYPE_ADP);
	AVB_PACKET_SET_LENGTH(&p->hdr, AVB_ADP_CONTROL_DATA_LENGTH);
	AVB_PACKET_ADP_SET_MESSAGE_TYPE(p, AVB_ADP_MESSAGE_TYPE_ENTITY_DISCOVER);
	p->entity_id = htobe64(entity_id);

	return len;
}

#endif /* TEST_AVB_UTILS_H */
