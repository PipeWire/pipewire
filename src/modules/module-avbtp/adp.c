/* AVB support
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <spa/utils/json.h>

#include <pipewire/pipewire.h>

#include "adp.h"
#include "aecp-aem-descriptors.h"
#include "internal.h"
#include "utils.h"

static const uint8_t mac[6] = AVB_BROADCAST_MAC;

struct entity {
	struct spa_list link;
	struct avbtp_packet_adp packet;
	uint64_t last_time;
	unsigned advertise:1;
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
		if (be64toh(e->packet.entity_id) == id)
			return e;
	return NULL;
}
static void entity_free(struct entity *e)
{
	spa_list_remove(&e->link);
	free(e);
}

static int send_departing(struct adp *adp, uint64_t now, struct entity *e)
{
	AVBTP_PACKET_ADP_SET_MESSAGE_TYPE(&e->packet, AVBTP_ADP_MESSAGE_TYPE_ENTITY_DEPARTING);
	e->packet.available_index = htonl(adp->available_index++);
	avbtp_server_send_packet(adp->server, mac, AVB_TSN_ETH, &e->packet, sizeof(e->packet));
	e->last_time = now;
	return 0;
}

static int send_advertise(struct adp *adp, uint64_t now, struct entity *e)
{
	AVBTP_PACKET_ADP_SET_MESSAGE_TYPE(&e->packet, AVBTP_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
	e->packet.available_index = htonl(adp->available_index++);
	avbtp_server_send_packet(adp->server, mac, AVB_TSN_ETH, &e->packet, sizeof(e->packet));
	e->last_time = now;
	return 0;
}

static int send_discover(struct adp *adp, uint64_t entity_id)
{
	struct avbtp_packet_adp p;
	spa_zero(p);
	AVBTP_PACKET_SET_SUBTYPE(&p.hdr, AVBTP_SUBTYPE_ADP);
	AVBTP_PACKET_SET_LENGTH(&p.hdr, AVBTP_ADP_CONTROL_DATA_LENGTH);
	AVBTP_PACKET_ADP_SET_MESSAGE_TYPE(&p, AVBTP_ADP_MESSAGE_TYPE_ENTITY_DISCOVER);
	p.entity_id = htonl(entity_id);
	avbtp_server_send_packet(adp->server, mac, AVB_TSN_ETH, &p, sizeof(p));
	return 0;
}

static int adp_message(void *data, uint64_t now, const void *message, int len)
{
	struct adp *adp = data;
	struct server *server = adp->server;
	const struct avbtp_packet_adp *p = message;
	struct entity *e;
	int message_type;
	char buf[128];
	uint64_t entity_id;

	if (ntohs(p->hdr.eth.type) != AVB_TSN_ETH)
		return 0;
	if (memcmp(p->hdr.eth.dest, mac, 6) != 0 &&
	    memcmp(p->hdr.eth.dest, server->mac_addr, 6) != 0)
		return 0;

	if (AVBTP_PACKET_GET_SUBTYPE(&p->hdr) != AVBTP_SUBTYPE_ADP ||
	    AVBTP_PACKET_GET_LENGTH(&p->hdr) < AVBTP_ADP_CONTROL_DATA_LENGTH)
		return 0;

	message_type = AVBTP_PACKET_ADP_GET_MESSAGE_TYPE(p);
	entity_id = be64toh(p->entity_id);

	e = find_entity_by_id(adp, entity_id);

	switch (message_type) {
	case AVBTP_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE:
		if (e == NULL) {
			e = calloc(1, sizeof(*e));
			if (e == NULL)
				return -errno;

			e->packet = *p;
			spa_list_append(&adp->entities, &e->link);
			pw_log_info("entity %s available",
				avbtp_utils_format_id(buf, sizeof(buf), entity_id));
		}
		e->last_time = now;
		break;
	case AVBTP_ADP_MESSAGE_TYPE_ENTITY_DEPARTING:
		if (e != NULL) {
			pw_log_info("entity %s departing",
				avbtp_utils_format_id(buf, sizeof(buf), entity_id));
			entity_free(e);
		}
		break;
	case AVBTP_ADP_MESSAGE_TYPE_ENTITY_DISCOVER:
		if (entity_id == 0UL ||
		    (e != NULL && e->advertise &&
		     be64toh(e->packet.entity_id) == entity_id)) {
			pw_log_info("entity %s discover",
					avbtp_utils_format_id(buf, sizeof(buf), entity_id));
			send_discover(adp, entity_id);
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
	spa_hook_remove(&adp->server_listener);
	free(adp);
}

static void check_timeout(struct adp *adp, uint64_t now)
{
	struct entity *e, *t;
	char buf[128];

	spa_list_for_each_safe(e, t, &adp->entities, link) {
		int valid_time = AVBTP_PACKET_ADP_GET_VALID_TIME(&e->packet);

		if (e->last_time + (valid_time + 2) * SPA_NSEC_PER_SEC > now)
			continue;

		pw_log_info("entity %s timeout",
			avbtp_utils_format_id(buf, sizeof(buf),
				be64toh(e->packet.entity_id)));

		if (e->advertise)
			send_departing(adp, now, e);

		entity_free(e);
	}
}
static void check_readvertize(struct adp *adp, uint64_t now, struct entity *e)
{
	int valid_time = AVBTP_PACKET_ADP_GET_VALID_TIME(&e->packet);
	char buf[128];

	if (!e->advertise)
		return;

	if (e->last_time + (valid_time / 2) * SPA_NSEC_PER_SEC > now)
		return;

	pw_log_debug("entity %s readvertise",
		avbtp_utils_format_id(buf, sizeof(buf),
			be64toh(e->packet.entity_id)));

	send_advertise(adp, now, e);
}

static int check_advertise(struct adp *adp, uint64_t now)
{
	struct server *server = adp->server;
	const struct descriptor *d;
	struct avbtp_aem_desc_entity *entity;
	struct avbtp_aem_desc_avb_interface *avb_interface;
	struct entity *e;
	uint64_t entity_id;
	struct avbtp_packet_adp *p;
	char buf[128];

	d = server_find_descriptor(server, AVBTP_AEM_DESC_ENTITY, 0);
	if (d == NULL)
		return 0;

	entity = d->ptr;
	entity_id = be64toh(entity->entity_id);

	if ((e = find_entity_by_id(adp, entity_id)) != NULL) {
		if (e->advertise)
			check_readvertize(adp, now, e);
		return 0;
	}

	d = server_find_descriptor(server, AVBTP_AEM_DESC_AVB_INTERFACE, 0);
	avb_interface = d ? d->ptr : NULL;

	pw_log_info("entity %s advertise",
		avbtp_utils_format_id(buf, sizeof(buf), entity_id));

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return -errno;

	e->advertise = true;
	e->last_time = now;

	p = &e->packet;
	AVBTP_PACKET_SET_LENGTH(&p->hdr, AVBTP_ADP_CONTROL_DATA_LENGTH);
	AVBTP_PACKET_SET_SUBTYPE(&p->hdr, AVBTP_SUBTYPE_ADP);
	AVBTP_PACKET_ADP_SET_MESSAGE_TYPE(p, AVBTP_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
	AVBTP_PACKET_ADP_SET_VALID_TIME(p, 10);

	p->entity_id = entity->entity_id;
	p->entity_model_id = entity->entity_model_id;
	p->entity_capabilities = entity->entity_capabilities;
	p->talker_stream_sources = entity->talker_stream_sources;
	p->talker_capabilities = entity->talker_capabilities;
	p->listener_stream_sinks = entity->listener_stream_sinks;
	p->listener_capabilities = entity->listener_capabilities;
	p->controller_capabilities = entity->controller_capabilities;
	p->available_index = entity->available_index;
	if (avb_interface) {
		p->gptp_grandmaster_id = avb_interface->clock_identity;
		p->gptp_domain_number = avb_interface->domain_number;
	}
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
	struct spa_json it[2];
	char key[128];
	uint64_t entity_id = 0ULL;

	spa_json_init(&it[0], args, strlen(args));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		int len;
		const char *value;
		uint64_t id_val;

		if ((len = spa_json_next(&it[1], &value)) <= 0)
			break;

		if (spa_json_is_null(value, len))
			continue;

		if (spa_streq(key, "entity-id")) {
			if (avbtp_utils_parse_id(value, len, &id_val) >= 0)
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
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = adp_destroy,
	.message = adp_message,
	.periodic = adp_periodic,
	.command = adp_command
};

struct avbtp_adp *avbtp_adp_register(struct server *server)
{
	struct adp *adp;

	adp = calloc(1, sizeof(*adp));
	if (adp == NULL)
		return NULL;

	adp->server = server;
	spa_list_init(&adp->entities);

	avdecc_server_add_listener(server, &adp->server_listener, &server_events, adp);

	return (struct avbtp_adp*)adp;
}

void avbtp_adp_unregister(struct avbtp_adp *adp)
{
	adp_destroy(adp);
}
