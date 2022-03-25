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

#include "mrp.h"

#define MRP_JOINTIMER_MS	100
#define MRP_LVTIMER_MS		1000
#define MRP_LVATIMER_MS		10000
#define MRP_PERIODTIMER_MS	1000

#define mrp_emit(s,m,v,...)		spa_hook_list_call(&s->listener_list, struct avb_mrp_events, m, v, ##__VA_ARGS__)
#define mrp_emit_event(s,n,e)		mrp_emit(s,event,0,n,e)
#define mrp_emit_notify(s,n,a,e)	mrp_emit(s,notify,0,n,a,e)

struct attribute {
	struct avb_mrp_attribute attr;
	struct spa_list link;
	uint8_t applicant_state;
	uint8_t registrar_state;
	uint64_t leave_timeout;
};

struct mrp {
	struct server *server;
	struct spa_hook server_listener;

	struct spa_hook_list listener_list;

	struct spa_list attributes;

	uint64_t periodic_timeout;
	uint64_t leave_all_timeout;
	uint64_t join_timeout;
};

static void mrp_destroy(void *data)
{
	struct mrp *mrp = data;
	spa_hook_remove(&mrp->server_listener);
	free(mrp);
}

static void global_event(struct mrp *mrp, uint64_t now, uint8_t event)
{
	struct attribute *a;
	spa_list_for_each(a, &mrp->attributes, link)
		avb_mrp_update_state((struct avb_mrp*)mrp, now, &a->attr, event);
	mrp_emit_event(mrp, now, event);
}

static void mrp_periodic(void *data, uint64_t now)
{
	struct mrp *mrp = data;
	bool leave_all = false;
	struct attribute *a;

	if (now > mrp->periodic_timeout) {
		if (mrp->periodic_timeout > 0)
			global_event(mrp, now, AVB_MRP_EVENT_PERIODIC);
		mrp->periodic_timeout = now + MRP_PERIODTIMER_MS * SPA_NSEC_PER_MSEC;
	}
	if (now > mrp->leave_all_timeout) {
		if (mrp->leave_all_timeout > 0) {
			global_event(mrp, now, AVB_MRP_EVENT_RX_LVA);
			leave_all = true;
		}
		mrp->leave_all_timeout = now + (MRP_LVATIMER_MS + (random() % (MRP_LVATIMER_MS / 2)))
			* SPA_NSEC_PER_MSEC;
	}

	if (now > mrp->join_timeout) {
		if (mrp->join_timeout > 0) {
			uint8_t event = leave_all ? AVB_MRP_EVENT_TX_LVA : AVB_MRP_EVENT_TX;
			global_event(mrp, now, event);
		}
		mrp->join_timeout = now + MRP_JOINTIMER_MS * SPA_NSEC_PER_MSEC;
	}

	spa_list_for_each(a, &mrp->attributes, link) {
		if (a->leave_timeout > 0 && now > a->leave_timeout) {
			a->leave_timeout = 0;
			avb_mrp_update_state((struct avb_mrp*)mrp, now, &a->attr, AVB_MRP_EVENT_LV_TIMER);
		}
		if (a->attr.pending_notify) {
			mrp_emit_notify(mrp, now, &a->attr, a->attr.pending_notify);
			a->attr.pending_notify = 0;
		}
	}
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = mrp_destroy,
	.periodic = mrp_periodic,
};

int avb_mrp_parse_packet(struct avb_mrp *mrp, uint64_t now, const void *pkt, int len,
		const struct avb_mrp_parse_info *info, void *data)
{
	uint8_t *e = SPA_PTROFF(pkt, len, uint8_t);
	uint8_t *m = SPA_PTROFF(pkt, sizeof(struct avb_packet_mrp), uint8_t);

	while (m < e && (m[0] != 0 || m[1] != 0)) {
		const struct avb_packet_mrp_hdr *hdr = (const struct avb_packet_mrp_hdr*)m;
		uint8_t attr_type = hdr->attribute_type;
		uint8_t attr_len = hdr->attribute_length;
		size_t hdr_size;
		bool has_param;

		if (!info->check_header(data, hdr, &hdr_size, &has_param))
			return -EINVAL;

		m += hdr_size;

		while (m < e && (m[0] != 0 || m[1] != 0)) {
			const struct avb_packet_mrp_vector *v =
				(const struct avb_packet_mrp_vector*)m;
			uint16_t i, num_values = AVB_MRP_VECTOR_GET_NUM_VALUES(v);
			uint8_t event_len = (num_values+2)/3;
			uint8_t param_len = has_param ? (num_values+3)/4 : 0;
			int plen = sizeof(*v) + attr_len + event_len + param_len;
			const uint8_t *first = v->first_value;
			uint8_t event[3], param[4] = { 0, };

			if (m + plen > e)
				return -EPROTO;

			if (v->lva)
				info->attr_event(data, now, attr_type, AVB_MRP_EVENT_RX_LVA);

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
				info->process(data, now, attr_type, first,
						event[i%3], param[i%4], i);
			}
			m += plen;
		}
		m += 2;
	}
	return 0;
}

