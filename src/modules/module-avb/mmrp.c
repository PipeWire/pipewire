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

#include "utils.h"
#include "mmrp.h"

static const uint8_t mac[6] = AVB_MMRP_MAC;

struct attr {
	struct avb_mmrp_attribute attr;
	struct spa_list link;
};

struct mmrp {
	struct server *server;
	struct spa_hook server_listener;

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
			avb_mrp_update_state(mmrp->server->mrp, now, a->attr.mrp, event);
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
			avb_mrp_rx_event(mmrp->server->mrp, now, a->attr.mrp, event);
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
			avb_mrp_rx_event(mmrp->server->mrp, now, a->attr.mrp, event);
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

static int mmrp_message(void *data, uint64_t now, const void *message, int len)
{
	struct mmrp *mmrp = data;
	const struct avb_packet_mrp *p = message;

	if (ntohs(p->eth.type) != AVB_MMRP_ETH)
		return 0;
	if (memcmp(p->eth.dest, mac, 6) != 0)
		return 0;

	pw_log_debug("MMRP");
	return avb_mrp_parse_packet(mmrp->server->mrp,
			now, message, len, &info, mmrp);
}

static void mmrp_destroy(void *data)
{
	struct mmrp *mmrp = data;
	spa_hook_remove(&mmrp->server_listener);
	free(mmrp);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = mmrp_destroy,
	.message = mmrp_message
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

	mmrp = calloc(1, sizeof(*mmrp));
	if (mmrp == NULL)
		return NULL;

	mmrp->server = server;
	spa_list_init(&mmrp->attributes);

	avdecc_server_add_listener(server, &mmrp->server_listener, &server_events, mmrp);

	return (struct avb_mmrp*)mmrp;
}
