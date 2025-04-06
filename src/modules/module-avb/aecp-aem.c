/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <time.h>

#include "aecp-aem.h"
#include "aecp-aem-state.h"
#include "aecp-aem-descriptors.h"

static int reply_status(struct aecp *aecp, int status, const void *m, int len)
{
	struct server *server = aecp->server;
	uint8_t buf[len];
	struct avb_ethernet_header *h = (void*)buf;
	struct avb_packet_aecp_header *reply = SPA_PTROFF(h, sizeof(*h), void);

	memcpy(buf, m, len);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(reply, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(reply, status);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

static int reply_not_implemented(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVB_AECP_AEM_STATUS_NOT_IMPLEMENTED, m, len);
}

static int reply_not_supported(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVB_AECP_AEM_STATUS_NOT_SUPPORTED, m, len);
}

static int reply_locked(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVB_AECP_AEM_STATUS_ENTITY_LOCKED, m, len);
}

static int reply_success(struct aecp *aecp, const void *m, int len)
{
	return reply_status(aecp, AVB_AECP_AEM_STATUS_SUCCESS, m, len);
}

/* ACQUIRE_ENTITY */
static int handle_acquire_entity(struct aecp *aecp, const void *m, int len)
{
#ifndef USE_MILAN
	const struct avb_packet_aecp_aem *p = m;
	const struct avb_packet_aecp_aem_acquire *ae;
	struct server *server = aecp->server;

	const struct descriptor *desc;
	uint16_t desc_type, desc_id;

	ae = (const struct avb_packet_aecp_aem_acquire*)p->payload;

	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

#endif

#ifdef USE_MILAN
	return reply_not_supported(aecp, m, len);

#else // USE_MILAN
	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	return reply_success(aecp, m, len);
#endif // USE_MILAN
}

/* LOCK_ENTITY */
#define AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT (60UL)
#define AECP_AEM_LOCK_ENTITY_FLAG_LOCK		(1)
static int handle_lock_entity(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_lock *ae;


	struct avb_ethernet_header *h_reply;
	struct avb_packet_aecp_aem *p_reply;
	struct avb_packet_aecp_aem_lock *ae_reply;

	const struct descriptor *desc;
	struct aecp_aem_lock_state *lock;
	uint16_t desc_type, desc_id;
	struct timespec ts_now;

	uint8_t buf[1024];

	ae = (const struct avb_packet_aecp_aem_lock*)p->payload;
	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	lock = aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid), aecp_aem_lock);
	if (!lock) {
		pw_log_error("invalid lock \n");
		spa_assert(0);
	}

	if (ae->flags & htonl(AECP_AEM_LOCK_ENTITY_FLAG_LOCK)) {
		/* Unlocking */
		if (!lock->is_locked) {
			return reply_success(aecp, m, len);
		}

		pw_log_debug("un-locking the entity %lx\n", htobe64(p->aecp.controller_guid));
		if (htobe64(p->aecp.controller_guid) == lock->locked_id) {
			pw_log_debug("unlocking\n");
			lock->is_locked = false;
			lock->locked_id = 0;
			return reply_success(aecp, m, len);
		} else {
			if (htobe64(p->aecp.controller_guid) != lock->locked_id) {
				pw_log_debug("but the device is locked by  %lx\n", htobe64(lock->locked_id));
				return reply_locked(aecp, m, len);
			} else {
				pw_log_error("Invalid state\n");
				spa_assert(0);
			}
		}
	} else {
		/* Locking */
		if (clock_gettime(CLOCK_MONOTONIC, &ts_now)) {
			pw_log_error("while getting CLOCK_MONOTONIC time");
			spa_assert(0);
		}

		// Is it really locked?
		if (!lock->is_locked || lock->expires < SPA_TIMESPEC_TO_NSEC(&ts_now)) {
			ts_now.tv_sec += AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT;
			lock->expires = SPA_TIMESPEC_TO_NSEC(&ts_now);
			lock->is_locked = true;
			lock->locked_id = htobe64(p->aecp.controller_guid);
		} else {
			// If the lock is taken again by device
			if (htobe64(p->aecp.controller_guid) == lock->locked_id) {
					lock->expires += AECP_AEM_LOCK_ENTITY_EXPIRE_TIMEOUT;
					lock->is_locked = true;
			} else {
				// Cannot lock because already locked
				pw_log_debug("The device is locked");
				return reply_locked(aecp, m, len);
			}
		}
	}

	/* Forge the response for the entity that is locking the device */
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *) buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	ae_reply = (struct avb_packet_aecp_aem_lock*)p_reply->payload;
	ae_reply->locked_guid = htobe64(lock->locked_id);

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p_reply->aecp,
										AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&p_reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