struct avb_mrp_attribute *avb_mrp_attribute_new(struct avb_mrp *m,
		size_t user_size)
{
	struct mrp *mrp = (struct mrp*)m;
	struct attribute *a;

	a = calloc(1, sizeof(*a) + user_size);
	if (a == NULL)
		return NULL;

	a->attr.user_data = SPA_PTROFF(a, sizeof(*a), void);
	spa_list_append(&mrp->attributes, &a->link);

	return &a->attr;
}

static uint8_t get_pending_send(struct avb_mrp *mrp, struct attribute *a, bool leave_all)
{
	uint8_t send = 0;

	switch (a->applicant_state) {
	case AVB_MRP_VP:
	case AVB_MRP_AA:
	case AVB_MRP_AP:
	case AVB_MRP_QA:
	case AVB_MRP_QP:
		if (leave_all && a->applicant_state == AVB_MRP_VP) {
			switch (a->registrar_state) {
			case AVB_MRP_IN:
				send = AVB_MRP_SEND_IN;
				break;
			default:
				send = AVB_MRP_SEND_MT;
				break;
			}
		} else if (leave_all || a->applicant_state != AVB_MRP_QP) {
			switch (a->registrar_state) {
			case AVB_MRP_IN:
				send = AVB_MRP_SEND_JOININ;
				break;
			default:
				send = AVB_MRP_SEND_JOINMT;
				break;
			}
		}
		break;
	case AVB_MRP_VN:
	case AVB_MRP_AN:
		send = AVB_MRP_SEND_NEW;
		break;
	case AVB_MRP_LA:
		send = AVB_MRP_SEND_LV;
		break;
	case AVB_MRP_LO:
		switch (a->registrar_state) {
		case AVB_MRP_IN:
			send = AVB_MRP_SEND_IN;
			break;
		default:
			send = AVB_MRP_SEND_MT;
			break;
		}
		break;
	}
	return send;
}

void avb_mrp_update_state(struct avb_mrp *mrp, uint64_t now,
		struct avb_mrp_attribute *attr, int event)
{
	struct attribute *a = SPA_CONTAINER_OF(attr, struct attribute, attr);
	uint8_t notify = 0, state;
	uint8_t send = 0;

	state = a->registrar_state;

	switch (event) {
	case AVB_MRP_EVENT_BEGIN:
		state = AVB_MRP_MT;
		break;
	case AVB_MRP_EVENT_RX_NEW:
		notify = AVB_MRP_NOTIFY_JOIN_NEW;
		switch (state) {
		case AVB_MRP_LV:
			a->leave_timeout = 0;
			SPA_FALLTHROUGH;
		case AVB_MRP_MT:
		case AVB_MRP_IN:
			state = AVB_MRP_IN;
                        break;
		}
		break;
	case AVB_MRP_EVENT_RX_JOININ:
	case AVB_MRP_EVENT_RX_JOINMT:
		switch (state) {
		case AVB_MRP_LV:
			a->leave_timeout = 0;
			SPA_FALLTHROUGH;
		case AVB_MRP_MT:
			notify = AVB_MRP_NOTIFY_JOIN;
			SPA_FALLTHROUGH;
		case AVB_MRP_IN:
			state = AVB_MRP_IN;
                        break;
		}
		break;
	case AVB_MRP_EVENT_RX_LV:
		notify = AVB_MRP_NOTIFY_LEAVE;
		SPA_FALLTHROUGH;
	case AVB_MRP_EVENT_RX_LVA:
	case AVB_MRP_EVENT_TX_LVA:
	case AVB_MRP_EVENT_REDECLARE:
		switch (state) {
		case AVB_MRP_IN:
			a->leave_timeout = now + MRP_LVTIMER_MS * SPA_NSEC_PER_MSEC;
			state = AVB_MRP_LV;
			break;
		}
		break;
	case AVB_MRP_EVENT_LV_TIMER:
		switch (state) {
		case AVB_MRP_LV:
			notify = AVB_MRP_NOTIFY_LEAVE;
			break;
		}
		break;
	case AVB_MRP_EVENT_FLUSH:
		notify = AVB_MRP_NOTIFY_LEAVE;
		switch (state) {
		case AVB_MRP_LV:
		case AVB_MRP_MT:
		case AVB_MRP_IN:
			state = AVB_MRP_MT;
			break;
		}
		break;
	default:
		break;
	}
	a->attr.pending_notify |= notify;
	if (a->registrar_state != state || notify) {
		pw_log_info("attr %p: %d %d -> %d %d", a, event, a->registrar_state, state, notify);
		a->registrar_state = state;
	}

