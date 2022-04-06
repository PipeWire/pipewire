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
#include <pipewire/conf.h>

#include "maap.h"

#define MAAP_ALLOCATION_POOL_SIZE	0xFE00
#define MAAP_ALLOCATION_POOL_BASE	 { 0x91, 0xe0, 0xf0, 0x00, 0x00, 0x00 }
static uint8_t maap_base[6] = MAAP_ALLOCATION_POOL_BASE;

#define MAAP_PROBE_RETRANSMITS		3

#define MAAP_PROBE_INTERVAL_MS		500
#define MAAP_PROBE_INTERVAL_VAR_MS	100

#define MAAP_ANNOUNCE_INTERVAL_MS	3000
#define MAAP_ANNOUNCE_INTERVAL_VAR_MS	2000

struct maap {
	struct server *server;
	struct spa_hook server_listener;

	struct pw_properties *props;

	struct spa_source *source;

#define STATE_IDLE	0
#define STATE_PROBE	1
#define STATE_ANNOUNCE	2
	uint32_t state;
	uint64_t timeout;
	uint32_t probe_count;

	unsigned short xsubi[3];

	uint16_t offset;
	uint16_t count;
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

#define PROBE_TIMEOUT(n) ((n) + (MAAP_PROBE_INTERVAL_MS + \
                        drand48() * MAAP_PROBE_INTERVAL_VAR_MS) * SPA_NSEC_PER_MSEC)
#define ANNOUNCE_TIMEOUT(n) ((n) + (MAAP_ANNOUNCE_INTERVAL_MS + \
                        drand48() * MAAP_ANNOUNCE_INTERVAL_VAR_MS) * SPA_NSEC_PER_MSEC)

static int make_new_address(struct maap *maap, uint64_t now, int range)
{
	maap->offset = nrand48(maap->xsubi) % (MAAP_ALLOCATION_POOL_SIZE - range);
	maap->count = range;
	maap->state = STATE_PROBE;
	maap->probe_count = MAAP_PROBE_RETRANSMITS;
	maap->timeout = PROBE_TIMEOUT(now);
	return 0;
}

static uint16_t maap_check_conflict(struct maap *maap, const uint8_t request_start[6],
		uint16_t request_count, uint8_t conflict_start[6])
{
	uint16_t our_start, our_end;
	uint16_t req_start, req_end;
	uint16_t conf_start, conf_count = 0;

	if (memcmp(request_start, maap_base, 4) != 0)
		return 0;

	our_start = maap->offset;
	our_end = our_start + maap->count;
	req_start = request_start[4] << 8 | request_start[5];
	req_end = req_start + request_count;

	if (our_start >= req_start && our_start <= req_end) {
		conf_start = our_start;
		conf_count = SPA_MIN(our_end, req_end) - our_start;
	}
	else if (req_start >= our_start && req_start <= our_end) {
		conf_start = req_start;
		conf_count = SPA_MIN(req_end, our_end) - req_start;
	}
	if (conf_count == 0)
		return 0;

	conflict_start[4] = conf_start >> 8;
	conflict_start[5] = conf_start;
	return conf_count;
}

static int send_packet(struct maap *maap, uint64_t now,
		uint8_t type, const uint8_t conflict_start[6], uint16_t conflict_count)
{
	struct avb_packet_maap p;
	uint8_t bmac[6] = AVB_MAAP_MAC;
	int res = 0;
	uint8_t start[6];

	spa_zero(p);
	memcpy(p.hdr.eth.dest, bmac, 6);
	memcpy(p.hdr.eth.src, maap->server->mac_addr, 6);
	p.hdr.eth.type = htons(AVB_TSN_ETH);
	p.hdr.subtype = AVB_SUBTYPE_MAAP;
	AVB_PACKET_SET_LENGTH(&p.hdr, sizeof(p) - sizeof(p.hdr.eth));

	AVB_PACKET_MAAP_SET_MAAP_VERSION(&p, 1);
	AVB_PACKET_MAAP_SET_MESSAGE_TYPE(&p, type);

	memcpy(start, maap_base, 4);
	start[4] = maap->offset >> 8;
	start[5] = maap->offset;
	AVB_PACKET_MAAP_SET_REQUEST_START(&p, start);
	AVB_PACKET_MAAP_SET_REQUEST_COUNT(&p, maap->count);
	if (conflict_count) {
		AVB_PACKET_MAAP_SET_CONFLICT_START(&p, conflict_start);
		AVB_PACKET_MAAP_SET_CONFLICT_COUNT(&p, conflict_count);
	}

	pw_log_info("send: %d (%s)", type, message_type_as_string(type));
	maap_message_debug(maap, &p);

	if (send(maap->source->fd, &p, sizeof(p), 0) < 0) {
		res = -errno;
		pw_log_warn("got send error: %m");
	}
	return res;
}

