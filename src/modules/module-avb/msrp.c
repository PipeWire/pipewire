/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>

#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>

#include "utils.h"
#include "msrp.h"
#include "acmp.h"
#include "stream.h"
#include "aecp-aem.h"
#include "aecp-aem-state.h"

static const uint8_t msrp_mac[6] = AVB_MSRP_MAC;

struct attr {
	struct avb_msrp_attribute *attr;
	struct msrp *msrp;
	struct spa_hook listener;
	struct spa_list link;
};

struct msrp {
	struct server *server;
	struct spa_hook server_listener;
	struct spa_hook mrp_listener;

	struct spa_source *source;

	struct spa_list attributes;
};

static void debug_msrp_talker_common(const struct avb_packet_msrp_talker *t)
{
	char buf[128];
	pw_log_info(" stream-id: %s", avb_utils_format_id(buf, sizeof(buf), be64toh(t->stream_id)));
	pw_log_info(" dest-addr: %s", avb_utils_format_addr(buf, sizeof(buf), t->dest_addr));
	pw_log_info(" vlan-id:   %d", ntohs(t->vlan_id));
	pw_log_info(" tspec-max-frame-size: %d", ntohs(t->tspec_max_frame_size));
	pw_log_info(" tspec-max-interval-frames: %d", ntohs(t->tspec_max_interval_frames));
	pw_log_info(" priority: %d", t->priority);
	pw_log_info(" rank: %d", t->rank);
	pw_log_info(" accumulated-latency: %d", ntohl(t->accumulated_latency));
}

static void debug_msrp_talker(const struct avb_packet_msrp_talker *t)
{
	pw_log_info("talker");
	debug_msrp_talker_common(t);
}

/* IEEE 802.1Q Section 35.2.2.4.4: Listener may declare Ready only once the matching
 * Talker Advertise is registered; otherwise it stays in AskingFailed. */
static void notify_talker(struct msrp *msrp, uint64_t now, struct attr *attr, uint8_t notify)
{
	struct stream_common *sc;
	char label[64];

	pw_log_info("> notify talker advertise: %s", avb_mrp_notify_name(notify));
	snprintf(label, sizeof(label), "MSRP talker-adv %s", avb_mrp_notify_name(notify));
	avb_log_state(msrp->server, label);
	if (msrp->server->avb_mode == AVB_MODE_MILAN_V12) {
		uint64_t stream_id = be64toh(attr->attr->attr.talker.stream_id);
		if (notify == AVB_MRP_NOTIFY_NEW || notify == AVB_MRP_NOTIFY_JOIN)
			handle_evt_tk_discovered(msrp->server->acmp, stream_id, now);
		else if (notify == AVB_MRP_NOTIFY_LEAVE)
			handle_evt_tk_departed(msrp->server->acmp, stream_id, now);
	}

	sc = SPA_CONTAINER_OF(attr->attr, struct stream_common, tastream_attr);
	if (sc->stream.direction == SPA_DIRECTION_INPUT) {
		if (notify == AVB_MRP_NOTIFY_NEW || notify == AVB_MRP_NOTIFY_JOIN)
			sc->lstream_attr.param = AVB_MSRP_LISTENER_PARAM_READY;
		else if (notify == AVB_MRP_NOTIFY_LEAVE)
			sc->lstream_attr.param = AVB_MSRP_LISTENER_PARAM_ASKING_FAILED;
		/* Milan Table 5.10: TA registrar state flips flags_ex.REGISTERING
		 * in the listener-side GET_STREAM_INFO answer — emit an unsol. */
		avb_aecp_aem_mark_stream_info_dirty(msrp->server,
				AVB_AEM_DESC_STREAM_INPUT, sc->stream.index);
	}
}

