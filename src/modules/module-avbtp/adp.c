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
#include "internal.h"

struct entity {
	struct spa_list link;
	struct avbtp_packet_adp packet;
	uint64_t last_time;
};

struct adp {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_list entities;
};

struct entity *find_entity_by_id(struct adp *adp, uint64_t id)
{
	struct entity *e;
	spa_list_for_each(e, &adp->entities, link)
		if (AVBTP_PACKET_ADP_GET_ENTITY_ID(&e->packet) == id)
			return e;
	return NULL;
}

struct bit_info {
	uint32_t bits;
	const char *value;
};

static const struct bit_info entity_capabilities_info[] = {
	{ AVBTP_ADP_ENTITY_CAPABILITY_EFU_MODE, "EFU Mode" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_ADDRESS_ACCESS_SUPPORTED, "Address Access Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_GATEWAY_ENTITY, "Gateway Entity" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED, "AEM Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_LEGACY_AVC, "Legacy AVC" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_ASSOCIATION_ID_SUPPORTED, "Association Id Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_ASSOCIATION_ID_VALID, "Association Id Valid" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_VENDOR_UNIQUE_SUPPORTED, "Vendor Unique Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED, "Class A Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_CLASS_B_SUPPORTED, "Class B Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED, "gPTP Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_AEM_AUTHENTICATION_SUPPORTED, "AEM Authentication Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_AEM_AUTHENTICATION_REQUIRED, "AEM Authentication Required" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_AEM_PERSISTENT_ACQUIRE_SUPPORTED, "AEM Persisitent Acquire Supported" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_AEM_IDENTIFY_CONTROL_INDEX_VALID, "AEM Identify Control Index Valid" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID, "AEM Interface Index Valid" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_GENERAL_CONTROLLER_IGNORE, "General Controller Ignore" },
	{ AVBTP_ADP_ENTITY_CAPABILITY_ENTITY_NOT_READY, "Entity Not Ready" },
	{ 0, NULL },
};
static const struct bit_info talker_capabilities_info[] = {
	{ AVBTP_ADP_TALKER_CAPABILITY_IMPLEMENTED, "Implemented" },
	{ AVBTP_ADP_TALKER_CAPABILITY_OTHER_SOURCE, "Other Source" },
	{ AVBTP_ADP_TALKER_CAPABILITY_CONTROL_SOURCE, "Control Source" },
	{ AVBTP_ADP_TALKER_CAPABILITY_MEDIA_CLOCK_SOURCE, "Media Clock Source" },
	{ AVBTP_ADP_TALKER_CAPABILITY_SMPTE_SOURCE, "SMPTE Source" },
	{ AVBTP_ADP_TALKER_CAPABILITY_MIDI_SOURCE, "MIDI Source" },
	{ AVBTP_ADP_TALKER_CAPABILITY_AUDIO_SOURCE, "Audio Source" },
	{ AVBTP_ADP_TALKER_CAPABILITY_VIDEO_SOURCE, "Video Source" },
	{ 0, NULL },
};

static const struct bit_info listener_capabilities_info[] = {
	{ AVBTP_ADP_LISTENER_CAPABILITY_IMPLEMENTED, "Implemented" },
	{ AVBTP_ADP_LISTENER_CAPABILITY_OTHER_SINK, "Other Sink" },
	{ AVBTP_ADP_LISTENER_CAPABILITY_CONTROL_SINK, "Control Sink" },
	{ AVBTP_ADP_LISTENER_CAPABILITY_MEDIA_CLOCK_SINK, "Media Clock Sink" },
	{ AVBTP_ADP_LISTENER_CAPABILITY_SMPTE_SINK, "SMPTE Sink" },
	{ AVBTP_ADP_LISTENER_CAPABILITY_MIDI_SINK, "MIDI Sink" },
	{ AVBTP_ADP_LISTENER_CAPABILITY_AUDIO_SINK, "Audio Sink" },
	{ AVBTP_ADP_LISTENER_CAPABILITY_VIDEO_SINK, "Video Sink" },
	{ 0, NULL },
};

static const struct bit_info controller_capabilities_info[] = {
	{ AVBTP_ADP_CONTROLLER_CAPABILITY_IMPLEMENTED, "Implemented" },
	{ AVBTP_ADP_CONTROLLER_CAPABILITY_LAYER3_PROXY, "Layer 3 Proxy" },
	{ 0, NULL },
};
static void print_bit_info(int indent, uint32_t bits, const struct bit_info *info)
{
	uint32_t i;
	for (i = 0; info[i].value; i++) {
		if ((info[i].bits & bits) == info[i].bits)
			pw_log_info("%*.s%08x %s", indent, "", info[i].bits, info[i].value);
	}
}