/* ENTITY AVAILABLE according to the locking state */
#define AECP_AEM_AVAIL_ENTITY_ACQUIRED 		(1<<0)
#define AECP_AEM_AVAIL_ENTITY_LOCKED		(1<<1)
#define AECP_AEM_AVAIL_SUBENTITY_ACQUIRED	(1<<2)
#define AECP_AEM_AVAIL_SUBENTITY_LOCKED 	(1<<3)

static int handle_entity_available(struct aecp *aecp, const void *m, int len)
{
	/* Commnand received specific */
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	struct avb_ethernet_header *h_reply;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);

	/* Reply specific */
	struct avb_packet_aecp_aem *p_reply;
	const struct avb_packet_aecp_aem_available *avail;
	struct avb_packet_aecp_aem_available *avail_reply;

	/* Entity specific */
	struct aecp_aem_lock_state *lock;
	struct timespec ts_now;
	uint8_t buf[1024];

#ifndef USE_MILAN
// TODO get the acquire state
#endif // USE_MILAN

	/* Forge the response for the entity that is locking the device */
	memcpy(buf, m, len);
	h_reply = (struct avb_ethernet_header *) buf;
	p_reply = SPA_PTROFF(h_reply, sizeof(*h_reply), void);
	avail_reply = (struct avb_packet_aecp_aem_available*)p_reply->payload;

#ifdef USE_MILAN
	avail_reply->acquired_controller_guid = 0;
#else // USE_MILAN
// TODO
#endif // USE_MILAN

	lock = aecp_aem_get_state_var(aecp, p->aecp.target_guid, aecp_aem_lock);
	/* Locking */
	if (clock_gettime(CLOCK_MONOTONIC, &ts_now)) {
		pw_log_error("while getting CLOCK_MONOTONIC time");
		spa_assert(0);
	}

	if (lock->expires < SPA_TIMESPEC_TO_NSEC(&ts_now) || !lock->is_locked) {
		avail_reply->lock_controller_guid = 0;
		avail_reply->flags = 0;
	} else if (lock->is_locked) {
		avail_reply->lock_controller_guid = htobe64(lock->locked_id);
		avail_reply->flags = htonl(AECP_AEM_AVAIL_ENTITY_LOCKED);
	}

	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&p_reply->aecp,
										AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&p_reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, len);
}

/* READ_DESCRIPTOR */
static int handle_read_descriptor(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *reply;
	const struct avb_packet_aecp_aem_read_descriptor *rd;
	uint16_t desc_type, desc_id;
	const struct descriptor *desc;
	uint8_t buf[2048];
	size_t size, psize;

	rd = (struct avb_packet_aecp_aem_read_descriptor*)p->payload;

	desc_type = ntohs(rd->descriptor_type);
	desc_id = ntohs(rd->descriptor_id);

	pw_log_info("descriptor type:%04x index:%d", desc_type, desc_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	memcpy(buf, m, len);

	psize = sizeof(*rd);
	size = sizeof(*h) + sizeof(*reply) + psize;

	memcpy(buf + size, desc->ptr, desc->size);
	size += desc->size;
	psize += desc->size;

	h = (void*)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + 12);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}