static void notify_talker_failed(struct msrp *msrp, uint64_t now, struct attr *attr, uint8_t notify)
{
	struct stream_common *sc;
	char label[64];

	pw_log_info("> notify talker failed: %s", avb_mrp_notify_name(notify));
	snprintf(label, sizeof(label), "MSRP talker-fail %s", avb_mrp_notify_name(notify));
	avb_log_state(msrp->server, label);

	if (msrp->server->avb_mode == AVB_MODE_MILAN_V12) {
		if (notify == AVB_MRP_NOTIFY_NEW || notify == AVB_MRP_NOTIFY_JOIN)
			handle_evt_tk_registration_failed(msrp->server->acmp, attr->attr, now);
	}

	/* Milan Table 5.10: TF registrar state also flips flags_ex.REGISTERING
	 * on the listener side; emit an unsol when it changes. */
	sc = SPA_CONTAINER_OF(attr->attr, struct stream_common, tfstream_attr);
	if (sc->stream.direction == SPA_DIRECTION_INPUT)
		avb_aecp_aem_mark_stream_info_dirty(msrp->server,
				AVB_AEM_DESC_STREAM_INPUT, sc->stream.index);
}

static int process_talker(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avb_packet_msrp_talker *t = m;
	struct attr *a;
	spa_list_for_each(a, &msrp->attributes, link) {
		if (a->attr->type == attr_type &&
		    a->attr->attr.talker.stream_id == t->stream_id) {
			a->attr->attr.talker = *t;
			avb_mrp_attribute_rx_event(a->attr->mrp, now, event);
		}
	}

	return 0;
}
static int encode_talker(struct msrp *msrp, struct attr *a, void *m)
{
	struct avb_packet_msrp_msg *msg = m;
	struct avb_packet_mrp_vector *v;
	struct avb_packet_msrp_talker *t;
	struct avb_packet_mrp_footer *f;
	uint8_t *ev;
	size_t attr_list_length = sizeof(*v) + sizeof(*t) + sizeof(*f) + 1;

	msg->attribute_type = AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE;
	msg->attribute_length = sizeof(*t);
	msg->attribute_list_length = htons(attr_list_length);

	v = (struct avb_packet_mrp_vector *)msg->attribute_list;
	v->lva = avb_mrp_lva_tx_pending(msrp->server->mrp) ? 1 : 0;
	AVB_MRP_VECTOR_SET_NUM_VALUES(v, 1);

	t = (struct avb_packet_msrp_talker *)v->first_value;
	*t = a->attr->attr.talker;

	ev = SPA_PTROFF(t, sizeof(*t), uint8_t);
	*ev = (a->attr->mrp->pending_send - 1) * 6 * 6;

	f = SPA_PTROFF(ev, sizeof(*ev), struct avb_packet_mrp_footer);
	f->end_mark = 0;

	return attr_list_length + sizeof(*msg);
}


static void debug_msrp_talker_fail(const struct avb_packet_msrp_talker_fail *t)
{
	char buf[128];
	pw_log_info("talker fail");
	debug_msrp_talker_common(&t->talker);
	pw_log_info(" bridge-id: %s", avb_utils_format_id(buf, sizeof(buf), be64toh(t->bridge_id)));
	pw_log_info(" failure-code: %d", t->failure_code);
}

static int process_talker_fail(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avb_packet_msrp_talker_fail *t = m;
	struct attr *a;

	debug_msrp_talker_fail(t);

	spa_list_for_each(a, &msrp->attributes, link)
		if (a->attr->type == attr_type &&
		    a->attr->attr.talker_fail.talker.stream_id == t->talker.stream_id)
			avb_mrp_attribute_rx_event(a->attr->mrp, now, event);
	return 0;
}

static void debug_msrp_listener(const struct avb_packet_msrp_listener *l, uint8_t param)
{
	char buf[128];
	pw_log_info("listener");
	pw_log_info(" %s", avb_utils_format_id(buf, sizeof(buf), be64toh(l->stream_id)));
	pw_log_info(" %d", param);
}

