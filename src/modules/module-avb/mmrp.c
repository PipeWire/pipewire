/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include <pipewire/pipewire.h>

#include "utils.h"
#include "mmrp.h"

static const uint8_t mmrp_mac[6] = AVB_MMRP_MAC;

struct attr {
	struct avb_mmrp_attribute attr;
	struct spa_list link;
};

struct mmrp {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_source *source;

	struct spa_list attributes;
};

static bool mmrp_check_header(void *data, const void *hdr, size_t *hdr_size, bool *has_params)
{
	const struct avb_packet_mmrp_msg *msg = hdr;
	uint8_t attr_type = msg->attribute_type;

	if (!AVB_MMRP_ATTRIBUTE_TYPE_VALID(attr_type))
		return false;

	*hdr_size = sizeof(*msg);
	*has_params = false;
	return true;
}

static int mmrp_attr_event(void *data, uint64_t now, uint8_t attribute_type, uint8_t event)
{
	struct mmrp *mmrp = data;
	struct attr *a;
	spa_list_for_each(a, &mmrp->attributes, link)
		if (a->attr.type == attribute_type)
			avb_mrp_attribute_update_state(a->attr.mrp, now, event);
	return 0;
}

static void debug_service_requirement(const struct avb_packet_mmrp_service_requirement *t)
{
	char buf[128];
	pw_log_info("service requirement");
	pw_log_info(" %s", avb_utils_format_addr(buf, sizeof(buf), t->addr));
}

static int process_service_requirement(struct mmrp *mmrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avb_packet_mmrp_service_requirement *t = m;
	struct attr *a;

	debug_service_requirement(t);

	spa_list_for_each(a, &mmrp->attributes, link)
		if (a->attr.type == attr_type &&
		    memcmp(a->attr.attr.service_requirement.addr, t->addr, 6) == 0)
			avb_mrp_attribute_rx_event(a->attr.mrp, now, event);
	return 0;
}

static void debug_process_mac(const struct avb_packet_mmrp_mac *t)
{
	char buf[128];
	pw_log_info("mac");
	pw_log_info(" %s", avb_utils_format_addr(buf, sizeof(buf), t->addr));
}

static int process_mac(struct mmrp *mmrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avb_packet_mmrp_mac *t = m;
	struct attr *a;

	debug_process_mac(t);

	spa_list_for_each(a, &mmrp->attributes, link)
		if (a->attr.type == attr_type &&
		    memcmp(a->attr.attr.mac.addr, t->addr, 6) == 0)
			avb_mrp_attribute_rx_event(a->attr.mrp, now, event);
	return 0;
}

static const struct {
	int (*dispatch) (struct mmrp *mmrp, uint64_t now, uint8_t attr_type,
			const void *m, uint8_t event, uint8_t param, int num);
} dispatch[] = {
	[AVB_MMRP_ATTRIBUTE_TYPE_SERVICE_REQUIREMENT] = { process_service_requirement, },
	[AVB_MMRP_ATTRIBUTE_TYPE_MAC] = { process_mac, },
};

static int mmrp_process(void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index)
{
	struct mmrp *mmrp = data;
	return dispatch[attribute_type].dispatch(mmrp, now,
				attribute_type, value, event, param, index);
}

static const struct avb_mrp_parse_info info = {
	AVB_VERSION_MRP_PARSE_INFO,
	.check_header = mmrp_check_header,
	.attr_event = mmrp_attr_event,
	.process = mmrp_process,
};

static int mmrp_message(struct mmrp *mmrp, uint64_t now, const void *message, int len)
{
	pw_log_debug("MMRP");
	return avb_mrp_parse_packet(mmrp->server->mrp,
			now, message, len, &info, mmrp);
}

static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct mmrp *mmrp = data;
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
			mmrp_message(mmrp, SPA_TIMESPEC_TO_NSEC(&now), buffer, len);
		}
	}
}
static void mmrp_destroy(void *data)
{
	struct mmrp *mmrp = data;
	spa_hook_remove(&mmrp->server_listener);
	pw_loop_destroy_source(mmrp->server->impl->loop, mmrp->source);
	free(mmrp);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = mmrp_destroy,
};

struct avb_mmrp_attribute *avb_mmrp_attribute_new(struct avb_mmrp *m,
		uint8_t type)
{
	struct mmrp *mmrp = (struct mmrp*)m;
	struct avb_mrp_attribute *attr;
	struct attr *a;

	attr = avb_mrp_attribute_new(mmrp->server->mrp, sizeof(struct attr));

	a = attr->user_data;
	a->attr.mrp = attr;
	a->attr.type = type;
	spa_list_append(&mmrp->attributes, &a->link);

	return &a->attr;
}

struct avb_mmrp *avb_mmrp_register(struct server *server)
{
	struct mmrp *mmrp;
	int fd, res;

	fd = avb_server_make_socket(server, AVB_MMRP_ETH, mmrp_mac);
	if (fd < 0) {
		errno = -fd;
		return NULL;
	}
	mmrp = calloc(1, sizeof(*mmrp));
	if (mmrp == NULL) {
		res = -errno;
		goto error_close;
	}

	mmrp->server = server;
	spa_list_init(&mmrp->attributes);

	mmrp->source = pw_loop_add_io(server->impl->loop, fd, SPA_IO_IN, true, on_socket_data, mmrp);
	if (mmrp->source == NULL) {
		res = -errno;
		pw_log_error("mmrp %p: can't create mmrp source: %m", mmrp);
		goto error_no_source;
	}
	avdecc_server_add_listener(server, &mmrp->server_listener, &server_events, mmrp);

	return (struct avb_mmrp*)mmrp;

error_no_source:
	free(mmrp);
error_close:
	close(fd);
	errno = -res;
	return NULL;
}