static int handle_set_configuration(struct aecp *aecp, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_configuration(struct aecp *aecp, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_set_stream_format(struct aecp *aecp, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_stream_format(struct aecp *aecp, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_set_stream_info(struct aecp *aecp, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_stream_info(struct aecp *aecp, const void *m, int len)
{
	// TODO difference with the stream input or the stream output
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_set_name(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_name(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_set_sampling_rate(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_sampling_rate(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_clock_source(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_set_clock_source(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_set_control(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_control(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);

}

static int handle_start_streaming(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);

}

static int handle_stop_streaming(struct aecp *aecp, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}


static int handle_register_unsol_notifications(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_deregister_unsol_notifications(struct aecp *aecp, const void *m, int len)
{
	// TODO action to provide update every seconds see state machines
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

/* GET_AVB_INFO */
static int handle_get_avb_info(struct aecp *aecp, const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	struct avb_packet_aecp_aem *reply;
	struct avb_packet_aecp_aem_get_avb_info *i;
	struct avb_aem_desc_avb_interface *avb_interface;
	uint16_t desc_type, desc_id;
	const struct descriptor *desc;
	uint8_t buf[2048];
	size_t size, psize;

	i = (struct avb_packet_aecp_aem_get_avb_info*)p->payload;

	desc_type = ntohs(i->descriptor_type);
	desc_id = ntohs(i->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	if (desc_type != AVB_AEM_DESC_AVB_INTERFACE || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	avb_interface = desc->ptr;

	memcpy(buf, m, len);

	psize = sizeof(*i);
	size = sizeof(*h) + sizeof(*reply) + psize;

	h = (void*)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + 12);

	i = (struct avb_packet_aecp_aem_get_avb_info*)reply->payload;
	i->gptp_grandmaster_id = avb_interface->clock_identity;
	i->propagation_delay = htonl(0);
	i->gptp_domain_number = avb_interface->domain_number;
	i->flags = 0;
	i->msrp_mappings_count = htons(0);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}

static int handle_get_as_path(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_counters(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_audio_map(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_add_audio_mappings(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_remove_audio_mappings(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_dynamic_info(struct aecp *aecp, const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

/* AEM_COMMAND */
struct cmd_info {
	const char *name;
	const bool is_readonly;
	int (*handle) (struct aecp *aecp, const void *p, int len);
};
#define AECP_AEM_CMD_RESP(cmd, readonly_desc, name_str, handle_exec) \
		[cmd] = { .name = name_str, .is_readonly = readonly_desc, 	 \
										 .handle = handle_exec}

static const struct cmd_info cmd_info[] = {
	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_ACQUIRE_ENTITY, false,
						"acquire-entity", handle_acquire_entity),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_LOCK_ENTITY, true,
						"lock-entity", handle_lock_entity),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_ENTITY_AVAILABLE, true,
						"entity-available", handle_entity_available),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_CONTROLLER_AVAILABLE, true,
						"controller-available", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_READ_DESCRIPTOR, true,
						"read-descriptor", handle_read_descriptor),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_WRITE_DESCRIPTOR, false,
						"write-descriptor", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_CONFIGURATION, false,
						"set-configuration", handle_set_configuration),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_CONFIGURATION, true,
						"get-configuration", handle_get_configuration),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_STREAM_FORMAT, false,
						"set-stream-format", handle_set_stream_format),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_STREAM_FORMAT, true,
						"get-stream-format", handle_get_stream_format),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_VIDEO_FORMAT, false,
						"set-video-format", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_VIDEO_FORMAT, true,
						"get-video-format", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_SENSOR_FORMAT, false,
						"set-sensor-format", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_SENSOR_FORMAT, true,
						"get-sensor-format", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_STREAM_INFO, false,
						"set-stream-info", handle_set_stream_info),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_STREAM_INFO, true,
						"get-stream-info", handle_get_stream_info),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_NAME, false,
						"set-name", handle_set_name),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_NAME, true,
						"get-name", handle_get_name),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_ASSOCIATION_ID, false,
						"set-association-id", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_ASSOCIATION_ID, true,
						"get-association-id", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_SAMPLING_RATE, false,
						"set-sampling-rate", handle_set_sampling_rate),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_SAMPLING_RATE, true,
						"get-sampling-rate", handle_get_sampling_rate),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_CLOCK_SOURCE, false,
						"set-clock-source", handle_set_clock_source),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_CLOCK_SOURCE, true,
						"get-clock-source", handle_get_clock_source),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_CONTROL, false,
						"set-control", handle_set_control),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_CONTROL, true,
						"get-control", handle_get_control),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_INCREMENT_CONTROL, false,
						"increment-control", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_DECREMENT_CONTROL, false,
						"decrement-control", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_SIGNAL_SELECTOR, false,
						"set-signal-selector", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_SIGNAL_SELECTOR, true,
						"get-signal-selector", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_MIXER, false,
						"set-mixer", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_MIXER, true,
						"get-mixer", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_SET_MATRIX, false,
						"set-matrix", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_MATRIX, true,
						"get-matrix", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_START_STREAMING, false,
						"start-streaming", handle_start_streaming),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_STOP_STREAMING, false,
						"stop-streaming", handle_stop_streaming),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION,
						 false, "register-unsolicited-notification",
						 handle_register_unsol_notifications),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION,
						 false, "deregister-unsolicited-notification",
						 handle_deregister_unsol_notifications),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_IDENTIFY_NOTIFICATION, true,
						"identify-notification", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_AVB_INFO, true,
						"get-avb-info", handle_get_avb_info),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_AS_PATH, true,
						"get-as-path", handle_get_as_path),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_COUNTERS, true,
						"get-counters", handle_get_counters),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_REBOOT, false,
						"reboot", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_AUDIO_MAP, true,
						"get-audio-map", handle_get_audio_map),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_ADD_AUDIO_MAPPINGS, false,
						"add-audio-mappings", handle_add_audio_mappings),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_REMOVE_AUDIO_MAPPINGS, false,
						"remove-audio-mappings", handle_remove_audio_mappings),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_VIDEO_MAP, true,
						"get-video-map", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_ADD_VIDEO_MAPPINGS, false,
						"add-video-mappings", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_REMOVE_VIDEO_MAPPINGS, false,
						"remove-video-mappings", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_SENSOR_MAP, true,
						"get-sensor-map", NULL),

	AECP_AEM_CMD_RESP( AVB_AECP_AEM_CMD_GET_DYNAMIC_INFO, true,
						"get-dynamic-info", handle_get_dynamic_info),

};