static void notify_listener(struct msrp *msrp, uint64_t now, struct attr *attr, uint8_t notify)
{
	struct stream_common *sc;
	struct stream *s;
	char label[64];

	pw_log_info("> notify listener: %s", avb_mrp_notify_name(notify));
	debug_msrp_listener(&attr->attr->attr.listener, attr->attr->param);
	snprintf(label, sizeof(label), "MSRP listener %s", avb_mrp_notify_name(notify));
	avb_log_state(msrp->server, label);

	if (msrp->server->avb_mode != AVB_MODE_MILAN_V12)
		return;

	 /* Milan Section 4.3.3.1 second condition: a Listener attribute
	 * matching the Stream Output's stream_id is registered. */
	sc = SPA_CONTAINER_OF(attr->attr, struct stream_common, lstream_attr);
	s = &sc->stream;

	if (s->direction == SPA_DIRECTION_INPUT) {
		if (notify == AVB_MRP_NOTIFY_NEW || notify == AVB_MRP_NOTIFY_JOIN)
			handle_evt_tk_registered(msrp->server->acmp, attr->attr, now);
		else if (notify == AVB_MRP_NOTIFY_LEAVE)
			handle_evt_tk_unregistered(msrp->server->acmp, attr->attr, now);
	} else {
		struct aecp_aem_stream_output_state *stream_out =
			SPA_CONTAINER_OF(sc, struct aecp_aem_stream_output_state, common);
		bool prev = stream_out->listener_observed;

		if (notify == AVB_MRP_NOTIFY_NEW || notify == AVB_MRP_NOTIFY_JOIN)
			stream_out->listener_observed = true;
		else if (notify == AVB_MRP_NOTIFY_LEAVE)
			stream_out->listener_observed = false;

		/* listener_observed flips flags_ex.REGISTERING in the
		 * GET_STREAM_INFO answer (Milan Table 5.12). Hint AECP to
		 * re-emit so controllers see the update. */
		if (prev != stream_out->listener_observed)
			avb_aecp_aem_mark_stream_info_dirty(msrp->server,
					AVB_AEM_DESC_STREAM_OUTPUT, s->index);
	}
}

/* IEEE 802.1Q Section 35.2.2.4.4: capture the rx 4-pack Listener Declaration Type. */
static int process_listener(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	const struct avb_packet_msrp_listener *l = m;
	struct attr *a;
	spa_list_for_each(a, &msrp->attributes, link)
		if (a->attr->type == attr_type &&
		    a->attr->attr.listener.stream_id == l->stream_id) {
			a->attr->attr.listener = *l;
			a->attr->param = param;
			avb_mrp_attribute_rx_event(a->attr->mrp, now, event);
		}
	return 0;
}
static int encode_listener(struct msrp *msrp, struct attr *a, void *m)
{
	struct avb_packet_msrp_msg *msg = m;
	struct avb_packet_mrp_vector *v;
	struct avb_packet_msrp_listener *l;
	struct avb_packet_mrp_footer *f;
	uint8_t *ev;
	size_t attr_list_length = sizeof(*v) + sizeof(*l) + sizeof(*f) + 1 + 1;

	msg->attribute_type = AVB_MSRP_ATTRIBUTE_TYPE_LISTENER;
	msg->attribute_length = sizeof(*l);
	msg->attribute_list_length = htons(attr_list_length);

	v = (struct avb_packet_mrp_vector *)msg->attribute_list;
	v->lva = avb_mrp_lva_tx_pending(msrp->server->mrp) ? 1 : 0;
	AVB_MRP_VECTOR_SET_NUM_VALUES(v, 1);

	l = (struct avb_packet_msrp_listener *)v->first_value;
	*l = a->attr->attr.listener;

	ev = SPA_PTROFF(l, sizeof(*l), uint8_t);
	*ev = (a->attr->mrp->pending_send - 1) * 6 * 6;

	ev = SPA_PTROFF(ev, sizeof(*ev), uint8_t);
	*ev = a->attr->param * 4 * 4 * 4;

	f = SPA_PTROFF(ev, sizeof(*ev), struct avb_packet_mrp_footer);
	f->end_mark = 0;

	return attr_list_length + sizeof(*msg);
}

static void debug_msrp_domain(const struct avb_packet_msrp_domain *d)
{
	pw_log_info("domain");
	pw_log_info(" id: %d", d->sr_class_id);
	pw_log_info(" prio: %d", d->sr_class_priority);
	pw_log_info(" vid: %d", ntohs(d->sr_class_vid));
}

static void notify_domain(struct msrp *msrp, uint64_t now, struct attr *attr, uint8_t notify)
{
	pw_log_info("> notify domain: %s", avb_mrp_notify_name(notify));
	debug_msrp_domain(&attr->attr->attr.domain);
}

