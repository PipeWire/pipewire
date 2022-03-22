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

void avbtp_mrp_attribute_init(struct avbtp_mrp *mrp,
		struct avbtp_mrp_attribute *attr,
		uint8_t type, void *info)
{
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
	switch (event) {
	case AVBTP_MRP_EVENT_BEGIN:
		attr->registrar_state = AVBTP_MRP_MT;
		break;
	case AVBTP_MRP_EVENT_RX_NEW:
		if (attr->registrar_state == AVBTP_MRP_LV)
			stop_avb_timer();
		attr->registrar_state = AVBTP_MRP_IN;
		attr->pending_indications |= AVBTP_PENDING_JOIN_NEW;
		attr->param = param;
		break;
	case AVBTP_MRP_EVENT_RX_JOININ:
	case AVBTP_MRP_EVENT_RX_JOINMT:
		if (attr->registrar_state == AVBTP_MRP_LV)
			stop_avb_timer();
		if (attr->registrar_state == AVBTP_MRP_MT) {
			attr->pending_indications |= AVBTP_PENDING_JOIN;
			attr->param = param;
		}
		attr->registrar_state = AVBTP_MRP_IN;
		break;
	case AVBTP_MRP_EVENT_RX_LV:
	case AVBTP_MRP_EVENT_RX_LVA:
	case AVBTP_MRP_EVENT_TX_LVA:
	case AVBTP_MRP_EVENT_REDECLARE:
		if (attr->registrar_state == AVBTP_MRP_IN) {
			start_avb_timer();
			attr->registrar_state = AVBTP_MRP_LV;
		}
		break;
	case AVBTP_MRP_EVENT_LV_TIMER:
	case AVBTP_MRP_EVENT_FLUSH:
		if (attr->registrar_state == AVBTP_MRP_LV) {
			attr->pending_indications |= AVBTP_PENDING_LEAVE;
			attr->param = param;
		}
		attr->registrar_state = AVBTP_MRP_MT;
		break;
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