static int handle_probe(struct maap *maap, uint64_t now, const struct avb_packet_maap *p)
{
	uint8_t conflict_start[6];
	uint16_t conflict_count;

	conflict_count = maap_check_conflict(maap, p->request_start, ntohs(p->request_count),
				conflict_start);
	if (conflict_count == 0)
		return 0;

	switch (maap->state) {
	case STATE_PROBE:
		make_new_address(maap, now, 8);
		break;
	case STATE_ANNOUNCE:
		send_packet(maap, now, AVB_MAAP_MESSAGE_TYPE_DEFEND, conflict_start, conflict_count);
		break;
	}
	return 0;
}

static int handle_defend(struct maap *maap, uint64_t now, const struct avb_packet_maap *p)
{
	uint8_t conflict_start[6];
	uint16_t conflict_count;

	conflict_count = maap_check_conflict(maap, p->conflict_start, ntohs(p->conflict_count),
				conflict_start);
	if (conflict_count != 0)
		make_new_address(maap, now, 8);
	return 0;
}

static int maap_message(struct maap *maap, uint64_t now, const void *message, int len)
{
	const struct avb_packet_maap *p = message;

	if (AVB_PACKET_GET_SUBTYPE(&p->hdr) != AVB_SUBTYPE_MAAP)
		return 0;

	maap_message_debug(maap, p);

	switch (AVB_PACKET_MAAP_GET_MESSAGE_TYPE(p)) {
	case AVB_MAAP_MESSAGE_TYPE_PROBE:
		handle_probe(maap, now, p);
		break;
	case AVB_MAAP_MESSAGE_TYPE_DEFEND:
	case AVB_MAAP_MESSAGE_TYPE_ANNOUNCE:
		handle_defend(maap, now, p);
		break;
	}
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

static void maap_periodic(void *data, uint64_t now)
{
	struct maap *maap = data;

	if (now < maap->timeout)
		return;

	switch(maap->state) {
	case STATE_IDLE:
		break;
	case STATE_PROBE:
		send_packet(maap, now, AVB_MAAP_MESSAGE_TYPE_PROBE, NULL, 0);
		if (--maap->probe_count == 0)
			maap->state = STATE_ANNOUNCE;
		maap->timeout = PROBE_TIMEOUT(now);
		break;
	case STATE_ANNOUNCE:
		send_packet(maap, now, AVB_MAAP_MESSAGE_TYPE_ANNOUNCE, NULL, 0);
		maap->timeout = ANNOUNCE_TIMEOUT(now);
		break;
	}
}

static void maap_free(struct maap *maap)
{
	pw_loop_destroy_source(maap->server->impl->loop, maap->source);
	spa_hook_remove(&maap->server_listener);
	pw_properties_free(maap->props);
	free(maap);
}

static void maap_destroy(void *data)
{
	struct maap *maap = data;
	maap_free(maap);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = maap_destroy,
	.periodic = maap_periodic,
};

struct avb_maap *avb_maap_register(struct server *server)
{
	struct maap *maap;
	uint8_t bmac[6] = AVB_MAAP_MAC;
	int fd, res;

	fd = avb_server_make_socket(server, AVB_TSN_ETH, bmac);
	if (fd < 0) {
		res = fd;
		goto error;
	}

	maap = calloc(1, sizeof(*maap));
	if (maap == NULL) {
		res = -errno;
		goto error_close;
	}
	maap->props = pw_properties_new(NULL, NULL);
	if (maap->props == NULL) {
		res = -errno;
		goto error_free;
	}

	pw_conf_load_state("module-avb", "maap", maap->props);

	maap->server = server;

	pw_getrandom(maap->xsubi, sizeof(maap->xsubi), 0);

	pw_log_info("%lx %d", server->entity_id, server->ifindex);

	maap->source = pw_loop_add_io(server->impl->loop, fd, SPA_IO_IN, true, on_socket_data, maap);
	if (maap->source == NULL) {
		res = -errno;
		pw_log_error("maap %p: can't create maap source: %m", maap);
		goto error_free;
	}
	avdecc_server_add_listener(server, &maap->server_listener, &server_events, maap);

	return (struct avb_maap *)maap;

error_free:
	free(maap);
error_close:
	close(fd);
error:
	errno = -res;
	return NULL;
}

int avb_maap_reserve(struct avb_maap *m, uint32_t count)
{
	struct maap *maap = (struct maap*)m;
	make_new_address(maap, 0, count);
	return 0;
}

int avb_maap_get_address(struct avb_maap *m, uint8_t addr[6], uint32_t index)
{
	struct maap *maap = (struct maap*)m;
	uint16_t offset;

	if (maap->state != STATE_ANNOUNCE)
		return -EAGAIN;

	memcpy(addr, maap_base, 6);
	offset = maap->offset + index;
	addr[4] = offset >> 8;
	addr[5] = offset;
	return 0;
}
