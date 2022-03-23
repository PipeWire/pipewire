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

struct attribute {
	struct avbtp_mrp_attribute attr;
	struct spa_list link;
	uint8_t applicant_state;
	uint8_t registrar_state;
	uint16_t pending_indications;
	struct spa_callbacks cb;
};

struct mrp {
	struct server *server;
	struct spa_hook server_listener;
};

static void mrp_destroy(void *data)
{
	struct mrp *mrp = data;
	spa_hook_remove(&mrp->server_listener);
	free(mrp);
}

static const struct server_events server_events = {
	AVBTP_VERSION_SERVER_EVENTS,
	.destroy = mrp_destroy,
};

struct avbtp_mrp_attribute *avbtp_mrp_attribute_new(struct avbtp_mrp *mrp,
		uint8_t type, uint8_t param,
		struct avbtp_mrp_attribute_methods *methods, void *data,
		size_t user_size)
{
	struct attribute *a;

	a = calloc(1, sizeof(*a) + user_size);
	if (a == NULL)
		return NULL;

	a->attr.type = type;
	a->attr.param = param;
	a->attr.user_data = SPA_PTROFF(a, sizeof(*a), void);
	a->cb = SPA_CALLBACKS_INIT(methods, data);

	return &a->attr;
}

static void stop_avb_timer(void)
{
}
static void start_avb_timer(void)
{
}

void avbtp_mrp_update_state(struct avbtp_mrp *mrp,
		struct avbtp_mrp_attribute *attr, int event, uint8_t param)
{
	struct attribute *a = SPA_CONTAINER_OF(attr, struct attribute, attr);
	switch (event) {
	case AVBTP_MRP_EVENT_BEGIN:
		a->registrar_state = AVBTP_MRP_MT;
		break;
	case AVBTP_MRP_EVENT_RX_NEW:
		if (a->registrar_state == AVBTP_MRP_LV)
			stop_avb_timer();
		a->registrar_state = AVBTP_MRP_IN;
		a->pending_indications |= AVBTP_PENDING_JOIN_NEW;
		a->attr.param = param;
		break;
	case AVBTP_MRP_EVENT_RX_JOININ:
	case AVBTP_MRP_EVENT_RX_JOINMT:
		if (a->registrar_state == AVBTP_MRP_LV)
			stop_avb_timer();
		if (a->registrar_state == AVBTP_MRP_MT) {
			a->pending_indications |= AVBTP_PENDING_JOIN;
			a->attr.param = param;
		}
		a->registrar_state = AVBTP_MRP_IN;
		break;
	case AVBTP_MRP_EVENT_RX_LV:
	case AVBTP_MRP_EVENT_RX_LVA:
	case AVBTP_MRP_EVENT_TX_LVA:
	case AVBTP_MRP_EVENT_REDECLARE:
		if (a->registrar_state == AVBTP_MRP_IN) {
			start_avb_timer();
			a->registrar_state = AVBTP_MRP_LV;
		}
		break;
	case AVBTP_MRP_EVENT_LV_TIMER:
	case AVBTP_MRP_EVENT_FLUSH:
		if (a->registrar_state == AVBTP_MRP_LV) {
			a->pending_indications |= AVBTP_PENDING_LEAVE;
			a->attr.param = param;
		}
		a->registrar_state = AVBTP_MRP_MT;
		break;
	default:
		break;
	}