	state = a->applicant_state;

	switch (event) {
	case AVB_MRP_EVENT_BEGIN:
		state = AVB_MRP_VO;
		break;
	case AVB_MRP_EVENT_NEW:
		switch (state) {
		case AVB_MRP_VN:
		case AVB_MRP_AN:
			break;
		default:
			state = AVB_MRP_VN;
			break;
		}
		break;
	case AVB_MRP_EVENT_JOIN:
		switch (state) {
		case AVB_MRP_VO:
		case AVB_MRP_LO:
			state = AVB_MRP_VP;
			break;
		case AVB_MRP_LA:
			state = AVB_MRP_AA;
			break;
		case AVB_MRP_AO:
			state = AVB_MRP_AP;
			break;
		case AVB_MRP_QO:
			state = AVB_MRP_QP;
			break;
		}
		break;
	case AVB_MRP_EVENT_LV:
		switch (state) {
		case AVB_MRP_QP:
			state = AVB_MRP_QO;
			break;
		case AVB_MRP_AP:
			state = AVB_MRP_AO;
			break;
		case AVB_MRP_VP:
			state = AVB_MRP_VO;
			break;
		case AVB_MRP_VN:
		case AVB_MRP_AN:
		case AVB_MRP_AA:
		case AVB_MRP_QA:
			state = AVB_MRP_LA;
			break;
		}
		break;
	case AVB_MRP_EVENT_RX_JOININ:
		switch (state) {
		case AVB_MRP_VO:
			state = AVB_MRP_AO;
			break;
		case AVB_MRP_VP:
			state = AVB_MRP_AP;
			break;
		case AVB_MRP_AA:
			state = AVB_MRP_QA;
			break;
		case AVB_MRP_AO:
			state = AVB_MRP_QO;
			break;
		case AVB_MRP_AP:
			state = AVB_MRP_QP;
			break;
		}
		SPA_FALLTHROUGH;
	case AVB_MRP_EVENT_RX_IN:
		switch (state) {
		case AVB_MRP_AA:
			state = AVB_MRP_QA;
			break;
		}
		SPA_FALLTHROUGH;
	case AVB_MRP_EVENT_RX_JOINMT:
	case AVB_MRP_EVENT_RX_MT:
		switch (state) {
		case AVB_MRP_QA:
			state = AVB_MRP_AA;
			break;
		case AVB_MRP_QO:
			state = AVB_MRP_AO;
			break;
		case AVB_MRP_QP:
			state = AVB_MRP_AP;
			break;
		case AVB_MRP_LO:
			state = AVB_MRP_VO;
			break;
		}
		break;
	case AVB_MRP_EVENT_RX_LV:
	case AVB_MRP_EVENT_RX_LVA:
	case AVB_MRP_EVENT_REDECLARE:
		switch (state) {
		case AVB_MRP_VO:
		case AVB_MRP_AO:
		case AVB_MRP_QO:
			state = AVB_MRP_LO;
			break;
		case AVB_MRP_AN:
			state = AVB_MRP_VN;
			break;
		case AVB_MRP_AA:
		case AVB_MRP_QA:
		case AVB_MRP_AP:
		case AVB_MRP_QP:
			state = AVB_MRP_VP;
			break;
		}
		break;
	case AVB_MRP_EVENT_PERIODIC:
		switch (state) {
		case AVB_MRP_QA:
			state = AVB_MRP_AA;
			break;
		case AVB_MRP_QP:
			state = AVB_MRP_AP;
			break;
		}
		break;
	case AVB_MRP_EVENT_TX:
		switch (state) {
		case AVB_MRP_VP:
		case AVB_MRP_VN:
		case AVB_MRP_AN:
		case AVB_MRP_AA:
		case AVB_MRP_LA:
		case AVB_MRP_AP:
		case AVB_MRP_LO:
			send = get_pending_send(mrp, a, false);
		}
		switch (state) {
		case AVB_MRP_VP:
			state = AVB_MRP_AA;
			break;
		case AVB_MRP_VN:
			state = AVB_MRP_AN;
			break;
		case AVB_MRP_AN:
		case AVB_MRP_AA:
		case AVB_MRP_AP:
			state = AVB_MRP_QA;
			break;
		case AVB_MRP_LA:
		case AVB_MRP_LO:
			state = AVB_MRP_VO;
			break;
		}
		break;
	case AVB_MRP_EVENT_TX_LVA:
	{
		switch (state) {
		case AVB_MRP_VP:
		case AVB_MRP_VN:
		case AVB_MRP_AN:
		case AVB_MRP_AA:
		case AVB_MRP_LA:
		case AVB_MRP_QA:
		case AVB_MRP_AP:
		case AVB_MRP_QP:
			send = get_pending_send(mrp, a, true);
		}
		switch (state) {
		case AVB_MRP_VO:
		case AVB_MRP_LA:
		case AVB_MRP_AO:
		case AVB_MRP_QO:
			state = AVB_MRP_LO;
			break;
		case AVB_MRP_VN:
			state = AVB_MRP_AN;
			break;
		case AVB_MRP_AN:
		case AVB_MRP_AA:
		case AVB_MRP_AP:
		case AVB_MRP_QP:
			state = AVB_MRP_QA;
			break;
		}
		break;
	}
	default:
		break;
	}
	if (a->applicant_state != state || send) {
		pw_log_info("attr %p: %d %d -> %d %d", a, event, a->applicant_state, state, send);
		a->applicant_state = state;
	}
	a->attr.pending_send = send;
}

