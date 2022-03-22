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
	struct spa_list link;
	struct avbtp_mrp_attribute attr;
	uint64_t stream_id;
};

struct msrp {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_list attributes;
};

static struct attr *find_attr_by_stream_id(struct msrp *msrp, uint64_t stream_id)
{
	struct attr *a;
	spa_list_for_each(a, &msrp->attributes, link)
		if (a->stream_id == stream_id)
			return a;
	return NULL;
}

static void attr_event(struct msrp *msrp, uint8_t type, int event)
{
	struct attr *a;
	spa_list_for_each(a, &msrp->attributes, link)
		if (a->attr.type == type)
			avbtp_mrp_update_state(msrp->server->mrp, &a->attr, event, 0);
}

static void process_talker(struct msrp *msrp, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_talker *t = m;
	char buf[128];
	struct attr *a;

	pw_log_info("talker");
	pw_log_info(" %s", avbtp_utils_format_id(buf, sizeof(buf), t->stream_id));

	a = find_attr_by_stream_id(msrp, be64toh(t->stream_id));
	if (a)
		avbtp_mrp_event(msrp->server->mrp, &a->attr, event, param);
}

static void process_talker_fail(struct msrp *msrp, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_talker_fail *t = m;
	char buf[128];
	pw_log_info("talker fail");
	pw_log_info(" %s", avbtp_utils_format_id(buf, sizeof(buf), t->talker.stream_id));
}

static void process_listener(struct msrp *msrp, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_listener *l = m;
	char buf[128];
	pw_log_info("listener");
	pw_log_info(" %s", avbtp_utils_format_id(buf, sizeof(buf), l->stream_id));
}

static void process_domain(struct msrp *msrp, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avbtp_packet_msrp_domain *d = m;
	pw_log_info("domain");
	pw_log_info(" %d", d->sr_class_id);
	pw_log_info(" %d", d->sr_class_priority);
	pw_log_info(" %d", ntohs(d->sr_class_vid));
}

static const struct {
	void (*dispatch) (struct msrp *msrp, uint8_t attr_type, const void *m, uint8_t event, uint8_t param, int num);
} dispatch[] = {
	[AVBTP_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE] = { process_talker, },
	[AVBTP_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED] = { process_talker_fail, },
	[AVBTP_MSRP_ATTRIBUTE_TYPE_LISTENER] = { process_listener, },
	[AVBTP_MSRP_ATTRIBUTE_TYPE_DOMAIN] = { process_domain, },
};

static inline bool has_params(uint16_t type)
{
	return type == AVBTP_MSRP_ATTRIBUTE_TYPE_LISTENER;
}

static int process(struct msrp *msrp, uint64_t now, const void *message, int len)
{
	uint8_t *e = SPA_PTROFF(message, len, uint8_t);
	uint8_t *m = SPA_PTROFF(message, sizeof(struct avbtp_packet_mrp), uint8_t);

	while (m < e && (m[0] != 0 || m[1] != 0)) {
		const struct avbtp_packet_msrp_msg *msg = (const struct avbtp_packet_msrp_msg*)m;
		uint8_t attr_len = msg->attribute_length;
		uint8_t attr_type = msg->attribute_type;
		bool has_param = has_params(attr_type);

		if (!AVBTP_MSRP_ATTRIBUTE_TYPE_VALID(attr_type))
			return -EINVAL;

		m += sizeof(*msg);

		while (m < e && (m[0] != 0 || m[1] != 0)) {
			const struct avbtp_packet_mrp_vector *v =
				(const struct avbtp_packet_mrp_vector*)m;
			uint16_t i, num_values = AVBTP_MRP_VECTOR_GET_NUM_VALUES(v);
			uint8_t event_len = (num_values+2)/3;
			uint8_t param_len = has_param ? (num_values+3)/4 : 0;
			int len = sizeof(*v) + attr_len + event_len + param_len;
			const uint8_t *first = v->first_value;
			uint8_t event[3], param[4] = { 0, };

			if (m + len > e)
				return -EPROTO;

			if (v->lva)
				attr_event(msrp, attr_type, AVBTP_MRP_EVENT_RX_LVA);

			for (i = 0; i < num_values; i++) {
				if (i % 3 == 0) {
					uint8_t ep = first[attr_len + i/3];
					event[2] = ep % 6; ep /= 6;
					event[1] = ep % 6; ep /= 6;
					event[0] = ep % 6;
				}
				if (has_param && (i % 4 == 0)) {
					uint8_t ep = first[attr_len + event_len + i/4];
					param[3] = ep % 4; ep /= 4;
					param[2] = ep % 4; ep /= 4;
					param[1] = ep % 4; ep /= 4;
					param[0] = ep % 4;
				}
				dispatch[attr_type].dispatch(msrp,
						attr_type, first, event[i%3], param[i%4], i);
			}
			m += len;
		}
	}
	return 0;
}


static int msrp_message(void *data, uint64_t now, const void *message, int len)
{
	struct msrp *msrp = data;
	const struct avbtp_packet_mrp *p = message;

	if (ntohs(p->eth.type) != AVB_MSRP_ETH)
		return 0;
	if (memcmp(p->eth.dest, mac, 6) != 0)
		return 0;

	pw_log_info("MSRP");
	return process(msrp, now, message, len);
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

int avbtp_msrp_register(struct server *server)
{
	struct msrp *msrp;

	msrp = calloc(1, sizeof(*msrp));
	if (msrp == NULL)
		return -errno;

	msrp->server = server;
	spa_list_init(&msrp->attributes);

	avdecc_server_add_listener(server, &msrp->server_listener, &server_events, msrp);

	return 0;
}