	switch (event) {
	case AVBTP_MRP_EVENT_BEGIN:
		a->applicant_state = AVBTP_MRP_VO;
		break;
	case AVBTP_MRP_EVENT_NEW:
		a->applicant_state = AVBTP_MRP_VN;
		break;
	case AVBTP_MRP_EVENT_JOIN:
		switch (a->applicant_state) {
		case AVBTP_MRP_VO:
		case AVBTP_MRP_LO:
			a->applicant_state = AVBTP_MRP_VP;
			break;
		case AVBTP_MRP_LA:
			a->applicant_state = AVBTP_MRP_AA;
			break;
		case AVBTP_MRP_AO:
			a->applicant_state = AVBTP_MRP_AP;
			break;
		case AVBTP_MRP_QO:
			a->applicant_state = AVBTP_MRP_QP;
			break;
		}
		break;
	case AVBTP_MRP_EVENT_LV:
		switch (a->applicant_state) {
		case AVBTP_MRP_QP:
			a->applicant_state = AVBTP_MRP_QO;
			break;
		case AVBTP_MRP_AP:
			a->applicant_state = AVBTP_MRP_AO;
			break;
		case AVBTP_MRP_VP:
			a->applicant_state = AVBTP_MRP_VO;
			break;
		case AVBTP_MRP_VN:
		case AVBTP_MRP_AN:
		case AVBTP_MRP_AA:
		case AVBTP_MRP_QA:
			a->applicant_state = AVBTP_MRP_LA;
			break;
		}
		break;
	case AVBTP_MRP_EVENT_RX_JOININ:
		switch (a->applicant_state) {
		case AVBTP_MRP_VO:
			a->applicant_state = AVBTP_MRP_AO;
			break;
		case AVBTP_MRP_VP:
			a->applicant_state = AVBTP_MRP_AP;
			break;
		case AVBTP_MRP_AA:
			a->applicant_state = AVBTP_MRP_QA;
			break;
		case AVBTP_MRP_AO:
			a->applicant_state = AVBTP_MRP_QO;
			break;
		case AVBTP_MRP_AP:
			a->applicant_state = AVBTP_MRP_QP;
			break;
		}
		SPA_FALLTHROUGH;
	case AVBTP_MRP_EVENT_RX_IN:
		switch (a->applicant_state) {
		case AVBTP_MRP_AA:
			a->applicant_state = AVBTP_MRP_QA;
			break;
		}
		SPA_FALLTHROUGH;
	case AVBTP_MRP_EVENT_RX_JOINMT:
	case AVBTP_MRP_EVENT_RX_MT:
		switch (a->applicant_state) {
		case AVBTP_MRP_QA:
			a->applicant_state = AVBTP_MRP_AA;
			break;
		case AVBTP_MRP_QO:
			a->applicant_state = AVBTP_MRP_AO;
			break;
		case AVBTP_MRP_QP:
			a->applicant_state = AVBTP_MRP_AP;
			break;
		case AVBTP_MRP_LO:
			a->applicant_state = AVBTP_MRP_VO;
			break;
		}
		break;
	case AVBTP_MRP_EVENT_RX_LV:
	case AVBTP_MRP_EVENT_RX_LVA:
	case AVBTP_MRP_EVENT_REDECLARE:
		switch (a->applicant_state) {
		case AVBTP_MRP_VO:
		case AVBTP_MRP_AO:
		case AVBTP_MRP_QO:
			a->applicant_state = AVBTP_MRP_LO;
			break;
		case AVBTP_MRP_AN:
			a->applicant_state = AVBTP_MRP_VN;
			break;
		case AVBTP_MRP_AA:
		case AVBTP_MRP_QA:
		case AVBTP_MRP_AP:
		case AVBTP_MRP_QP:
			a->applicant_state = AVBTP_MRP_VP;
			break;
		}
		break;
	case AVBTP_MRP_EVENT_PERIODIC:
		switch (a->applicant_state) {
		case AVBTP_MRP_QA:
			a->applicant_state = AVBTP_MRP_AA;
			break;
		case AVBTP_MRP_QP:
			a->applicant_state = AVBTP_MRP_AP;
			break;
		}
		break;
	case AVBTP_MRP_EVENT_TX:
		switch (a->applicant_state) {
		case AVBTP_MRP_VP:
		case AVBTP_MRP_VN:
		case AVBTP_MRP_AN:
		case AVBTP_MRP_AA:
		case AVBTP_MRP_LA:
		case AVBTP_MRP_AP:
		case AVBTP_MRP_LO:
//			int vector = makeTxEvent(e, st, 0);
//			doTx(st, vector);
			break;
		}
		switch (a->applicant_state) {
		case AVBTP_MRP_VP:
			a->applicant_state = AVBTP_MRP_AA;
			break;
		case AVBTP_MRP_VN:
			a->applicant_state = AVBTP_MRP_AN;
			break;
		case AVBTP_MRP_AN:
		case AVBTP_MRP_AA:
		case AVBTP_MRP_AP:
			a->applicant_state = AVBTP_MRP_QA;
			break;
		case AVBTP_MRP_LA:
		case AVBTP_MRP_LO:
			a->applicant_state = AVBTP_MRP_VO;
			break;
		}
		break;
	case AVBTP_MRP_EVENT_TX_LVA:
	{
		switch (a->applicant_state) {
		case AVBTP_MRP_VP:
		case AVBTP_MRP_VN:
		case AVBTP_MRP_AN:
		case AVBTP_MRP_AA:
		case AVBTP_MRP_LA:
		case AVBTP_MRP_QA:
		case AVBTP_MRP_AP:
		case AVBTP_MRP_QP:
//			int vector = makeTxEvent(e, st, 1);
//			doTx(st, vector);
		}
		switch (a->applicant_state) {
		case AVBTP_MRP_VO:
		case AVBTP_MRP_LA:
		case AVBTP_MRP_AO:
		case AVBTP_MRP_QO:
			a->applicant_state = AVBTP_MRP_LO;
			break;
		case AVBTP_MRP_VN:
			a->applicant_state = AVBTP_MRP_AN;
			break;
		case AVBTP_MRP_AN:
		case AVBTP_MRP_AA:
		case AVBTP_MRP_AP:
		case AVBTP_MRP_QP:
			a->applicant_state = AVBTP_MRP_QA;
			break;
		}
		break;
	}
	default:
		break;
	}
}

