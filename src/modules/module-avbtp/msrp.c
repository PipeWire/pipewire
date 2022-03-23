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

#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "utils.h"
#include "msrp.h"

static const uint8_t mac[6] = AVB_MSRP_MAC;

struct attr {
	struct avbtp_msrp_attribute attr;
	struct spa_list link;
};

struct msrp {
	struct server *server;
	struct spa_hook server_listener;
	struct spa_hook mrp_listener;

	struct spa_list attributes;
};

static struct attr *find_attr_by_stream_id(struct msrp *msrp, uint64_t stream_id)
{
	struct attr *a;
	spa_list_for_each(a, &msrp->attributes, link)
		if (a->attr.attr.talker.stream_id == stream_id)
			return a;
	return NULL;
}

static void debug_msrp_talker(const struct avbtp_packet_msrp_talker *t)
{
	char buf[128];
	pw_log_info(" stream-id: %s", avbtp_utils_format_id(buf, sizeof(buf), be64toh(t->stream_id)));
	pw_log_info(" dest-addr: %s", avbtp_utils_format_addr(buf, sizeof(buf), t->dest_addr));
	pw_log_info(" vlan-id:   %d", ntohs(t->vlan_id));
	pw_log_info(" tspec-max-frame-size: %d", ntohs(t->tspec_max_frame_size));
	pw_log_info(" tspec-max-interval-frames: %d", ntohs(t->tspec_max_interval_frames));
	pw_log_info(" priority: %d", t->priority);
	pw_log_info(" rank: %d", t->rank);
	pw_log_info(" accumulated-latency: %d", ntohl(t->accumulated_latency));
}

static int process_talker(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_talker *t = m;
	struct attr *a;

	pw_log_info("talker");
	debug_msrp_talker(t);

	a = find_attr_by_stream_id(msrp, be64toh(t->stream_id));
	if (a)
		avbtp_mrp_rx_event(msrp->server->mrp, now, a->attr.mrp, event);
	return 0;
}

static void debug_msrp_talker_fail(const struct avbtp_packet_msrp_talker_fail *t)
{
	char buf[128];
	debug_msrp_talker(&t->talker);
	pw_log_info(" bridge-id: %s", avbtp_utils_format_id(buf, sizeof(buf), be64toh(t->bridge_id)));
	pw_log_info(" failure-code: %d", t->failure_code);
}

static int process_talker_fail(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_talker_fail *t = m;
	pw_log_info("talker fail");
	debug_msrp_talker_fail(t);
	return 0;
}

static void debug_msrp_listener(const struct avbtp_packet_msrp_listener *l)
{
	char buf[128];
	pw_log_info(" %s", avbtp_utils_format_id(buf, sizeof(buf), l->stream_id));
}

static int process_listener(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_listener *l = m;
	pw_log_info("listener");
	debug_msrp_listener(l);
	return 0;
}

static void debug_msrp_domain(const struct avbtp_packet_msrp_domain *d)
{
	pw_log_info(" %d", d->sr_class_id);
	pw_log_info(" %d", d->sr_class_priority);
	pw_log_info(" %d", ntohs(d->sr_class_vid));
}

static int process_domain(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_domain *d = m;
	pw_log_info("domain");
	debug_msrp_domain(d);
	return 0;
}

static const struct {
	int (*dispatch) (struct msrp *msrp, uint64_t now, uint8_t attr_type,
			const void *m, uint8_t event, uint8_t param, int num);
} dispatch[] = {
	[AVBTP_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE] = { process_talker, },
	[AVBTP_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED] = { process_talker_fail, },
	[AVBTP_MSRP_ATTRIBUTE_TYPE_LISTENER] = { process_listener, },
	[AVBTP_MSRP_ATTRIBUTE_TYPE_DOMAIN] = { process_domain, },
};