static int process_domain(struct msrp *msrp, uint64_t now, uint8_t attr_type,
		const void *m, uint8_t event, uint8_t param, int num)
{
	struct attr *a;
	const struct avb_packet_msrp_domain *d = m;

	spa_list_for_each(a, &msrp->attributes, link) {
		if (a->attr->type != attr_type) {
			continue;
		}

		if (msrp->server->avb_mode == AVB_MODE_MILAN_V12)
		{
			/** Milan V1.2 Section 4.2.7.2.1:
			    The endstation shall re-adjust the domain according
			    to the from the MSRPDU received on its interface */
			bool mismatch = (a->attr->attr.domain.sr_class_id != d->sr_class_id
				|| a->attr->attr.domain.sr_class_priority != d->sr_class_priority
				|| a->attr->attr.domain.sr_class_vid  != d->sr_class_vid);

			if (mismatch) {
				pw_log_info("Domain mismatch re-adjusting");
				a->attr->attr.domain = *d;
				avb_mrp_attribute_leave(a->attr->mrp, now);
				avb_mrp_attribute_begin(a->attr->mrp, now);
				avb_mrp_attribute_join(a->attr->mrp, now, true);
			}
		}

		avb_mrp_attribute_rx_event(a->attr->mrp, now, event);
	}

	return 0;
}

static int encode_domain(struct msrp *msrp, struct attr *a, void *m)
{
	struct avb_packet_msrp_msg *msg = m;
	struct avb_packet_mrp_vector *v;
	struct avb_packet_msrp_domain *d;
	struct avb_packet_mrp_footer *f;
	uint8_t *ev;
	size_t attr_list_length = sizeof(*v) + sizeof(*d) + sizeof(*f) + 1;

	msg->attribute_type = AVB_MSRP_ATTRIBUTE_TYPE_DOMAIN;
	msg->attribute_length = sizeof(*d);
	msg->attribute_list_length = htons(attr_list_length);

	v = (struct avb_packet_mrp_vector *)msg->attribute_list;
	v->lva = avb_mrp_lva_tx_pending(msrp->server->mrp) ? 1 : 0;
	AVB_MRP_VECTOR_SET_NUM_VALUES(v, 1);

	d = (struct avb_packet_msrp_domain *)v->first_value;
	*d = a->attr->attr.domain;

	ev = SPA_PTROFF(d, sizeof(*d), uint8_t);
	*ev = (a->attr->mrp->pending_send - 1) * 36;

	f = SPA_PTROFF(ev, sizeof(*ev), struct avb_packet_mrp_footer);
	f->end_mark = 0;

	return attr_list_length + sizeof(*msg);
}

static const struct {
	const char *name;
	int (*process) (struct msrp *msrp, uint64_t now, uint8_t attr_type,
			const void *m, uint8_t event, uint8_t param, int num);
	int (*encode) (struct msrp *msrp, struct attr *attr, void *m);
	void (*notify) (struct msrp *msrp, uint64_t now, struct attr *attr, uint8_t notify);
} dispatch[] = {
	[AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE] = { "talker", process_talker, encode_talker, notify_talker, },
	[AVB_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED] = { "talker-fail", process_talker_fail, NULL, notify_talker_failed, },
	[AVB_MSRP_ATTRIBUTE_TYPE_LISTENER] = { "listener", process_listener, encode_listener, notify_listener },
	[AVB_MSRP_ATTRIBUTE_TYPE_DOMAIN] = { "domain", process_domain, encode_domain, notify_domain, },
};

static bool msrp_check_header(void *data, const void *hdr, size_t *hdr_size, bool *has_params)
{
	const struct avb_packet_msrp_msg *msg = hdr;
	uint8_t attr_type = msg->attribute_type;

	if (!AVB_MSRP_ATTRIBUTE_TYPE_VALID(attr_type))
		return false;

	*hdr_size = sizeof(*msg);
	*has_params = attr_type == AVB_MSRP_ATTRIBUTE_TYPE_LISTENER;
	return true;
}

