/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/json.h>

#include <pipewire/pipewire.h>

#include "adp.h"
#include "acmp.h"
#include "aecp-aem-descriptors.h"
#include "gptp.h"
#include "internal.h"
#include "utils.h"

static const uint8_t mac[6] = AVB_BROADCAST_MAC;

struct entity {
	struct spa_list link;
	uint64_t entity_id;
	uint64_t last_time;
	int valid_time;
	unsigned advertise:1;
	size_t len;
	uint8_t buf[128];
};

struct adp {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_list entities;
	uint32_t available_index;
};

static struct entity *find_entity_by_id(struct adp *adp, uint64_t id)
{
	struct entity *e;
	spa_list_for_each(e, &adp->entities, link)
		if (e->entity_id == id)
			return e;
	return NULL;
}
static void entity_free(struct entity *e)
{
	spa_list_remove(&e->link);
	free(e);
}

static void refresh_gptp_fields(struct server *server, struct avb_packet_adp *p)
{
	const struct descriptor *d;
	struct avb_aem_desc_avb_interface *avb_interface;
	uint64_t gm_id_be;

	d = server_find_descriptor(server, AVB_AEM_DESC_AVB_INTERFACE, 0);
	avb_interface = d ? descriptor_body(d) : NULL;
	if (avb_interface == NULL) {
		return;
	}

	if (avb_gptp_get_grandmaster_id(server->gptp, &gm_id_be)) {
		p->gptp_grandmaster_id = gm_id_be;
	} else {
		p->gptp_grandmaster_id = avb_interface->clock_identity;
	}
	p->gptp_domain_number = avb_interface->domain_number;
}

static int send_departing(struct adp *adp, uint64_t now, struct entity *e)
{
	struct avb_ethernet_header *h = (void*)e->buf;
	struct avb_packet_adp *p = SPA_PTROFF(h, sizeof(*h), void);

	refresh_gptp_fields(adp->server, p);
	AVB_PACKET_ADP_SET_MESSAGE_TYPE(p, AVB_ADP_MESSAGE_TYPE_ENTITY_DEPARTING);
	p->available_index = htonl(adp->available_index++);
	avb_server_send_packet(adp->server, mac, AVB_TSN_ETH, e->buf, e->len);
	e->last_time = now;
	return 0;
}