static bool msrp_check_header(void *data, const void *hdr, size_t *hdr_size, bool *has_params)
{
	const struct avbtp_packet_msrp_msg *msg = hdr;
	uint8_t attr_type = msg->attribute_type;

	if (!AVBTP_MSRP_ATTRIBUTE_TYPE_VALID(attr_type))
		return false;

	*hdr_size = sizeof(*msg);
	*has_params = attr_type == AVBTP_MSRP_ATTRIBUTE_TYPE_LISTENER;
	return true;
}

static int msrp_attr_event(void *data, uint64_t now, uint8_t attribute_type, uint8_t event)
{
	struct msrp *msrp = data;
	struct attr *a;
	spa_list_for_each(a, &msrp->attributes, link)
		if (a->attr.type == attribute_type)
			avbtp_mrp_update_state(msrp->server->mrp, now, a->attr.mrp, event);
	return 0;
}

static int msrp_process(void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index)
{
	struct msrp *msrp = data;
	return dispatch[attribute_type].dispatch(msrp, now,
				attribute_type, value, event, param, index);
}

static const struct avbtp_mrp_parse_info info = {
	AVBTP_VERSION_MRP_PARSE_INFO,
	.check_header = msrp_check_header,
	.attr_event = msrp_attr_event,
	.process = msrp_process,
};


static int msrp_message(void *data, uint64_t now, const void *message, int len)
{
	struct msrp *msrp = data;
	const struct avbtp_packet_mrp *p = message;

	if (ntohs(p->eth.type) != AVB_MSRP_ETH)
		return 0;
	if (memcmp(p->eth.dest, mac, 6) != 0)
		return 0;

	pw_log_info("MSRP");
	return avbtp_mrp_parse_packet(msrp->server->mrp,
			now, message, len, &info, msrp);
}

static void msrp_destroy(void *data)
{
	struct msrp *msrp = data;
	spa_hook_remove(&msrp->server_listener);
	free(msrp);
}

static const struct server_events server_events = {
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = msrp_destroy,
	.message = msrp_message
};

static int msrp_attr_compare(void *data, struct avbtp_mrp_attribute *a, struct avbtp_mrp_attribute *b)
{
	return 0;
}

static int msrp_attr_merge(void *data, struct avbtp_mrp_attribute *a, int vector)
{
	pw_log_info("attr merge");
	return 0;
}

static const struct avbtp_mrp_attribute_callbacks attr_cb = {
	AVBTP_VERSION_MRP_ATTRIBUTE_CALLBACKS,
	.compare = msrp_attr_compare,
	.merge = msrp_attr_merge
};

struct avbtp_msrp_attribute *avbtp_msrp_attribute_new(struct avbtp_msrp *m,
		uint8_t type)
{
	struct msrp *msrp = (struct msrp*)m;
	struct avbtp_mrp_attribute *attr;
	struct attr *a;

	attr = avbtp_mrp_attribute_new(msrp->server->mrp,
		&attr_cb, msrp, sizeof(struct attr));

	a = attr->user_data;
	a->attr.mrp = attr;
	spa_list_append(&msrp->attributes, &a->link);
	a->attr.type = type;

	return &a->attr;
}

static int msrp_tx_event(void *data, uint8_t event, bool start)
{
	pw_log_info("tx %s", start ? "start" : "stop");
	return 0;
}

static const struct avbtp_mrp_events mrp_events = {
	AVBTP_VERSION_MRP_ATTRIBUTE_CALLBACKS,
	.tx_event = msrp_tx_event
};

struct avbtp_msrp *avbtp_msrp_register(struct server *server)
{
	struct msrp *msrp;

	msrp = calloc(1, sizeof(*msrp));
	if (msrp == NULL)
		return NULL;

	msrp->server = server;
	spa_list_init(&msrp->attributes);

	avdecc_server_add_listener(server, &msrp->server_listener, &server_events, msrp);
	avbtp_mrp_add_listener(server->mrp, &msrp->mrp_listener, &mrp_events, msrp);

	return (struct avbtp_msrp*)msrp;
}
