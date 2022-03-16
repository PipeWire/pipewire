/* PipeWire
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

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <limits.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <spa/support/cpu.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "avb.h"
#include "packets.h"
#include "internal.h"
#include "adp.h"
#include "aecp.h"
#include "maap.h"

#define DEFAULT_INTERVAL	1

#define server_emit(s,m,v,...) spa_hook_list_call(&s->listener_list, struct server_events, m, v, ##__VA_ARGS__)
#define server_emit_destroy(s)		server_emit(s, destroy, 0)
#define server_emit_message(s,n,mc,m,l)	server_emit(s, message, 0, n, mc, m, l)
#define server_emit_periodic(s,n)	server_emit(s, periodic, 0, n)
#define server_emit_command(s,n,c,a)	server_emit(s, command, 0, n, c, a)

static void on_timer_event(void *data, uint64_t expirations)
{
	struct server *server = data;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	server_emit_periodic(server, SPA_TIMESPEC_TO_NSEC(&now));
}

static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct server *server = data;
	struct timespec now;

	if (mask & SPA_IO_IN) {
		int len, count;
		struct sockaddr_ll sll;
		uint8_t buffer[2048];
		socklen_t sl;

		sl = sizeof(sll);
		len = recvfrom(fd, buffer, sizeof(buffer), 0,
				(struct sockaddr *)&sll, &sl);

		if (sll.sll_protocol != htons(ETH_P_TSN))
			return;

		if (len < 0) {
			pw_log_warn("got recv error: %m");
		}
		else if (len < (int)sizeof(struct avbtp_packet_header)) {
			pw_log_warn("short packet received (%d < %d)", len,
					(int)sizeof(struct avbtp_packet_header));
		} else {
			clock_gettime(CLOCK_REALTIME, &now);
			server_emit_message(server, SPA_TIMESPEC_TO_NSEC(&now),
					sll.sll_addr, buffer, len);
		}
	}
}

int avbtp_server_send_packet(struct server *server, const uint8_t dest[6], void *data, size_t size)
{
	struct sockaddr_ll sll;
	int res = 0;

	spa_zero(sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_TSN);
        sll.sll_ifindex = server->ifindex;
	sll.sll_halen = ETH_ALEN;
	memcpy(sll.sll_addr, dest, ETH_ALEN);

	if ((res = sendto(server->source->fd, data, size, 0,
				(struct sockaddr *)&sll, sizeof(sll))) < 0) {
		res = -errno;
		pw_log_warn("got send error: %m");
	}
	return res;
}

int avbtp_server_broadcast_packet(struct server *server, void *data, size_t size)
{
	static const uint8_t dest[6] = { 0x91, 0xe0, 0xf0, 0x01, 0x00, 0x00 };
	return avbtp_server_send_packet(server, dest, data, size);
}

static int setup_socket(struct server *server)
{
	struct impl *impl = server->impl;
	int fd, res;
	struct ifreq req;
	struct packet_mreq mreq;
	struct sockaddr_ll sll;
	struct timespec value, interval;

	fd = socket(AF_PACKET, SOCK_DGRAM|SOCK_NONBLOCK, htons(ETH_P_ALL));
	if (fd < 0) {
		pw_log_error("socket() failed: %m");
		return -errno;
	}

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	if (ioctl(fd, SIOCGIFINDEX, &req) < 0) {
		res = -errno;
		pw_log_error("SIOCGIFINDEX %s failed: %m", server->ifname);
		goto error_close;
	}
	server->ifindex = req.ifr_ifindex;

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	if (ioctl(fd, SIOCGIFHWADDR, &req) < 0) {
		res = -errno;
		pw_log_error("SIOCGIFHWADDR %s failed: %m", server->ifname);
		goto error_close;
	}
	memcpy(server->mac_addr, req.ifr_hwaddr.sa_data, sizeof(server->mac_addr));

	server->entity_id = (uint64_t)server->mac_addr[0] << 56 |
			(uint64_t)server->mac_addr[1] << 48 |
			(uint64_t)server->mac_addr[2] << 40 |
			(uint64_t)0xff << 32 |
			(uint64_t)0xfe << 24 |
			(uint64_t)server->mac_addr[3] << 16 |
			(uint64_t)server->mac_addr[4] << 8 |
			(uint64_t)server->mac_addr[5];

	pw_log_info("%lx %d", server->entity_id, server->ifindex);

	spa_zero(mreq);
	mreq.mr_ifindex = server->ifindex;
	mreq.mr_type = PACKET_MR_PROMISC;
	if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
				&mreq, sizeof(mreq)) < 0) {
		res = -errno;
		pw_log_error("setsockopt(ADD_MEMBERSHIP) failed: %m");
		goto error_close;
	}

	spa_zero(sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = server->ifindex;
	if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error_close;
	}

	server->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, true, on_socket_data, server);
	if (server->source == NULL) {
		res = -errno;
		pw_log_error("server %p: can't create server source: %m", impl);
		goto error_close;
	}
	server->timer = pw_loop_add_timer(impl->loop, on_timer_event, server);
	if (server->timer == NULL) {
		res = -errno;
		pw_log_error("server %p: can't create timer source: %m", impl);
		goto error_close;
	}
	value.tv_sec = 0;
        value.tv_nsec = 1;
	interval.tv_sec = DEFAULT_INTERVAL;
        interval.tv_nsec = 0;
        pw_loop_update_timer(impl->loop, server->timer, &value, &interval, false);

	return 0;

error_close:
	close(fd);
	return res;
}

struct server *avdecc_server_new(struct impl *impl, const char *ifname, struct spa_dict *props)
{
	struct server *server;
	int res = 0;
	struct timespec now;

	server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;

	server->impl = impl;
	spa_list_append(&impl->servers, &server->link);
	server->ifname = strdup(ifname);
	spa_hook_list_init(&server->listener_list);

	server->debug_messages = true;

	if ((res = setup_socket(server)) < 0)
		goto error_free;

	avbtp_adp_register(server);
	avbtp_aecp_register(server);
	avbtp_maap_register(server);

	clock_gettime(CLOCK_REALTIME, &now);
	server_emit_command(server, SPA_TIMESPEC_TO_NSEC(&now),
			"/adp/advertise",
			"{"
			"  valid-time = 10 "
			"  entity-model-id = \"00:01:02:03:04:05:0600\" "
			"  entity-capabilities = [ "
			"                         aem-supported "
			"                         class-a-supported "
			"                         gptp-supported "
			"                         aem-identify-control-index-valid "
			"                         aem-interface-index-valid ] "
			"  talker-stream-sources = 5 "
			"  talker-capabilities = [ implemented audio-source ] "
			"  listener-stream-sinks = 4 "
			"  listener-capabilities = [ implemented audio-sink ] "
			"  controller-capabilities = [ ] "
			"  available-index = 10 "
			"  gptp-grandmaster-id = \"10:20:30:40:50:60:0001\" "
			"  gptp-domain-number = 6 "
			"  association-id = 0001 "
			"}");
	return server;

error_free:
	free(server);
	if (res < 0)
		errno = -res;
	return NULL;
}

void avdecc_server_add_listener(struct server *server,
			   struct spa_hook *listener,
			   const struct server_events *events,
			   void *data)
{
	spa_hook_list_append(&server->listener_list, listener, events, data);
}

void avdecc_server_free(struct server *server)
{
	struct impl *impl = server->impl;

	spa_list_remove(&server->link);
	if (server->source)
		pw_loop_destroy_source(impl->loop, server->source);
	if (server->timer)
		pw_loop_destroy_source(impl->loop, server->source);
	spa_hook_list_clean(&server->listener_list);
	free(server);
}