static int send_advertise(struct adp *adp, uint64_t now, struct entity *e)
{
	struct avb_ethernet_header *h = (void*)e->buf;
	struct avb_packet_adp *p = SPA_PTROFF(h, sizeof(*h), void);

	refresh_gptp_fields(adp->server, p);
	AVB_PACKET_ADP_SET_MESSAGE_TYPE(p, AVB_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
	p->available_index = htonl(adp->available_index++);
	avb_server_send_packet(adp->server, mac, AVB_TSN_ETH, e->buf, e->len);
	e->last_time = now;
	return 0;
}

static int send_discover(struct adp *adp, uint64_t entity_id)
{
	uint8_t buf[128];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_adp *p = SPA_PTROFF(h, sizeof(*h), void);
	size_t len = sizeof(*h) + sizeof(*p);

	spa_memzero(buf, sizeof(buf));
	AVB_PACKET_SET_SUBTYPE(&p->hdr, AVB_SUBTYPE_ADP);
	AVB_PACKET_SET_LENGTH(&p->hdr, AVB_ADP_CONTROL_DATA_LENGTH);
	AVB_PACKET_ADP_SET_MESSAGE_TYPE(p, AVB_ADP_MESSAGE_TYPE_ENTITY_DISCOVER);
	p->entity_id = htobe64(entity_id);
	avb_server_send_packet(adp->server, mac, AVB_TSN_ETH, buf, len);
	return 0;
}

static int adp_message(void *data, uint64_t now, const void *message, int len)
{
	struct adp *adp = data;
	struct server *server = adp->server;
	const struct avb_ethernet_header *h = message;
	const struct avb_packet_adp *p = SPA_PTROFF(h, sizeof(*h), void);
	struct entity *e;
	int message_type;
	char buf[128];
	uint64_t entity_id;

	if (len < 0 || (size_t)len < sizeof(*h) + sizeof(*p))
		return 0;

	if (ntohs(h->type) != AVB_TSN_ETH)
		return 0;
	if (memcmp(h->dest, mac, 6) != 0 &&
	    memcmp(h->dest, server->mac_addr, 6) != 0)
		return 0;

	if (AVB_PACKET_GET_SUBTYPE(&p->hdr) != AVB_SUBTYPE_ADP ||
	    AVB_PACKET_GET_LENGTH(&p->hdr) < AVB_ADP_CONTROL_DATA_LENGTH)
		return 0;

	message_type = AVB_PACKET_ADP_GET_MESSAGE_TYPE(p);
	entity_id = be64toh(p->entity_id);

	e = find_entity_by_id(adp, entity_id);

	{
		const char *mt = (message_type == AVB_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE) ? "ADP rx ENTITY_AVAILABLE" :
				 (message_type == AVB_ADP_MESSAGE_TYPE_ENTITY_DEPARTING) ? "ADP rx ENTITY_DEPARTING" :
				 (message_type == AVB_ADP_MESSAGE_TYPE_ENTITY_DISCOVER)  ? "ADP rx ENTITY_DISCOVER"  :
				                                                           "ADP rx ?";
		avb_log_state(server, mt);
	}

	switch (message_type) {
	case AVB_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE:
		if (e == NULL) {
			e = calloc(1, sizeof(*e));
			if (e == NULL)
				return -errno;

			memcpy(e->buf, message, len);
			e->len = len;
			e->valid_time = AVB_PACKET_ADP_GET_VALID_TIME(p);
			e->entity_id = entity_id;
			spa_list_append(&adp->entities, &e->link);
			pw_log_info("entity %s available",
				avb_utils_format_id(buf, sizeof(buf), entity_id));

			if (server->avb_mode == AVB_MODE_MILAN_V12) {
				//Milan V1.2 Section 5.6.4.5.1
				if (handle_evt_tk_discovered(server->acmp, entity_id, now)) {
					pw_log_info("handling available event");
					return -1;
				}
			}
		} else {
			if (server->avb_mode == AVB_MODE_MILAN_V12) {
				//Milan V1.2
				//Milan V1.2 Section 5.6.4.5.2
				struct avb_ethernet_header *h_saved = (struct avb_ethernet_header *) e->buf;
				struct avb_packet_adp *p_saved =
				       	SPA_PTROFF(h_saved, sizeof(*h_saved), void);

				if (ntohl(p->available_index) <= ntohl(p_saved->available_index)) {
					if (handle_evt_tk_departed(server->acmp, entity_id, now)) {
						pw_log_info("handling departing event");
						return -1;
					}

					bool has_gptp_domain_changed =
						(p_saved->gptp_domain_number != p->gptp_domain_number) ||
						(p_saved->gptp_grandmaster_id != p->gptp_grandmaster_id);

					if (has_gptp_domain_changed) {
						e->last_time = INT64_MAX;
						spa_list_remove(&e->link);
						pw_log_info("Removing from the adp list \n");
						return 0;
					}

					if (handle_evt_tk_discovered(server->acmp, entity_id, now)) {
						pw_log_warn("handling available event");
						return -1;
					}
				}

				memcpy(e->buf, message, len);
			}
		}
		e->last_time = now;

		break;
	case AVB_ADP_MESSAGE_TYPE_ENTITY_DEPARTING:
		if (e != NULL) {
			if (server->avb_mode == AVB_MODE_MILAN_V12) {
				// Milan v1.2 Section 5.6.4.5.3
				handle_evt_tk_departed(server->acmp, entity_id, now);
			}

			pw_log_info("entity %s departing",
				avb_utils_format_id(buf, sizeof(buf), entity_id));
			entity_free(e);
		}

		break;
	case AVB_ADP_MESSAGE_TYPE_ENTITY_DISCOVER:
		pw_log_info("entity %s advertise",
				avb_utils_format_id(buf, sizeof(buf), entity_id));
		if (entity_id == 0UL) {
			spa_list_for_each(e, &adp->entities, link)
				if (e->advertise)
					send_advertise(adp, now, e);
		} else if (e != NULL &&
		    e->advertise && e->entity_id == entity_id) {
			send_advertise(adp, now, e);
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void adp_destroy(void *data)
{
	struct adp *adp = data;
	struct entity *e, *t;

	spa_hook_remove(&adp->server_listener);

        spa_list_for_each_safe(e, t, &adp->entities, link) {
		entity_free(e);
        }

	free(adp);
}

static void check_timeout(struct adp *adp, uint64_t now)
{
	struct entity *e, *t;
	char buf[128];
	struct avb_acmp *avb_acmp = adp->server->acmp;

	spa_list_for_each_safe(e, t, &adp->entities, link) {
		if (e->last_time + (e->valid_time + 2) * SPA_NSEC_PER_SEC > now)
			continue;

		pw_log_info("entity %s timeout",
			avb_utils_format_id(buf, sizeof(buf), e->entity_id));

		handle_evt_tk_departed(avb_acmp, e->entity_id, now);

		if (e->advertise)
			send_departing(adp, now, e);

		entity_free(e);
	}
}
static void check_readvertize(struct adp *adp, uint64_t now, struct entity *e)
{
	char buf[128];

	if (!e->advertise)
		return;

	if (e->last_time + (e->valid_time / 2) * SPA_NSEC_PER_SEC > now)
		return;

	pw_log_debug("entity %s readvertise",
		avb_utils_format_id(buf, sizeof(buf), e->entity_id));

	send_advertise(adp, now, e);
}

static int check_advertise(struct adp *adp, uint64_t now)
{
	struct server *server = adp->server;
	const struct descriptor *d;
	struct avb_aem_desc_entity *entity;
	struct entity *e;
	uint64_t entity_id;
	struct avb_ethernet_header *h;
	struct avb_packet_adp *p;
	char buf[128];

	d = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (d == NULL)
		return -1;

	entity = descriptor_body(d);
	entity_id = be64toh(entity->entity_id);

	if ((e = find_entity_by_id(adp, entity_id)) != NULL) {
		if (e->advertise)
			check_readvertize(adp, now, e);
		return 0;
	}

	pw_log_info("entity %s advertise",
		avb_utils_format_id(buf, sizeof(buf), entity_id));

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return -errno;

	e->advertise = true;
	e->valid_time = 10;
	e->last_time = now;
	e->entity_id = entity_id;
	e->len = sizeof(*h) + sizeof(*p);

	h = (void*)e->buf;
	p = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_SET_LENGTH(&p->hdr, AVB_ADP_CONTROL_DATA_LENGTH);
	AVB_PACKET_SET_SUBTYPE(&p->hdr, AVB_SUBTYPE_ADP);
	AVB_PACKET_ADP_SET_MESSAGE_TYPE(p, AVB_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
	AVB_PACKET_ADP_SET_VALID_TIME(p, e->valid_time);

	p->entity_id = entity->entity_id;
	p->entity_model_id = entity->entity_model_id;
	p->entity_capabilities = entity->entity_capabilities;
	p->talker_stream_sources = entity->talker_stream_sources;
	p->talker_capabilities = entity->talker_capabilities;
	p->listener_stream_sinks = entity->listener_stream_sinks;
	p->listener_capabilities = entity->listener_capabilities;
	p->controller_capabilities = entity->controller_capabilities;
	p->available_index = entity->available_index;
	p->identify_control_index = 0;
	p->interface_index = 0;
	p->association_id = entity->association_id;

	spa_list_append(&adp->entities, &e->link);

	return 0;
}

static void adp_periodic(void *data, uint64_t now)
{
	struct adp *adp = data;
	check_timeout(adp, now);
	check_advertise(adp, now);
}

static int do_help(struct adp *adp, const char *args, FILE *out)
{
	fprintf(out, "{ \"type\": \"help\","
			"\"text\": \""
			  "/adp/help: this help \\n"
			  "/adp/discover [{ \"entity-id\": <id> }] : trigger discover\\n"
			"\" }");
	return 0;
}

static int do_discover(struct adp *adp, const char *args, FILE *out)
{
	struct spa_json it[1];
	char key[128];
	uint64_t entity_id = 0ULL;
	int len;
	const char *value;

	if (spa_json_begin_object(&it[0], args, strlen(args)) <= 0)
		return -EINVAL;

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &value)) > 0) {
		uint64_t id_val;

		if (spa_json_is_null(value, len))
			continue;

		if (spa_streq(key, "entity-id")) {
			if (avb_utils_parse_id(value, len, &id_val) >= 0)
				entity_id = id_val;
		}
	}
	send_discover(adp, entity_id);
	return 0;
}

static int adp_command(void *data, uint64_t now, const char *command, const char *args, FILE *out)
{
	struct adp *adp = data;
	int res;

	if (!spa_strstartswith(command, "/adp/"))
		return 0;

	command += strlen("/adp/");

	if (spa_streq(command, "help"))
		res = do_help(adp, args, out);
	else if (spa_streq(command, "discover"))
		res = do_discover(adp, args, out);
	else
		res = -ENOTSUP;

	return res;
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = adp_destroy,
	.message = adp_message,
	.periodic = adp_periodic,
	.command = adp_command
};

bool adp_is_discovered_entity(struct server *server, uint64_t entity_id)
{
	struct adp *adp = (struct adp*)server->adp;
	struct entity *entity = find_entity_by_id(adp, entity_id);

	if (entity == NULL) {
		return false;
	}

	return true;
}

int adp_start_discovery_entity(struct server *server, uint64_t entity_id)
{
	pw_log_info("ADP: start discovery of entity 0x%"PRIx64, entity_id);
	return send_discover((struct adp*) server->adp, entity_id);
}

void adp_stop_discovery_entity(struct server *server, uint64_t entity_id)
{
	struct entity *e, *t;
	struct adp *adp = (struct adp*)server->adp;
	pw_log_info("ADP: stop discovery of entity 0x%" PRIx64, entity_id);

        spa_list_for_each_safe(e, t, &adp->entities, link) {
		if (e->entity_id == entity_id) {
			entity_free(e);
			return;
		}
        }

	pw_log_warn("Could not find entity 0x%"PRIx64, entity_id);
}

void adp_log_state(struct server *server, const char *label)
{
	struct adp *adp = (struct adp *)server->adp;
	struct entity *e;
	struct timespec ts;
	uint64_t now;
	char buf[64];
	int n = 0;

	if (adp == NULL)
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = SPA_TIMESPEC_TO_NSEC(&ts);

	spa_list_for_each(e, &adp->entities, link)
		n++;
	pw_log_debug("[%s] ADP: %d entit%s", label, n, n == 1 ? "y" : "ies");

	spa_list_for_each(e, &adp->entities, link) {
		struct avb_ethernet_header *h = (void *)e->buf;
		struct avb_packet_adp *p = SPA_PTROFF(h, sizeof(*h), void);
		uint64_t age_ms = (now - e->last_time) / 1000000ULL;
		pw_log_debug("[%s]   %s last_seen=%" PRIu64 "ms valid=%ds available_index=%u",
				label,
				avb_utils_format_id(buf, sizeof(buf), e->entity_id),
				age_ms, e->valid_time, ntohl(p->available_index));
	}
}

struct avb_adp *avb_adp_register(struct server *server)
{
	struct adp *adp;

	adp = calloc(1, sizeof(*adp));
	if (adp == NULL)
		return NULL;

	adp->server = server;
	spa_list_init(&adp->entities);

	avdecc_server_add_listener(server, &adp->server_listener, &server_events, adp);

	return (struct avb_adp*)adp;
}

void avb_adp_unregister(struct avb_adp *adp)
{
	adp_destroy(adp);
}
