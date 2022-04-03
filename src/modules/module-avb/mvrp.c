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

#include "mvrp.h"

static const uint8_t mvrp_mac[6] = AVB_MVRP_MAC;

struct attr {
	struct avb_mvrp_attribute attr;
	struct spa_list link;
};

struct mvrp {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_source *source;

	struct spa_list attributes;
};

static bool mvrp_check_header(void *data, const void *hdr, size_t *hdr_size, bool *has_params)
{
	const struct avb_packet_mvrp_msg *msg = hdr;
	uint8_t attr_type = msg->attribute_type;

	if (!AVB_MVRP_ATTRIBUTE_TYPE_VALID(attr_type))
		return false;

	*hdr_size = sizeof(*msg);
	*has_params = false;
	return true;
}

static int mvrp_attr_event(void *data, uint64_t now, uint8_t attribute_type, uint8_t event)
{
	struct mvrp *mvrp = data;
	struct attr *a;
	spa_list_for_each(a, &mvrp->attributes, link)
		if (a->attr.type == attribute_type)
			avb_mrp_attribute_rx_event(a->attr.mrp, now, event);
	return 0;
}

static void debug_vid(const void *p)
{
	const struct avb_packet_mvrp_vid *t = p;
	pw_log_info("vid");
	pw_log_info(" %d", ntohs(t->vlan));
}

static int process_vid(struct mvrp *mvrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	return mvrp_attr_event(mvrp, now, attr_type, event);
}

static const struct {
	void (*debug) (const void *p);
	int (*dispatch) (struct mvrp *mvrp, uint64_t now, uint8_t attr_type,
			const void *m, uint8_t event, uint8_t param, int num);
} dispatch[] = {
	[AVB_MVRP_ATTRIBUTE_TYPE_VID] = { debug_vid, process_vid, },
};

static int mvrp_process(void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index)
{
	struct mvrp *mvrp = data;
	return dispatch[attribute_type].dispatch(mvrp, now,
				attribute_type, value, event, param, index);
}

static const struct avb_mrp_parse_info info = {
	AVB_VERSION_MRP_PARSE_INFO,
	.check_header = mvrp_check_header,
	.attr_event = mvrp_attr_event,
	.process = mvrp_process,
};

static int mvrp_message(struct mvrp *mvrp, uint64_t now, const void *message, int len)
{
	pw_log_debug("MVRP");
	return avb_mrp_parse_packet(mvrp->server->mrp,
			now, message, len, &info, mvrp);
}

static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct mvrp *mvrp = data;
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
			mvrp_message(mvrp, SPA_TIMESPEC_TO_NSEC(&now), buffer, len);
		}
	}
}

static void mvrp_destroy(void *data)
{
	struct mvrp *mvrp = data;
	spa_hook_remove(&mvrp->server_listener);
	pw_loop_destroy_source(mvrp->server->impl->loop, mvrp->source);
	free(mvrp);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = mvrp_destroy,
};

struct avb_mvrp_attribute *avb_mvrp_attribute_new(struct avb_mvrp *m,
		uint8_t type)
{
	struct mvrp *mvrp = (struct mvrp*)m;
	struct avb_mrp_attribute *attr;
	struct attr *a;

	attr = avb_mrp_attribute_new(mvrp->server->mrp, sizeof(struct attr));

	a = attr->user_data;
	a->attr.mrp = attr;
	a->attr.type = type;
	spa_list_append(&mvrp->attributes, &a->link);

	return &a->attr;
}

struct avb_mvrp *avb_mvrp_register(struct server *server)
{
	struct mvrp *mvrp;
	int fd, res;

	fd = avb_server_make_socket(server, AVB_MVRP_ETH, mvrp_mac);
	if (fd < 0) {
		errno = -fd;
		return NULL;
	}
	mvrp = calloc(1, sizeof(*mvrp));
	if (mvrp == NULL) {
		res = -errno;
		goto error_close;
	}

	mvrp->server = server;
	spa_list_init(&mvrp->attributes);

	mvrp->source = pw_loop_add_io(server->impl->loop, fd, SPA_IO_IN, true, on_socket_data, mvrp);
	if (mvrp->source == NULL) {
		res = -errno;
		pw_log_error("mvrp %p: can't create mvrp source: %m", mvrp);
		goto error_no_source;
	}
	avdecc_server_add_listener(server, &mvrp->server_listener, &server_events, mvrp);

	return (struct avb_mvrp*)mvrp;

error_no_source:
	free(mvrp);
error_close:
	close(fd);
	errno = -res;
	return NULL;
}
