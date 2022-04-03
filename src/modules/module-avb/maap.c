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

#include <unistd.h>

#include <pipewire/pipewire.h>

#include "maap.h"

struct maap {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_source *source;
};

static const char *message_type_as_string(uint8_t message_type)
{
	switch (message_type) {
	case AVB_MAAP_MESSAGE_TYPE_PROBE:
		return "PROBE";
	case AVB_MAAP_MESSAGE_TYPE_DEFEND:
		return "DEFEND";
	case AVB_MAAP_MESSAGE_TYPE_ANNOUNCE:
		return "ANNOUNCE";
	}
	return "INVALID";
}

static void maap_message_debug(struct maap *maap, const struct avb_packet_maap *p)
{
	uint32_t v;
	const uint8_t *addr;

	v = AVB_PACKET_MAAP_GET_MESSAGE_TYPE(p);
	pw_log_info("message-type: %d (%s)", v, message_type_as_string(v));
	pw_log_info("  maap-version: %d", AVB_PACKET_MAAP_GET_MAAP_VERSION(p));
	pw_log_info("  length: %d", AVB_PACKET_GET_LENGTH(&p->hdr));

	pw_log_info("  stream-id: 0x%"PRIx64, AVB_PACKET_MAAP_GET_STREAM_ID(p));
	addr = AVB_PACKET_MAAP_GET_REQUEST_START(p);
	pw_log_info("  request-start: %02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	pw_log_info("  request-count: %d", AVB_PACKET_MAAP_GET_REQUEST_COUNT(p));
	addr = AVB_PACKET_MAAP_GET_CONFLICT_START(p);
	pw_log_info("  conflict-start: %02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	pw_log_info("  conflict-count: %d", AVB_PACKET_MAAP_GET_CONFLICT_COUNT(p));
}

static int maap_message(struct maap *maap, uint64_t now, const void *message, int len)
{
	const struct avb_packet_maap *p = message;

	if (AVB_PACKET_GET_SUBTYPE(&p->hdr) != AVB_SUBTYPE_MAAP)
		return 0;

	maap_message_debug(maap, p);

	return 0;
}

static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct maap *maap = data;
	struct timespec now;

	if (mask & SPA_IO_IN) {
		int len;
		uint8_t buffer[2048];

		len = recv(fd, buffer, sizeof(buffer), 0);

		if (len < 0) {
			pw_log_warn("got recv error: %m");
		}
		else if (len < (int)sizeof(struct avb_packet_header)) {
			pw_log_warn("short packet received (%d < %d)", len,
					(int)sizeof(struct avb_packet_header));
		} else {
			clock_gettime(CLOCK_REALTIME, &now);
			maap_message(maap, SPA_TIMESPEC_TO_NSEC(&now), buffer, len);
		}
	}
}

static void maap_destroy(void *data)
{
	struct maap *maap = data;
	pw_loop_destroy_source(maap->server->impl->loop, maap->source);
	spa_hook_remove(&maap->server_listener);
	free(maap);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = maap_destroy,
};

int avb_maap_register(struct server *server)
{
	struct maap *maap;
	uint8_t bmac[6] = AVB_MAAP_MAC;
	int res, fd;

	fd = avb_server_make_socket(server, AVB_TSN_ETH, bmac);
	if (fd < 0)
		return fd;

	maap = calloc(1, sizeof(*maap));
	if (maap == NULL) {
		res = -errno;
		goto error_close;
	}

	maap->server = server;

	pw_log_info("%lx %d", server->entity_id, server->ifindex);

	maap->source = pw_loop_add_io(server->impl->loop, fd, SPA_IO_IN, true, on_socket_data, maap);
	if (maap->source == NULL) {
		res = -errno;
		pw_log_error("maap %p: can't create maap source: %m", maap);
		goto error_no_source;
	}
	avdecc_server_add_listener(server, &maap->server_listener, &server_events, maap);

	return 0;

error_no_source:
	free(maap);
error_close:
	close(fd);
	return res;
}