static int msrp_attr_event(void *data, uint64_t now, uint8_t attribute_type, uint8_t event)
{
	struct msrp *msrp = data;
	struct attr *a;
	spa_list_for_each(a, &msrp->attributes, link)
		if (a->attr->type == attribute_type)
			avb_mrp_attribute_update_state(a->attr->mrp, now, event);
	return 0;
}

static int msrp_process(void *data, uint64_t now, uint8_t attribute_type, const void *value,
			uint8_t event, uint8_t param, int index)
{
	struct msrp *msrp = data;
	return dispatch[attribute_type].process(msrp, now,
				attribute_type, value, event, param, index);
}

static const struct avb_mrp_parse_info info = {
	AVB_VERSION_MRP_PARSE_INFO,
	.check_header = msrp_check_header,
	.attr_event = msrp_attr_event,
	.process = msrp_process,
};


static int msrp_message(struct msrp *msrp, uint64_t now, const void *message, int len)
{
	return avb_mrp_parse_packet(msrp->server->mrp,
			now, message, len, &info, msrp);
}
static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct msrp *msrp = data;
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
			msrp_message(msrp, SPA_TIMESPEC_TO_NSEC(&now), buffer, len);
		}
	}
}

static void msrp_destroy(void *data)
{
	struct msrp *msrp = data;
	spa_hook_remove(&msrp->server_listener);
	pw_loop_destroy_source(msrp->server->impl->loop, msrp->source);
	free(msrp);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = msrp_destroy,
};

static void msrp_notify(void *data, uint64_t now, uint8_t notify)
{
	struct attr *a = data;
	struct msrp *msrp = a->msrp;

	if (dispatch[a->attr->type].notify)
		dispatch[a->attr->type].notify(msrp, now, a, notify);
}

static const struct avb_mrp_attribute_events mrp_attr_events = {
	AVB_VERSION_MRP_ATTRIBUTE_EVENTS,
	.notify = msrp_notify,
};

int avb_msrp_attribute_new(struct avb_msrp *m, struct avb_msrp_attribute *msrp_attr,
		uint8_t type)
{
	struct msrp *msrp = (struct msrp*)m;
	struct avb_mrp_attribute *attr;
	struct attr *a;

	attr = avb_mrp_attribute_new(msrp->server->mrp, sizeof(struct attr));
	if (!attr) {
		pw_log_error("MSRP attribute allocation failed");
		return -1;
	}

	a = attr->user_data;
	a->msrp = msrp;
	a->attr = msrp_attr;
	a->attr->mrp = attr;
	a->attr->type = type;
	a->attr->mrp = attr;
	attr->name = "MSRP";
	spa_list_append(&msrp->attributes, &a->link);
	avb_mrp_attribute_add_listener(attr, &a->listener, &mrp_attr_events, a);

	return 0;
}

static void msrp_event(void *data, uint64_t now, uint8_t event)
{
	struct msrp *msrp = data;
	uint8_t buffer[2048];
	struct avb_packet_mrp *p = (struct avb_packet_mrp*)buffer;
	struct avb_packet_mrp_footer *f;
	void *msg = SPA_PTROFF(buffer, sizeof(*p), void);
	struct attr *a;
	int len, count = 0;
	size_t total = sizeof(*p) + 2;

	p->version = AVB_MRP_PROTOCOL_VERSION;

	spa_list_for_each(a, &msrp->attributes, link) {
		if (!a->attr->mrp->pending_send)
			continue;
		if (dispatch[a->attr->type].encode == NULL)
			continue;

		pw_log_info("MSRP encode %s %s",
				dispatch[a->attr->type].name,
				avb_mrp_send_name(a->attr->mrp->pending_send));

		len = dispatch[a->attr->type].encode(msrp, a, msg);
		if (len < 0)
			break;

		a->attr->mrp->pending_send = 0;

		count++;
		msg = SPA_PTROFF(msg, len, void);
		total += len;
	}
	f = (struct avb_packet_mrp_footer *)msg;
	f->end_mark = 0;

	if (count > 0) {
		pw_log_info("MSRP send: %d attribute(s), %zu bytes", count, total);
		avb_server_send_packet(msrp->server, msrp_mac, AVB_MSRP_ETH,
				buffer, total);
	}
}