static inline const struct cmd_info *find_cmd_info(uint16_t type, const char *name)
{
	if (type > (sizeof(cmd_info) / sizeof(cmd_info[0]))) {
		pw_log_error("Invalid Command Type %u\n", type);
		return NULL;
	}

	return &cmd_info[type];
}

static inline bool check_locked(struct aecp *aecp,
	const struct avb_packet_aecp_aem *p, const struct cmd_info *cmd)
{
	struct timespec ts_now;
	int64_t now;

	pw_log_info("Accessing %s, current entity is %s\n",
		cmd->is_readonly? "ro" : "wo", "locked");

	struct aecp_aem_lock_state *lock;
	lock = aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid),
								  aecp_aem_lock);
	// No lock was found for the entity, that is an error, return issue
	if (!lock) {
		pw_log_error("Cannot retrieve the lock\n");
		return false;
	}

	/**
	 * Time is always using the monotonic time
	 */
	if (clock_gettime(CLOCK_MONOTONIC, &ts_now)) {
		pw_log_error("while getting CLOCK_MONOTONIC time\n");
	}

	/* Check whether the lock has expired */
	now = SPA_TIMESPEC_TO_NSEC(&ts_now);
	if (lock->expires < now) {
		return false;
	}

	/**
	 * Return lock if the lock is locked and the controller id is different
	 * than the controller locking the entity
	 */
	return lock->is_locked &&
			(lock->locked_id != htobe64(p->aecp.controller_guid));
}

int avb_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	uint16_t cmd_type;
	const struct cmd_info *info;

	cmd_type = AVB_PACKET_AEM_GET_COMMAND_TYPE(p);

	info = find_cmd_info(cmd_type, NULL);
	if (info == NULL)
		return reply_not_implemented(aecp, m, len);

	pw_log_info("aem command %s", info->name);

	if (info->handle == NULL)
		return reply_not_implemented(aecp, m, len);

	if (info->is_readonly) {
		return info->handle(aecp, m, len);
	} else {
		/** If not locked then continue below */
		if (!check_locked(aecp, p, &cmd_info[cmd_type])) {
			return info->handle(aecp, m, len);
		}
	}

	return -1;
}

int avb_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len)
{
	return 0;
}

#ifdef USE_MILAN

static uint64_t avb_general_48_char_to_64bit(const uint8_t *input48)
{
	uint64_t output64 = 0;
	for (uint32_t pos = 0; pos < 6; pos++) {
		output64 |=  input48[pos] << (pos << 3);
	}

	return  output64;
}

int avb_aecp_vendor_unique_command(struct aecp *aecp, const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_milan_vendor_unique *p = SPA_PTROFF(h, sizeof(*h), void);
	uint64_t mvu = avb_general_48_char_to_64bit(p->protocol_id);

	pw_log_warn("Retrieve value of %lu\n", mvu);

	return 0;
}

int avb_aecp_vendor_unique_response(struct aecp *aecp, const void *m, int len)
{
	return 0;
}

#endif // USE_MILAN
