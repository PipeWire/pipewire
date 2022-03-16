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

#include <pipewire/pipewire.h>

#include "maap.h"

struct maap {
	struct server *server;
	struct spa_hook server_listener;
};

static const char *message_type_as_string(uint8_t message_type)
{
	switch (message_type) {
	case AVBTP_MAAP_MESSAGE_TYPE_PROBE:
		return "PROBE";
	case AVBTP_MAAP_MESSAGE_TYPE_DEFEND:
		return "DEFEND";
	case AVBTP_MAAP_MESSAGE_TYPE_ANNOUNCE:
		return "ANNOUNCE";
	}
	return "INVALID";
}

static void maap_message_debug(struct maap *maap, const struct avbtp_packet_maap *p)
{
	uint32_t v;
	const uint8_t *addr;

	v = AVBTP_PACKET_MAAP_GET_MESSAGE_TYPE(p);
	pw_log_info("message-type: %d (%s)", v, message_type_as_string(v));
	pw_log_info("  maap-version: %d", AVBTP_PACKET_MAAP_GET_MAAP_VERSION(p));
	pw_log_info("  length: %d", AVBTP_PACKET_MAAP_GET_LENGTH(p));

	pw_log_info("  stream-id: 0x%"PRIx64, AVBTP_PACKET_MAAP_GET_STREAM_ID(p));
	addr = AVBTP_PACKET_MAAP_GET_REQUEST_START(p);
	pw_log_info("  request-start: %02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	pw_log_info("  request-count: %d", AVBTP_PACKET_MAAP_GET_REQUEST_COUNT(p));
	addr = AVBTP_PACKET_MAAP_GET_CONFLICT_START(p);
	pw_log_info("  conflict-start: %02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	pw_log_info("  conflict-count: %d", AVBTP_PACKET_MAAP_GET_CONFLICT_COUNT(p));
}

static int maap_message(void *data, uint64_t now, const uint8_t src[6], const void *message, int len)
{
	struct maap *maap = data;
	const struct avbtp_packet_maap *p = message;

	if (AVBTP_PACKET_GET_SUBTYPE(p) != AVBTP_SUBTYPE_MAAP)
		return 0;

	if (maap->server->debug_messages)
		maap_message_debug(maap, p);

	return 0;
}

static void maap_destroy(void *data)
{
	struct maap *maap = data;
	spa_hook_remove(&maap->server_listener);
	free(maap);
}

static const struct server_events server_events = {
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = maap_destroy,
	.message = maap_message
};

int avbtp_maap_register(struct server *server)
{
	struct maap *maap;

	maap = calloc(1, sizeof(*maap));
	if (maap == NULL)
		return -errno;

	maap->server = server;

	avdecc_server_add_listener(server, &maap->server_listener, &server_events, maap);

	return 0;
}
