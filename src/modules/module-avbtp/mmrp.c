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
	struct avbtp_mmrp_attribute attr;
	struct spa_list link;
	struct avbtp_mrp_attribute *a;
	uint8_t addr[6];
};

struct mmrp {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_list attributes;
};

static struct attr *find_attr_by_addr(struct mmrp *mmrp, const uint8_t addr[6])
{
	struct attr *a;
	spa_list_for_each(a, &mmrp->attributes, link)
		if (memcmp(a->addr, addr, 6) == 0)
			return a;
	return NULL;
}

static bool mmrp_check_header(void *data, const void *hdr, size_t *hdr_size, bool *has_params)
{
	const struct avbtp_packet_mmrp_msg *msg = hdr;
	uint8_t attr_type = msg->attribute_type;

	if (!AVBTP_MMRP_ATTRIBUTE_TYPE_VALID(attr_type))
		return false;

	*hdr_size = sizeof(*msg);
	*has_params = false;
	return true;
}

static int mmrp_attr_event(void *data, uint64_t now, uint8_t attribute_type, uint8_t event)
{
	struct mmrp *mmrp = data;
	struct attr *a;
	pw_log_info("leave all");
	spa_list_for_each(a, &mmrp->attributes, link)
		if (a->attr.type == attribute_type)
			avbtp_mrp_update_state(mmrp->server->mrp, now, a->a, event);
	return 0;
}

static int process_service_requirement(struct mmrp *mmrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_mmrp_service_requirement *t = m;
	char buf[128];
	struct attr *a;

	pw_log_info("service requirement");
	pw_log_info(" %s", avbtp_utils_format_addr(buf, sizeof(buf), t->addr));

	a = find_attr_by_addr(mmrp, t->addr);
	if (a)
		avbtp_mrp_rx_event(mmrp->server->mrp, now, a->a, event);
	return 0;
}

static int process_mac(struct mmrp *mmrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_mmrp_mac *t = m;
	char buf[128];
	struct attr *a;

	pw_log_info("mac");
	pw_log_info(" %s", avbtp_utils_format_addr(buf, sizeof(buf), t->addr));

	a = find_attr_by_addr(mmrp, t->addr);
	if (a)
		avbtp_mrp_rx_event(mmrp->server->mrp, now, a->a, event);
	return 0;
}

static const struct {
	int (*dispatch) (struct mmrp *mmrp, uint64_t now, uint8_t attr_type,
			const void *m, uint8_t event, uint8_t param, int num);
} dispatch[] = {
	[AVBTP_MMRP_ATTRIBUTE_TYPE_SERVICE_REQUIREMENT] = { process_service_requirement, },
	[AVBTP_MMRP_ATTRIBUTE_TYPE_MAC] = { process_mac, },
};

static int mmrp_process(void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index)
{
	struct mmrp *mmrp = data;
	return dispatch[attribute_type].dispatch(mmrp, now,
				attribute_type, value, event, param, index);
}

static const struct avbtp_mrp_parse_info info = {
	AVBTP_VERSION_MRP_PARSE_INFO,
	.check_header = mmrp_check_header,
	.attr_event = mmrp_attr_event,
	.process = mmrp_process,
};

static int mmrp_message(void *data, uint64_t now, const void *message, int len)
{
	struct mmrp *mmrp = data;
	const struct avbtp_packet_mrp *p = message;

	if (ntohs(p->eth.type) != AVB_MMRP_ETH)
		return 0;
	if (memcmp(p->eth.dest, mac, 6) != 0)
		return 0;

	pw_log_info("MMRP");
	return avbtp_mrp_parse_packet(mmrp->server->mrp,
			now, message, len, &info, mmrp);
}

static void mmrp_destroy(void *data)
{
	struct mmrp *mmrp = data;
	spa_hook_remove(&mmrp->server_listener);
	free(mmrp);
}

static const struct server_events server_events = {
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = mmrp_destroy,
	.message = mmrp_message
};

static int mmrp_attr_compare(void *data, struct avbtp_mrp_attribute *a, struct avbtp_mrp_attribute *b)
{
	return 0;
}

static int mmrp_attr_merge(void *data, struct avbtp_mrp_attribute *a, int vector)
{
	return 0;
}

static const struct avbtp_mrp_attribute_callbacks attr_cb = {
	AVBTP_VERSION_MRP_ATTRIBUTE_CALLBACKS,
	.compare = mmrp_attr_compare,
	.merge = mmrp_attr_merge
};

struct avbtp_mmrp_attribute *avbtp_mmrp_attribute_new(struct avbtp_mmrp *m,
		uint8_t type)
{
	struct mmrp *mmrp = (struct mmrp*)m;
	struct avbtp_mrp_attribute *attr;
	struct attr *a;

	attr = avbtp_mrp_attribute_new(mmrp->server->mrp,
		&attr_cb, mmrp, sizeof(struct attr));

	a = attr->user_data;
	a->a = attr;
	a->attr.type = type;

	spa_list_append(&mmrp->attributes, &a->link);

	return &a->attr;
}

struct avbtp_mmrp *avbtp_mmrp_register(struct server *server)
{
	struct mmrp *mmrp;

	mmrp = calloc(1, sizeof(*mmrp));
	if (mmrp == NULL)
		return NULL;

	mmrp->server = server;
	spa_list_init(&mmrp->attributes);

	avdecc_server_add_listener(server, &mmrp->server_listener, &server_events, mmrp);

	return (struct avbtp_mmrp*)mmrp;
}