void avb_mrp_rx_event(struct avb_mrp *mrp, uint64_t now,
		struct avb_mrp_attribute *attr, uint8_t event)
{
	static const int map[] = {
		[AVB_MRP_ATTRIBUTE_EVENT_NEW] = AVB_MRP_EVENT_RX_NEW,
		[AVB_MRP_ATTRIBUTE_EVENT_JOININ] = AVB_MRP_EVENT_RX_JOININ,
		[AVB_MRP_ATTRIBUTE_EVENT_IN] = AVB_MRP_EVENT_RX_IN,
		[AVB_MRP_ATTRIBUTE_EVENT_JOINMT] = AVB_MRP_EVENT_RX_JOINMT,
		[AVB_MRP_ATTRIBUTE_EVENT_MT] = AVB_MRP_EVENT_RX_MT,
		[AVB_MRP_ATTRIBUTE_EVENT_LV] = AVB_MRP_EVENT_RX_LV,
	};
	avb_mrp_update_state(mrp, now, attr, map[event]);
}

void avb_mrp_mad_begin(struct avb_mrp *mrp, uint64_t now, struct avb_mrp_attribute *attr)
{
	struct attribute *a = SPA_CONTAINER_OF(attr, struct attribute, attr);
	a->leave_timeout = 0;
	avb_mrp_update_state(mrp, now, attr, AVB_MRP_EVENT_BEGIN);
}

void avb_mrp_mad_join(struct avb_mrp *mrp, uint64_t now, struct avb_mrp_attribute *attr, bool is_new)
{
	if (is_new)
		avb_mrp_update_state(mrp, now, attr, AVB_MRP_EVENT_NEW);
	else
		avb_mrp_update_state(mrp, now, attr, AVB_MRP_EVENT_JOIN);
}

void avb_mrp_mad_leave(struct avb_mrp *mrp, uint64_t now, struct avb_mrp_attribute *attr)
{
	avb_mrp_update_state(mrp, now, attr, AVB_MRP_EVENT_LV);
}

void avb_mrp_destroy(struct avb_mrp *mrp)
{
	mrp_destroy(mrp);
}

struct avb_mrp *avb_mrp_new(struct server *server)
{
	struct mrp *mrp;

	mrp = calloc(1, sizeof(*mrp));
	if (mrp == NULL)
		return NULL;

	mrp->server = server;
	spa_list_init(&mrp->attributes);
	spa_hook_list_init(&mrp->listener_list);

	avdecc_server_add_listener(server, &mrp->server_listener, &server_events, mrp);

	return (struct avb_mrp*)mrp;
}

void avb_mrp_add_listener(struct avb_mrp *m, struct spa_hook *listener,
		const struct avb_mrp_events *events, void *data)
{
	struct mrp *mrp = (struct mrp*)m;
	spa_hook_list_append(&mrp->listener_list, listener, events, data);
}