static const char *message_type_as_string(uint8_t message_type)
{
	switch (message_type) {
	case AVBTP_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE:
		return "ENTITY_AVAIALABLE";
	case AVBTP_ADP_MESSAGE_TYPE_ENTITY_DEPARTING:
		return "ENTITY_DEPARTING";
	case AVBTP_ADP_MESSAGE_TYPE_ENTITY_DISCOVER:
		return "ENTITY_DISCOVER";
	}
	return "INVALID";
}

#define KEY_VALID_TIME			"valid-time"
#define KEY_ENTITY_ID			"entity-id"
#define KEY_ENTITY_MODEL_ID		"entity-model-id"
#define KEY_ENTITY_CAPABILITIES		"entity-capabilities"
#define KEY_TALKER_STREAM_SOURCES	"talker-stream-sources"
#define KEY_TALKER_CAPABILITIES		"talker-capabilities"
#define KEY_LISTENER_STREAM_SINKS	"listener-stream-sinks"
#define KEY_LISTENER_CAPABILITIES	"listener-capabilities"
#define KEY_CONTROLLER_CAPABILITIES	"controller-capabilities"
#define KEY_AVAILABLE_INDEX		"available-index"
#define KEY_GPTP_GRANDMASTER_ID		"gptp-grandmaster-id"
#define KEY_ASSOCIATION_ID		"association-id"

static inline char *format_id(char *str, size_t size, const uint64_t id)
{
	snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x:%04x",
			(uint8_t)(id >> 56),
			(uint8_t)(id >> 48),
			(uint8_t)(id >> 40),
			(uint8_t)(id >> 32),
			(uint8_t)(id >> 24),
			(uint8_t)(id >> 16),
			(uint16_t)(id));
	return str;
}
static void adp_message_debug(struct adp *adp, const struct avbtp_packet_adp *p)
{
	uint32_t v;
	char buf[256];

	v = AVBTP_PACKET_ADP_GET_MESSAGE_TYPE(p);
	pw_log_info("message-type: %d (%s)", v, message_type_as_string(v));
	pw_log_info("  length: %d", AVBTP_PACKET_ADP_GET_LENGTH(p));
	pw_log_info("  "KEY_VALID_TIME": %d", AVBTP_PACKET_ADP_GET_VALID_TIME(p));
	pw_log_info("  "KEY_ENTITY_ID": %s", format_id(buf, sizeof(buf), AVBTP_PACKET_ADP_GET_ENTITY_ID(p)));
	pw_log_info("  "KEY_ENTITY_MODEL_ID": 0x%"PRIx64, AVBTP_PACKET_ADP_GET_ENTITY_MODEL_ID(p));
	v = AVBTP_PACKET_ADP_GET_ENTITY_CAPABILITIES(p);
	pw_log_info("  "KEY_ENTITY_CAPABILITIES": 0x%08x", v);
	print_bit_info(4, v, entity_capabilities_info);
	pw_log_info("  "KEY_TALKER_STREAM_SOURCES": %d", AVBTP_PACKET_ADP_GET_TALKER_STREAM_SOURCES(p));
	v = AVBTP_PACKET_ADP_GET_TALKER_CAPABILITIES(p);
	pw_log_info("  "KEY_TALKER_CAPABILITIES": %04x", v);
	print_bit_info(4, v, talker_capabilities_info);
	pw_log_info("  "KEY_LISTENER_STREAM_SINKS": %d", AVBTP_PACKET_ADP_GET_LISTENER_STREAM_SINKS(p));
	v = AVBTP_PACKET_ADP_GET_LISTENER_CAPABILITIES(p);
	pw_log_info("  "KEY_LISTENER_CAPABILITIES": %04x", v);
	print_bit_info(4, v, listener_capabilities_info);
	v = AVBTP_PACKET_ADP_GET_CONTROLLER_CAPABILITIES(p);
	pw_log_info("  "KEY_CONTROLLER_CAPABILITIES": %08x", v);
	print_bit_info(4, v, controller_capabilities_info);
	pw_log_info("  "KEY_AVAILABLE_INDEX": 0x%08x", AVBTP_PACKET_ADP_GET_AVAILABLE_INDEX(p));
	pw_log_info("  "KEY_GPTP_GRANDMASTER_ID": 0x%"PRIx64, AVBTP_PACKET_ADP_GET_GPTP_GRANDMASTER_ID(p));
	pw_log_info("  "KEY_ASSOCIATION_ID": 0x%08x", AVBTP_PACKET_ADP_GET_ASSOCIATION_ID(p));
}

static int adp_message(void *data, uint64_t now, const void *message, int len)
{
	struct adp *adp = data;
	const struct avbtp_packet_adp *p = message;
	struct entity *e;

	if (AVBTP_PACKET_GET_SUBTYPE(p) != AVBTP_SUBTYPE_ADP)
		return 0;


	e = find_entity_by_id(adp, AVBTP_PACKET_ADP_GET_ENTITY_ID(p));
	if (e == NULL) {
		e = calloc(1, sizeof(*e));
		if (e == NULL)
			return -errno;

		e->packet = *p;
		spa_list_append(&adp->entities, &e->link);

		if (adp->server->debug_messages)
			adp_message_debug(adp, p);
	}
	e->last_time = now;

	return 0;
}

