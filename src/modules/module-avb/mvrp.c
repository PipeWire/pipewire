/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include <pipewire/pipewire.h>

#include "mvrp.h"

static const uint8_t mvrp_mac[6] = AVB_MVRP_MAC;

struct attr {
	struct avb_mvrp_attribute attr;
	struct spa_hook listener;
	struct spa_list link;
	struct mvrp *mvrp;
};

struct mvrp {
	struct server *server;
	struct spa_hook server_listener;
	struct spa_hook mrp_listener;

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

static void debug_vid(const struct avb_packet_mvrp_vid *t)
{
	pw_log_info("vid");
	pw_log_info(" %d", ntohs(t->vlan));
}

static int process_vid(struct mvrp *mvrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	return mvrp_attr_event(mvrp, now, attr_type, event);
}

static int encode_vid(struct mvrp *mvrp, struct attr *a, void *m)
{
	struct avb_packet_mvrp_msg *msg = m;
	struct avb_packet_mrp_vector *v;
	struct avb_packet_mvrp_vid *d;
	struct avb_packet_mrp_footer *f;
	uint8_t *ev;
	size_t attr_list_length = sizeof(*v) + sizeof(*d) + sizeof(*f) + 1;

	msg->attribute_type = AVB_MVRP_ATTRIBUTE_TYPE_VID;
	msg->attribute_length = sizeof(*d);

	v = (struct avb_packet_mrp_vector *)msg->attribute_list;
	v->lva = 0;
	AVB_MRP_VECTOR_SET_NUM_VALUES(v, 1);

	d = (struct avb_packet_mvrp_vid *)v->first_value;
	*d = a->attr.attr.vid;

	ev = SPA_PTROFF(d, sizeof(*d), uint8_t);
	*ev = a->attr.mrp->pending_send * 36;

	f = SPA_PTROFF(ev, sizeof(*ev), struct avb_packet_mrp_footer);
	f->end_mark = 0;

	return attr_list_length + sizeof(*msg);
}

static void notify_vid(struct mvrp *mvrp, uint64_t now, struct attr *attr, uint8_t notify)
{
	pw_log_info("> notify vid: %s", avb_mrp_notify_name(notify));
	debug_vid(&attr->attr.attr.vid);
}

static const struct {
	const char *name;
	int (*process) (struct mvrp *mvrp, uint64_t now, uint8_t attr_type,
			const void *m, uint8_t event, uint8_t param, int num);
	int (*encode) (struct mvrp *mvrp, struct attr *attr, void *m);
	void (*notify) (struct mvrp *mvrp, uint64_t now, struct attr *attr, uint8_t notify);
} dispatch[] = {
	[AVB_MVRP_ATTRIBUTE_TYPE_VID] = { "vid", process_vid, encode_vid, notify_vid },
};

static int mvrp_process(void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index)
{
	struct mvrp *mvrp = data;
	return dispatch[attribute_type].process(mvrp, now,
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

static void mvrp_notify(void *data, uint64_t now, uint8_t notify)
{
	struct attr *a = data;
	struct mvrp *mvrp = a->mvrp;
	return dispatch[a->attr.type].notify(mvrp, now, a, notify);
}

static const struct avb_mrp_attribute_events mrp_attr_events = {
	AVB_VERSION_MRP_ATTRIBUTE_EVENTS,
	.notify = mvrp_notify,
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
	avb_mrp_attribute_add_listener(attr, &a->listener, &mrp_attr_events, a);

	return &a->attr;
}

static void mvrp_event(void *data, uint64_t now, uint8_t event)
{
	struct mvrp *mvrp = data;
	uint8_t buffer[2048];
	struct avb_packet_mrp *p = (struct avb_packet_mrp*)buffer;
	struct avb_packet_mrp_footer *f;
	void *msg = SPA_PTROFF(buffer, sizeof(*p), void);
	struct attr *a;
	int len, count = 0;
	size_t total = sizeof(*p) + 2;

	p->version = AVB_MRP_PROTOCOL_VERSION;

	spa_list_for_each(a, &mvrp->attributes, link) {
		if (!a->attr.mrp->pending_send)
			continue;
		if (dispatch[a->attr.type].encode == NULL)
			continue;

		pw_log_debug("send %s %s", dispatch[a->attr.type].name,
				avb_mrp_send_name(a->attr.mrp->pending_send));

		len = dispatch[a->attr.type].encode(mvrp, a, msg);
		if (len < 0)
			break;

		count++;
		msg = SPA_PTROFF(msg, len, void);
		total += len;
	}
	f = (struct avb_packet_mrp_footer *)msg;
	f->end_mark = 0;

	if (count > 0)
		avb_server_send_packet(mvrp->server, mvrp_mac, AVB_MVRP_ETH,
				buffer, total);
}

static const struct avb_mrp_events mrp_events = {
	AVB_VERSION_MRP_EVENTS,
	.event = mvrp_event,
};

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
	avb_mrp_add_listener(server->mrp, &mvrp->mrp_listener, &mrp_events, mvrp);

	return (struct avb_mvrp*)mvrp;

error_no_source:
	free(mvrp);
error_close:
	close(fd);
	errno = -res;
	return NULL;
}
