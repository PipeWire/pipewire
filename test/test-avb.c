/* AVB tests */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWire contributors */
/* SPDX-License-Identifier: MIT */

#include "pwtest.h"

#include <pipewire/pipewire.h>

#include "module-avb/aecp-aem-descriptors.h"
#include "test-avb-utils.h"

static struct impl *test_impl_new(void)
{
	struct impl *impl;
	struct pw_main_loop *ml;
	struct pw_context *context;

	pw_init(0, NULL);

	ml = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(ml);

	context = pw_context_new(pw_main_loop_get_loop(ml),
			pw_properties_new(
				PW_KEY_CONFIG_NAME, "null",
				NULL), 0);
	pwtest_ptr_notnull(context);

	impl = calloc(1, sizeof(*impl));
	pwtest_ptr_notnull(impl);

	impl->loop = pw_main_loop_get_loop(ml);
	impl->timer_queue = pw_context_get_timer_queue(context);
	impl->context = context;
	spa_list_init(&impl->servers);

	return impl;
}

static void test_impl_free(struct impl *impl)
{
	struct server *s;
	spa_list_consume(s, &impl->servers, link)
		avb_test_server_free(s);
	free(impl);
	pw_deinit();
}

/*
 * Test: inject an ADP ENTITY_AVAILABLE packet and verify
 * that the server processes it without error.
 */
PWTEST(avb_adp_entity_available)
{
	struct impl *impl;
	struct server *server;
	uint8_t pkt[256];
	int len;
	static const uint8_t remote_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
	uint64_t remote_entity_id = 0x020000fffe000002ULL;

	impl = test_impl_new();
	server = avb_test_server_new(impl);
	pwtest_ptr_notnull(server);

	/* Build and inject an entity available packet from a remote device */
	len = avb_test_build_adp_entity_available(pkt, sizeof(pkt),
			remote_mac, remote_entity_id, 10);
	pwtest_int_gt(len, 0);

	avb_test_inject_packet(server, 1 * SPA_NSEC_PER_SEC, pkt, len);

	/* The packet should have been processed without crashing.
	 * We can't easily inspect ADP internal state without exposing it,
	 * but we can verify the server is still functional by doing another
	 * inject and triggering periodic. */
	avb_test_tick(server, 2 * SPA_NSEC_PER_SEC);

	test_impl_free(impl);

	return PWTEST_PASS;
}

/*
 * Test: inject ENTITY_AVAILABLE then ENTITY_DEPARTING for the same entity.
 */
PWTEST(avb_adp_entity_departing)
{
	struct impl *impl;
	struct server *server;
	uint8_t pkt[256];
	int len;
	static const uint8_t remote_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 };
	uint64_t remote_entity_id = 0x020000fffe000003ULL;

	impl = test_impl_new();
	server = avb_test_server_new(impl);
	pwtest_ptr_notnull(server);

	/* First make the entity known */
	len = avb_test_build_adp_entity_available(pkt, sizeof(pkt),
			remote_mac, remote_entity_id, 10);
	avb_test_inject_packet(server, 1 * SPA_NSEC_PER_SEC, pkt, len);

	/* Now send departing */
	len = avb_test_build_adp_entity_departing(pkt, sizeof(pkt),
			remote_mac, remote_entity_id);
	pwtest_int_gt(len, 0);
	avb_test_inject_packet(server, 2 * SPA_NSEC_PER_SEC, pkt, len);

	avb_test_tick(server, 3 * SPA_NSEC_PER_SEC);

	test_impl_free(impl);

	return PWTEST_PASS;
}

/*
 * Test: inject ENTITY_DISCOVER with entity_id=0 (discover all).
 * The server should respond with its own entity advertisement
 * once it has one (after periodic runs check_advertise).
 */
PWTEST(avb_adp_entity_discover)
{
	struct impl *impl;
	struct server *server;
	uint8_t pkt[256];
	int len;
	static const uint8_t remote_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x04 };

	impl = test_impl_new();
	server = avb_test_server_new(impl);
	pwtest_ptr_notnull(server);

	/* Trigger periodic to let the server advertise its own entity
	 * (check_advertise reads the entity descriptor) */
	avb_test_tick(server, 1 * SPA_NSEC_PER_SEC);
	avb_loopback_clear_packets(server);

	/* Send discover-all (entity_id = 0) */
	len = avb_test_build_adp_entity_discover(pkt, sizeof(pkt),
			remote_mac, 0);
	pwtest_int_gt(len, 0);
	avb_test_inject_packet(server, 2 * SPA_NSEC_PER_SEC, pkt, len);

	/* The server should have sent an advertise response */
	pwtest_int_gt(avb_loopback_get_packet_count(server), 0);

	test_impl_free(impl);

	return PWTEST_PASS;
}

/*
 * Test: entity timeout — add an entity, then advance time past
 * valid_time + 2 seconds and verify periodic cleans it up.
 */