void avbtp_mrp_event(struct avbtp_mrp *mrp, struct avbtp_mrp_attribute *attr,
		uint8_t event, uint8_t param)
{
	static const int map[] = {
		[AVBTP_MRP_ATTRIBUTE_EVENT_NEW] = AVBTP_MRP_EVENT_RX_NEW,
		[AVBTP_MRP_ATTRIBUTE_EVENT_JOININ] = AVBTP_MRP_EVENT_RX_JOININ,
		[AVBTP_MRP_ATTRIBUTE_EVENT_IN] = AVBTP_MRP_EVENT_RX_IN,
		[AVBTP_MRP_ATTRIBUTE_EVENT_JOINMT] = AVBTP_MRP_EVENT_RX_JOINMT,
		[AVBTP_MRP_ATTRIBUTE_EVENT_MT] = AVBTP_MRP_EVENT_RX_MT,
		[AVBTP_MRP_ATTRIBUTE_EVENT_LV] = AVBTP_MRP_EVENT_RX_LV,
	};
	avbtp_mrp_update_state(mrp, attr, map[event], param);
}

void avbtp_mrp_mad_begin(struct avbtp_mrp *mrp, struct avbtp_mrp_attribute *attr)
{
}

void avbtp_mrp_mad_join(struct avbtp_mrp *mrp, struct avbtp_mrp_attribute *attr, bool is_new)
{
}

void avbtp_mrp_mad_leave(struct avbtp_mrp *mrp, struct avbtp_mrp_attribute *attr)
{
}

void avbtp_mrp_destroy(struct avbtp_mrp *mrp)
{
	mrp_destroy(mrp);
}

struct avbtp_mrp *avbtp_mrp_new(struct server *server)
{
	struct mrp *mrp;

	mrp = calloc(1, sizeof(*mrp));
	if (mrp == NULL)
		return NULL;

	mrp->server = server;

	avdecc_server_add_listener(server, &mrp->server_listener, &server_events, mrp);

	return (struct avbtp_mrp*)mrp;
}