static void adp_destroy(void *data)
{
	struct adp *adp = data;
	spa_hook_remove(&adp->server_listener);
	free(adp);
}

static void adp_periodic(void *data, uint64_t now)
{
	struct adp *adp = data;
}

static int parse_id(const char *value, int len, uint64_t *id)
{
	char str[64];
	uint8_t v[6];
	uint16_t unique_id;
	if (spa_json_parse_stringn(value, len, str, sizeof(str)) <= 0)
		return 0;
	if (sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hx",
			&v[0], &v[1], &v[2], &v[3],
			&v[4], &v[5], &unique_id) != 7)
		return -EINVAL;
	*id = (uint64_t) v[0] << 56 |
		    (uint64_t) v[1] << 48 |
		    (uint64_t) v[2] << 40 |
		    (uint64_t) v[3] << 32 |
		    (uint64_t) v[4] << 24 |
		    (uint64_t) v[5] << 16 |
		    unique_id;
	return 0;
}
static int parse_bits(const char *value, int len, int *bits)
{
	*bits = 0;
	return 0;
}

static int do_advertise(struct adp *adp, const char *args)
{
	struct entity *e;
	struct spa_json it[2];
	char key[128];
	struct avbtp_packet_adp *p;

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return -errno;

	spa_json_init(&it[0], args, strlen(args));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return -EINVAL;

	p = &e->packet;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		int len, int_val;
		const char *value;
		uint64_t id_val;

		if ((len = spa_json_next(&it[1], &value)) <= 0)
			break;

		if (spa_json_is_null(value, len))
			continue;

		if (spa_streq(key, KEY_VALID_TIME)) {
			if (spa_json_parse_int(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_VALID_TIME(p, int_val);
		} else if (spa_streq(key, KEY_ENTITY_ID)) {
			if (parse_id(value, len, &id_val))
				AVBTP_PACKET_ADP_SET_ENTITY_ID(p, id_val);
		} else if (spa_streq(key, KEY_ENTITY_MODEL_ID)) {
			if (parse_id(value, len, &id_val))
				AVBTP_PACKET_ADP_SET_ENTITY_MODEL_ID(p, id_val);
		} else if (spa_streq(key, KEY_ENTITY_CAPABILITIES)) {
			if (parse_bits(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_ENTITY_CAPABILITIES(p, int_val);
		} else if (spa_streq(key, KEY_TALKER_STREAM_SOURCES)) {
			if (spa_json_parse_int(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_TALKER_STREAM_SOURCES(p, int_val);
		} else if (spa_streq(key, KEY_TALKER_CAPABILITIES)) {
			if (parse_bits(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_TALKER_CAPABILITIES(p, int_val);
		} else if (spa_streq(key, KEY_LISTENER_STREAM_SINKS)) {
			if (spa_json_parse_int(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_LISTENER_STREAM_SINKS(p, int_val);
		} else if (spa_streq(key, KEY_LISTENER_CAPABILITIES)) {
			if (parse_bits(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_LISTENER_CAPABILITIES(p, int_val);
		} else if (spa_streq(key, KEY_CONTROLLER_CAPABILITIES)) {
			if (parse_bits(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_CONTROLLER_CAPABILITIES(p, int_val);
		} else if (spa_streq(key, KEY_AVAILABLE_INDEX)) {
			if (spa_json_parse_int(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_AVAILABLE_INDEX(p, int_val);
		} else if (spa_streq(key, KEY_GPTP_GRANDMASTER_ID)) {
			if (parse_id(value, len, &id_val))
				AVBTP_PACKET_ADP_SET_GPTP_GRANDMASTER_ID(p, id_val);
		} else if (spa_streq(key, KEY_ASSOCIATION_ID)) {
			if (spa_json_parse_int(value, len, &int_val))
				AVBTP_PACKET_ADP_SET_ASSOCIATION_ID(p, int_val);
		}
	}
	if (find_entity_by_id(adp, AVBTP_PACKET_ADP_GET_ENTITY_ID(p))) {
		free(e);
		return -EEXIST;
	}
	spa_list_append(&adp->entities, &e->link);

	if (adp->server->debug_messages)
		adp_message_debug(adp, p);

	return 0;
}

static int do_depart(struct adp *adp, const char *args)
{
	return 0;
}

static int do_discover(struct adp *adp, const char *args)
{
	return 0;
}

static int adp_command(void *data, const char *command, const char *args)
{
	struct adp *adp = data;
	int res;

	if (spa_streq(command, "/adp/advertise"))
		res = do_advertise(adp, args);
	else if (spa_streq(command, "/adp/depart"))
		res = do_depart(adp, args);
	else if (spa_streq(command, "/adp/discover"))
		res = do_discover(adp, args);
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