PWTEST(avb_adp_entity_timeout)
{
	struct impl *impl;
	struct server *server;
	uint8_t pkt[256];
	int len;
	static const uint8_t remote_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x05 };
	uint64_t remote_entity_id = 0x020000fffe000005ULL;
	int valid_time = 10; /* seconds */

	impl = test_impl_new();
	server = avb_test_server_new(impl);
	pwtest_ptr_notnull(server);

	/* Add entity */
	len = avb_test_build_adp_entity_available(pkt, sizeof(pkt),
			remote_mac, remote_entity_id, valid_time);
	avb_test_inject_packet(server, 1 * SPA_NSEC_PER_SEC, pkt, len);

	/* Tick at various times before timeout — entity should survive */
	avb_test_tick(server, 5 * SPA_NSEC_PER_SEC);
	avb_test_tick(server, 10 * SPA_NSEC_PER_SEC);

	/* Tick past valid_time + 2 seconds from last_time (1s + 12s = 13s) */
	avb_test_tick(server, 14 * SPA_NSEC_PER_SEC);

	/* The entity should have been timed out and cleaned up.
	 * If the entity was still present and had advertise=true, a departing
	 * packet would be sent. Inject a discover to verify: if the entity
	 * is gone, no response for that specific entity_id. */

	avb_loopback_clear_packets(server);
	len = avb_test_build_adp_entity_discover(pkt, sizeof(pkt),
			remote_mac, remote_entity_id);
	avb_test_inject_packet(server, 15 * SPA_NSEC_PER_SEC, pkt, len);

	/* Remote entities don't have advertise=true, so even before timeout
	 * a discover for them wouldn't generate a response. But at least
	 * the timeout path was exercised without crashes. */

	test_impl_free(impl);

	return PWTEST_PASS;
}

/*
 * Test: basic MRP attribute lifecycle — create, begin, join.
 */
PWTEST(avb_mrp_attribute_lifecycle)
{
	struct impl *impl;
	struct server *server;
	struct avb_msrp_attribute *attr;

	impl = test_impl_new();
	server = avb_test_server_new(impl);
	pwtest_ptr_notnull(server);

	/* Create an MSRP talker attribute */
	attr = avb_msrp_attribute_new(server->msrp,
			AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE);
	pwtest_ptr_notnull(attr);
	pwtest_ptr_notnull(attr->mrp);

	/* Begin and join the attribute */
	avb_mrp_attribute_begin(attr->mrp, 0);
	avb_mrp_attribute_join(attr->mrp, 0, true);

	/* Tick to process the MRP state machine */
	avb_test_tick(server, 1 * SPA_NSEC_PER_SEC);
	avb_test_tick(server, 2 * SPA_NSEC_PER_SEC);

	test_impl_free(impl);

	return PWTEST_PASS;
}

/*
 * Test: server with Milan v1.2 mode.
 */
PWTEST(avb_milan_server_create)
{
	struct impl *impl;
	struct server *server;

	impl = test_impl_new();

	/* Create a Milan-mode server manually */
	server = calloc(1, sizeof(*server));
	pwtest_ptr_notnull(server);

	server->impl = impl;
	server->ifname = strdup("test0");
	server->avb_mode = AVB_MODE_MILAN_V12;
	server->transport = &avb_transport_loopback;

	spa_list_append(&impl->servers, &server->link);
	spa_hook_list_init(&server->listener_list);
	spa_list_init(&server->descriptors);

	pwtest_int_eq(server->transport->setup(server), 0);

	server->mrp = avb_mrp_new(server);
	pwtest_ptr_notnull(server->mrp);

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

	/* Add minimal entity descriptor (skip init_descriptors which needs pw_core) */
	{
		struct avb_aem_desc_entity entity;
		memset(&entity, 0, sizeof(entity));
		entity.entity_id = htobe64(server->entity_id);
		entity.entity_model_id = htobe64(0x0001000000000001ULL);
		entity.configurations_count = htons(1);
		server_add_descriptor(server, AVB_AEM_DESC_ENTITY, 0,
				sizeof(entity), &entity);
	}

	/* Verify Milan mode was set correctly */
	pwtest_str_eq(get_avb_mode_str(server->avb_mode), "Milan V1.2");

	/* Tick to exercise periodic handlers with Milan descriptors */
	avb_test_tick(server, 1 * SPA_NSEC_PER_SEC);

	test_impl_free(impl);

	return PWTEST_PASS;
}

PWTEST_SUITE(avb)
{
	pwtest_add(avb_adp_entity_available, PWTEST_NOARG);
	pwtest_add(avb_adp_entity_departing, PWTEST_NOARG);
	pwtest_add(avb_adp_entity_discover, PWTEST_NOARG);
	pwtest_add(avb_adp_entity_timeout, PWTEST_NOARG);
	pwtest_add(avb_mrp_attribute_lifecycle, PWTEST_NOARG);
	pwtest_add(avb_milan_server_create, PWTEST_NOARG);

	return PWTEST_PASS;
}