static const struct avb_mrp_events mrp_events = {
	AVB_VERSION_MRP_EVENTS,
	.event = msrp_event,
};


static const char *msrp_attr_type_name(uint8_t type)
{
	switch (type) {
	case AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE: return "talker-adv";
	case AVB_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED:    return "talker-fail";
	case AVB_MSRP_ATTRIBUTE_TYPE_LISTENER:         return "listener";
	case AVB_MSRP_ATTRIBUTE_TYPE_DOMAIN:           return "domain";
	default:                                       return "?";
	}
}

void avb_msrp_log_state(struct server *server, const char *label)
{
	struct msrp *msrp = (struct msrp *)server->msrp;
	struct attr *a;
	char buf[64];
	int n = 0;

	if (msrp == NULL)
		return;

	spa_list_for_each(a, &msrp->attributes, link)
		n++;
	pw_log_debug("[%s] MSRP: %d attribute%s", label, n, n == 1 ? "" : "s");

	spa_list_for_each(a, &msrp->attributes, link) {
		uint8_t app_st = avb_mrp_attribute_get_applicant_state(a->attr->mrp);
		uint8_t reg_st = avb_mrp_attribute_get_registrar_state(a->attr->mrp);
		switch (a->attr->type) {
		case AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE:
		case AVB_MSRP_ATTRIBUTE_TYPE_TALKER_FAILED:
			pw_log_debug("[%s]   %-11s stream_id=%s app=%s reg=%s",
					label, msrp_attr_type_name(a->attr->type),
					avb_utils_format_id(buf, sizeof(buf),
						be64toh(a->attr->attr.talker.stream_id)),
					avb_applicant_state_name(app_st),
					avb_registrar_state_name(reg_st));
			break;
		case AVB_MSRP_ATTRIBUTE_TYPE_LISTENER:
			pw_log_debug("[%s]   %-11s stream_id=%s param=%u app=%s reg=%s",
					label, msrp_attr_type_name(a->attr->type),
					avb_utils_format_id(buf, sizeof(buf),
						be64toh(a->attr->attr.listener.stream_id)),
					a->attr->param,
					avb_applicant_state_name(app_st),
					avb_registrar_state_name(reg_st));
			break;
		case AVB_MSRP_ATTRIBUTE_TYPE_DOMAIN:
			pw_log_debug("[%s]   %-11s class_id=%u prio=%u vid=%u app=%s reg=%s",
					label, msrp_attr_type_name(a->attr->type),
					a->attr->attr.domain.sr_class_id,
					a->attr->attr.domain.sr_class_priority,
					ntohs(a->attr->attr.domain.sr_class_vid),
					avb_applicant_state_name(app_st),
					avb_registrar_state_name(reg_st));
			break;
		default:
			pw_log_debug("[%s]   type=%u app=%s reg=%s", label, a->attr->type,
					avb_applicant_state_name(app_st),
					avb_registrar_state_name(reg_st));
			break;
		}
	}
}

struct avb_msrp *avb_msrp_register(struct server *server)
{
	struct msrp *msrp;
	int fd, res;

	fd = avb_server_make_socket(server, AVB_MSRP_ETH, msrp_mac);
	if (fd < 0) {
		errno = -fd;
		return NULL;
	}
	msrp = calloc(1, sizeof(*msrp));
	if (msrp == NULL) {
		res = -errno;
		goto error_close;
	}

	msrp->server = server;
	spa_list_init(&msrp->attributes);

	msrp->source = pw_loop_add_io(server->impl->loop, fd, SPA_IO_IN, true, on_socket_data, msrp);
	if (msrp->source == NULL) {
		res = -errno;
		pw_log_error("msrp %p: can't create msrp source: %m", msrp);
		goto error_no_source;
	}
	avdecc_server_add_listener(server, &msrp->server_listener, &server_events, msrp);
	avb_mrp_add_listener(server->mrp, &msrp->mrp_listener, &mrp_events, msrp);

	return (struct avb_msrp*)msrp;

error_no_source:
	free(msrp);
error_close:
	close(fd);
	errno = -res;
	return NULL;
}
