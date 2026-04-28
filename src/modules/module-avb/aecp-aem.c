/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#include "aecp-aem.h"
#include "aecp-aem-descriptors.h"
#include "aecp-aem-state.h"
#include "aecp-aem-cmds-resps/cmd-resp-helpers.h"
#include "gptp.h"
#include "utils.h"

/* The headers including the command and response of the system  */
#include "aecp-aem-cmds-resps/cmd-available.h"
#include "aecp-aem-cmds-resps/cmd-get-set-configuration.h"
#include "aecp-aem-cmds-resps/cmd-get-set-sampling-rate.h"
#include "aecp-aem-cmds-resps/cmd-get-set-control.h"
#include "aecp-aem-cmds-resps/cmd-lock-entity.h"
#include "aecp-aem-cmds-resps/cmd-register-unsolicited-notifications.h"
#include "aecp-aem-cmds-resps/cmd-deregister-unsolicited-notifications.h"
#include "aecp-aem-cmds-resps/cmd-get-set-name.h"
#include "aecp-aem-cmds-resps/cmd-get-set-stream-format.h"
#include "aecp-aem-cmds-resps/cmd-get-set-clock-source.h"
#include "aecp-aem-cmds-resps/cmd-lock-entity.h"
#include "aecp-aem-cmds-resps/cmd-get-dynamic-info.h"
#include "aecp-aem-cmds-resps/cmd-get-set-stream-info.h"
#include "aecp-aem-cmds-resps/cmd-start-stop-streaming.h"
#include "aecp-aem-cmds-resps/cmd-get-counters.h"
#include "aecp-aem-cmds-resps/cmd-get-as-path.h"
#include "aecp-aem-cmds-resps/cmd-get-set-max-transit-time.h"


/* ACQUIRE_ENTITY */
static int handle_acquire_entity_avb_legacy(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_acquire *ae;
	const struct descriptor *desc;
	uint16_t desc_type, desc_id;

	ae = (const struct avb_packet_aecp_aem_acquire*)p->payload;

	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp,
				AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	return reply_success(aecp, m, len);
}

/* LOCK_ENTITY */
static int handle_lock_entity_avb_legacy(struct aecp *aecp, int64_t now,
	const void *m, int len)
{
	struct server *server = aecp->server;
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	const struct avb_packet_aecp_aem_acquire *ae;
	const struct descriptor *desc;
	uint16_t desc_type, desc_id;

	ae = (const struct avb_packet_aecp_aem_acquire*)p->payload;

	desc_type = ntohs(ae->descriptor_type);
	desc_id = ntohs(ae->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, p, len);

	if (desc_type != AVB_AEM_DESC_ENTITY || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	return reply_success(aecp, m, len);
}

/* READ_DESCRIPTOR */
static int handle_read_descriptor_common(struct aecp *aecp, int64_t now, const void *m, int len)
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

	if (len < 0 || (size_t)len > sizeof(buf))
		return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);

	psize = sizeof(*rd);
	size = sizeof(*h) + sizeof(*reply) + psize;

	if (desc->size > sizeof(buf) || size > sizeof(buf) - desc->size)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_RESOURCES, m, len);

	memcpy(buf, m, len);

	memcpy(buf + size, descriptor_body(desc), desc->size);
	size += desc->size;
	psize += desc->size;

	h = (void*)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + 12);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}

/* GET_AVB_INFO */
static int handle_get_avb_info_common(struct aecp *aecp, int64_t now,
	const void *m, int len)
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
	uint64_t gm_id_be;
	uint8_t flags;

	i = (struct avb_packet_aecp_aem_get_avb_info*)p->payload;

	desc_type = ntohs(i->descriptor_type);
	desc_id = ntohs(i->descriptor_id);

	desc = server_find_descriptor(server, desc_type, desc_id);
	if (desc == NULL)
		return reply_status(aecp, AVB_AECP_AEM_STATUS_NO_SUCH_DESCRIPTOR, m, len);

	if (desc_type != AVB_AEM_DESC_AVB_INTERFACE || desc_id != 0)
		return reply_not_implemented(aecp, m, len);

	if (len < 0 || (size_t)len > sizeof(buf))
		return reply_status(aecp, AVB_AECP_AEM_STATUS_BAD_ARGUMENTS, m, len);

	avb_interface = descriptor_body(desc);

	memcpy(buf, m, len);

	psize = sizeof(*i);
	size = sizeof(*h) + sizeof(*reply) + psize;

	h = (void*)buf;
	reply = SPA_PTROFF(h, sizeof(*h), void);
	AVB_PACKET_AECP_SET_MESSAGE_TYPE(&reply->aecp, AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE);
	AVB_PACKET_AECP_SET_STATUS(&reply->aecp, AVB_AECP_AEM_STATUS_SUCCESS);
	AVB_PACKET_SET_LENGTH(&reply->aecp.hdr, psize + 12);

	i = (struct avb_packet_aecp_aem_get_avb_info*)reply->payload;
	flags = AVB_AEM_AVB_INFO_FLAG_SRP_ENABLED;
	/* IEEE 1722.1-2021 Section 7.4.40 GET_AVB_INFO. */
	if (avb_gptp_get_grandmaster_id(server->gptp, &gm_id_be)) {
		i->gptp_grandmaster_id = gm_id_be;
		flags |= AVB_AEM_AVB_INFO_FLAG_GPTP_ENABLED;
	} else {
		i->gptp_grandmaster_id = avb_interface->clock_identity;
	}
	i->flags = flags;
	i->propagation_delay = htonl(0);
	i->gptp_domain_number = avb_interface->domain_number;
	i->msrp_mappings_count = htons(0);

	return avb_server_send_packet(server, h->src, AVB_TSN_ETH, buf, size);
}

/* AEM_COMMAND */
/* TODO in the case the AVB mode allows you to modifiy a Milan readonly
descriptor, then create a array of is_readonly depending on the mode used */
static const char * const cmd_names[] = {
	[AVB_AECP_AEM_CMD_ACQUIRE_ENTITY] = "acquire-entity",
	[AVB_AECP_AEM_CMD_LOCK_ENTITY] = "lock-entity",
	[AVB_AECP_AEM_CMD_ENTITY_AVAILABLE] = "entity-available",
	[AVB_AECP_AEM_CMD_CONTROLLER_AVAILABLE] = "controller-available",
	[AVB_AECP_AEM_CMD_READ_DESCRIPTOR] = "read-descriptor",
	[AVB_AECP_AEM_CMD_WRITE_DESCRIPTOR] = "write-descriptor",
	[AVB_AECP_AEM_CMD_SET_CONFIGURATION] = "set-configuration",
	[AVB_AECP_AEM_CMD_GET_CONFIGURATION] = "get-configuration",
	[AVB_AECP_AEM_CMD_SET_STREAM_FORMAT] = "set-stream-format",
	[AVB_AECP_AEM_CMD_GET_STREAM_FORMAT] = "get-stream-format",
	[AVB_AECP_AEM_CMD_SET_VIDEO_FORMAT] = "set-video-format",
	[AVB_AECP_AEM_CMD_GET_VIDEO_FORMAT] = "get-video-format",
	[AVB_AECP_AEM_CMD_SET_SENSOR_FORMAT] = "set-sensor-format",
	[AVB_AECP_AEM_CMD_GET_SENSOR_FORMAT] = "get-sensor-format",
	[AVB_AECP_AEM_CMD_SET_STREAM_INFO] = "set-stream-info",
	[AVB_AECP_AEM_CMD_GET_STREAM_INFO] = "get-stream-info",
	[AVB_AECP_AEM_CMD_SET_NAME] = "set-name",
	[AVB_AECP_AEM_CMD_GET_NAME] = "get-name",
	[AVB_AECP_AEM_CMD_SET_ASSOCIATION_ID] = "set-association-id",
	[AVB_AECP_AEM_CMD_GET_ASSOCIATION_ID] = "get-association-id",
	[AVB_AECP_AEM_CMD_SET_SAMPLING_RATE] = "set-sampling-rate",
	[AVB_AECP_AEM_CMD_GET_SAMPLING_RATE] = "get-sampling-rate",
	[AVB_AECP_AEM_CMD_SET_CLOCK_SOURCE] = "set-clock-source",
	[AVB_AECP_AEM_CMD_GET_CLOCK_SOURCE] = "get-clock-source",
	[AVB_AECP_AEM_CMD_SET_CONTROL] = "set-control",
	[AVB_AECP_AEM_CMD_GET_CONTROL] = "get-control",
	[AVB_AECP_AEM_CMD_INCREMENT_CONTROL] = "increment-control",
	[AVB_AECP_AEM_CMD_DECREMENT_CONTROL] = "decrement-control",
	[AVB_AECP_AEM_CMD_SET_SIGNAL_SELECTOR] = "set-signal-selector",
	[AVB_AECP_AEM_CMD_GET_SIGNAL_SELECTOR] = "get-signal-selector",
	[AVB_AECP_AEM_CMD_SET_MIXER] = "set-mixer",
	[AVB_AECP_AEM_CMD_GET_MIXER] = "get-mixer",
	[AVB_AECP_AEM_CMD_SET_MATRIX] = "set-matrix",
	[AVB_AECP_AEM_CMD_GET_MATRIX] = "get-matrix",
	[AVB_AECP_AEM_CMD_START_STREAMING] = "start-streaming",
	[AVB_AECP_AEM_CMD_STOP_STREAMING] = "stop-streaming",
	[AVB_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION] = "register-unsolicited-notification",
	[AVB_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION] = "deregister-unsolicited-notification",
	[AVB_AECP_AEM_CMD_IDENTIFY_NOTIFICATION] = "identify-notification",
	[AVB_AECP_AEM_CMD_GET_AVB_INFO] = "get-avb-info",
	[AVB_AECP_AEM_CMD_GET_AS_PATH] = "get-as-path",
	[AVB_AECP_AEM_CMD_GET_COUNTERS] = "get-counters",
	[AVB_AECP_AEM_CMD_REBOOT] = "reboot",
	[AVB_AECP_AEM_CMD_GET_AUDIO_MAP] = "get-audio-map",
	[AVB_AECP_AEM_CMD_ADD_AUDIO_MAPPINGS] = "add-audio-mappings",
	[AVB_AECP_AEM_CMD_REMOVE_AUDIO_MAPPINGS] = "remove-audio-mappings",
	[AVB_AECP_AEM_CMD_GET_VIDEO_MAP] = "get-video-map",
	[AVB_AECP_AEM_CMD_ADD_VIDEO_MAPPINGS] = "add-video-mappings",
	[AVB_AECP_AEM_CMD_REMOVE_VIDEO_MAPPINGS] = "remove-video-mappings",
	[AVB_AECP_AEM_CMD_GET_SENSOR_MAP] = "get-sensor-map",
	[AVB_AECP_AEM_CMD_ADD_SENSOR_MAPPINGS] = "add-sensor-mappings",
	[AVB_AECP_AEM_CMD_REMOVE_SENSOR_MAPPINGS] = "remove-sensor-mappings",
	[AVB_AECP_AEM_CMD_START_OPERATION] = "start-operation",
	[AVB_AECP_AEM_CMD_ABORT_OPERATION] = "abort-operation",
	[AVB_AECP_AEM_CMD_OPERATION_STATUS] = "operation-status",
	[AVB_AECP_AEM_CMD_AUTH_ADD_KEY] = "auth-add-key",
	[AVB_AECP_AEM_CMD_AUTH_DELETE_KEY] = "auth-delete-key",
	[AVB_AECP_AEM_CMD_AUTH_GET_KEY_LIST] = "auth-get-key-list",
	[AVB_AECP_AEM_CMD_AUTH_GET_KEY] = "auth-get-key",
	[AVB_AECP_AEM_CMD_AUTH_ADD_KEY_TO_CHAIN] = "auth-add-key-to-chain",
	[AVB_AECP_AEM_CMD_AUTH_DELETE_KEY_FROM_CHAIN] = "auth-delete-key-from-chain",
	[AVB_AECP_AEM_CMD_AUTH_GET_KEYCHAIN_LIST] = "auth-get-keychain-list",
	[AVB_AECP_AEM_CMD_AUTH_GET_IDENTITY] = "auth-get-identity",
	[AVB_AECP_AEM_CMD_AUTH_ADD_TOKEN] = "auth-add-token",
	[AVB_AECP_AEM_CMD_AUTH_DELETE_TOKEN] = "auth-delete-token",
	[AVB_AECP_AEM_CMD_AUTHENTICATE] = "authenticate",
	[AVB_AECP_AEM_CMD_DEAUTHENTICATE] = "deauthenticate",
	[AVB_AECP_AEM_CMD_ENABLE_TRANSPORT_SECURITY] = "enable-transport-security",
	[AVB_AECP_AEM_CMD_DISABLE_TRANSPORT_SECURITY] = "disable-transport-security",
	[AVB_AECP_AEM_CMD_ENABLE_STREAM_ENCRYPTION] = "enable-stream-encryption",
	[AVB_AECP_AEM_CMD_DISABLE_STREAM_ENCRYPTION] = "disable-stream-encryption",
	[AVB_AECP_AEM_CMD_SET_MEMORY_OBJECT_LENGTH] = "set-memory-object-length",
	[AVB_AECP_AEM_CMD_GET_MEMORY_OBJECT_LENGTH] = "get-memory-object-length",
	[AVB_AECP_AEM_CMD_SET_STREAM_BACKUP] = "set-stream-backup",
	[AVB_AECP_AEM_CMD_GET_STREAM_BACKUP] = "get-stream-backup",
	[AVB_AECP_AEM_CMD_GET_DYNAMIC_INFO] = "get-dynamic-info",
	[AVB_AECP_AEM_CMD_SET_MAX_TRANSIT_TIME] = "set-max-transit-time",
	[AVB_AECP_AEM_CMD_GET_MAX_TRANSIT_TIME] = "get-max-transit-time",
};

/* AEM_COMMAND */
struct cmd_info {
	/**
	 * \brief Is Readonly is a hint used to decide whether or not the
	 * unsollocited notifications is to be sent for this descriptor or not
	 */
	const bool is_readonly;

	/**
	 * \brief handle a command for a specific descriptor
	 */
	int (*handle_command) (struct aecp *aecp, int64_t now, const void *p,
		 int len);

	/**
	 * \brief Response are sent upon changes that occure internally
	 * 	and that are then propagated to the network and are not
	 * 	unsollicited notifications
	 */
	int (*handle_response) (struct aecp *aecp, int64_t now, const void *p,
		 int len);

	/**
	 * \brief Handling of the unsolicited notification that are used
	 * to inform subscribed controller about the change of status of
	 * a specific descriptor or the counter associted with it
	 */
	int (*handle_unsol_timer) (struct aecp *aecp, int64_t now);
};

#define AECP_AEM_HANDLE_CMD(cmd, readonly_desc, handle_exec)		\
	[cmd] = {							\
		.is_readonly = readonly_desc,				\
		.handle_command = handle_exec				\
	}


#define AECP_AEM_HANDLE_RESP(cmd, handle_cmd, handle_exec_unsol)	\
	[cmd] = {							\
		.name = name_str,					\
		.is_readonly = false,					\
		.handle_response = handle_cmd				\
	}

#define AECP_AEM_CMD_RESP_AND_UNSOL(cmd, readonly_desc, handle_exec,	\
	 handle_exec_unsol)						\
	 [cmd] = {							\
		.name = name_str,					\
		.is_readonly = readonly_desc,				\
		.handle = handle_exec,					\
		.handle_unsol = handle_exec_unsol			\
	}

static const struct cmd_info cmd_info_avb_legacy[] = {
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_ACQUIRE_ENTITY, true,
		handle_acquire_entity_avb_legacy),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_LOCK_ENTITY, true,
		 handle_lock_entity_avb_legacy),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_CONFIGURATION, false,
		 handle_cmd_get_configuration_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_READ_DESCRIPTOR, true,
		 handle_read_descriptor_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_SAMPLING_RATE, true,
		handle_cmd_get_sampling_rate_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_AVB_INFO, true,
		 handle_get_avb_info_common),
};

static const struct cmd_info cmd_info_milan_v12[] = {
	/** Milan V1.2 should not implement acquire */
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_ACQUIRE_ENTITY, true,
			direct_reply_not_supported),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_LOCK_ENTITY, false,
		 handle_cmd_lock_entity_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_ENTITY_AVAILABLE, true,
		 handle_cmd_entity_available_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_STREAM_FORMAT, false,
		 handle_cmd_set_stream_format_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_STREAM_FORMAT, true,
		 handle_cmd_get_stream_format_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_CONFIGURATION, false,
		 handle_cmd_set_configuration_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_CONFIGURATION, false,
		 handle_cmd_get_configuration_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_READ_DESCRIPTOR, true,
		 handle_read_descriptor_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION,
		false, handle_cmd_register_unsol_notif_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION,
		false, handle_cmd_deregister_unsol_notif_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_AVB_INFO, true,
		 handle_get_avb_info_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_NAME, false,
		handle_cmd_set_name_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_NAME, true,
		handle_cmd_get_name_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_CLOCK_SOURCE, false,
		handle_cmd_set_clock_source_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_CLOCK_SOURCE, true,
		handle_cmd_get_clock_source_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_CONTROL, false,
		handle_cmd_set_control_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_CONTROL, true,
		handle_cmd_get_control_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_SAMPLING_RATE, false,
		handle_cmd_set_sampling_rate_milan_v12),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_SAMPLING_RATE, true,
		handle_cmd_get_sampling_rate_common),

	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_DYNAMIC_INFO, true,
		 handle_cmd_get_dynamic_info_milan_v12),

	/* Milan v1.2 Section 5.4.2.9 / Section 5.4.2.10 */
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_STREAM_INFO, true,
		 handle_cmd_get_stream_info_milan_v12),
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_STREAM_INFO, false,
		 handle_cmd_set_stream_info_milan_v12),

	/* Milan v1.2 Section 5.4.2.19 / Section 5.4.2.20 */
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_START_STREAMING, false,
		 handle_cmd_start_streaming_milan_v12),
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_STOP_STREAMING, false,
		 handle_cmd_stop_streaming_milan_v12),

	/* Milan v1.2 Section 5.4.2.25 */
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_COUNTERS, true,
		 handle_cmd_get_counters_milan_v12),

	/* IEEE 1722.1-2021 Section 7.4.41. Returns NOT_IMPLEMENTED until gPTP lands;
	 * dispatched here so the response payload size matches what the
	 * controller computed (4-byte command echo). */
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_AS_PATH, true,
		 handle_cmd_get_as_path_milan_v12),

	/* SET_MAX_TRANSIT_TIME = 0x004C, GET_MAX_TRANSIT_TIME = 0x004D
	 * (relocated from IEEE 1722.1-2021 originals 0x004B/0x004C because
	 * 0x004B is GET_DYNAMIC_INFO in this stack). GET reflects the MSRP
	 * accumulated_latency floor; SET rejects values below it. */
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_SET_MAX_TRANSIT_TIME, false,
		 handle_cmd_set_max_transit_time_milan_v12),
	AECP_AEM_HANDLE_CMD(AVB_AECP_AEM_CMD_GET_MAX_TRANSIT_TIME, true,
		 handle_cmd_get_max_transit_time_milan_v12),
};

static const struct {
	const struct cmd_info *cmd_info;
	size_t count;
} cmd_info_modes[AVB_MODE_MAX] = {
	[AVB_MODE_LEGACY] = {
		.cmd_info = cmd_info_avb_legacy,
		.count = SPA_N_ELEMENTS(cmd_info_avb_legacy),
	},
	[AVB_MODE_MILAN_V12] = {
		.cmd_info = cmd_info_milan_v12,
		.count = SPA_N_ELEMENTS(cmd_info_milan_v12),
	},
};

/**
 * \brief Stub that queries the AECP entity lock state.
 *
 * Returns true when the entity is currently locked by a *different* controller
 * than the one sending the command, meaning the command must be rejected with
 * ENTITY_LOCKED.  Returns false in all other cases (not locked, lock expired,
 * or requester is the lock owner).
 *
 * Only Milan V1.2 entities maintain a lock state; legacy AVB entities always
 * return false (unlocked).
 */
static bool check_locked(struct aecp *aecp, int64_t now,
		const struct avb_packet_aecp_aem *p)
{
	struct server *server = aecp->server;
	const struct descriptor *desc;
	const struct aecp_aem_entity_milan_state *entity_state;
	const struct aecp_aem_lock_state *lock;

	if (server->avb_mode != AVB_MODE_MILAN_V12)
		return false;

	desc = server_find_descriptor(server, AVB_AEM_DESC_ENTITY, 0);
	if (desc == NULL)
		return false;

	entity_state = desc->ptr;
	lock = &entity_state->state.lock_state;

	/* A lock that has expired is treated as if the entity is unlocked. */
	if (lock->base_info.expire_timeout < now)
		return false;

	/* Locked by a different controller → reject. */
	return lock->is_locked &&
		(lock->locked_id != htobe64(p->aecp.controller_guid));
}

int avb_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	uint16_t cmd_type;
	struct server *server = aecp->server;
	const struct cmd_info *info;
	struct timespec ts_now = {0};
	int64_t now;

	cmd_type = AVB_PACKET_AEM_GET_COMMAND_TYPE(p);

	if (cmd_info_modes[server->avb_mode].count <= cmd_type) {
		pw_log_warn("unknown aem command %d (max %zu)\n", cmd_type,
			cmd_info_modes[server->avb_mode].count);
		return reply_not_implemented(aecp, m, len);
	}

	pw_log_info("mode: %s aem command %s",
		get_avb_mode_str(server->avb_mode),
		cmd_type < SPA_N_ELEMENTS(cmd_names) && cmd_names[cmd_type]
			? cmd_names[cmd_type] : "unknown");

	info = &cmd_info_modes[server->avb_mode].cmd_info[cmd_type];
	if (!info || !info->handle_command )
		return reply_not_implemented(aecp, m, len);


	if (clock_gettime(CLOCK_TAI, &ts_now)) {
		pw_log_warn("clock_gettime(CLOCK_TAI): %m\n");
	}

	now = SPA_TIMESPEC_TO_NSEC(&ts_now);

	/*
	 * For write (non-readonly) commands, check whether the entity is locked
	 * by a different controller before dispatching the handler.  Readonly
	 * commands are always allowed regardless of lock state.
	 */
	if (!info->is_readonly && check_locked(aecp, now, p)) {
		pw_log_info("aem command %d rejected: entity locked", cmd_type);
		return reply_entity_locked(aecp, m, len);
	}

	return info->handle_command(aecp, now, m, len);
}

void avb_aecp_aem_periodic(struct aecp *aecp, int64_t now)
{
	struct server *server = aecp->server;

	if (server->avb_mode != AVB_MODE_MILAN_V12)
		return;

	handle_cmd_lock_entity_expired_milan_v12(aecp, now);

	/* Milan Section 5.4.5: emit unsolicited GET_COUNTERS when any counter is
	 * updated, max once per descriptor per second. The dirty/rate-limit
	 * is enforced per descriptor inside this function — no outer gate. */
	cmd_get_counters_periodic_milan_v12(aecp, now);

	{
		struct server *srv = aecp->server;
		uint16_t i;
		for (i = 0; i < UINT16_MAX; i++) {
			struct descriptor *d = server_find_descriptor(srv,
					AVB_AEM_DESC_STREAM_INPUT, i);
			struct aecp_aem_stream_input_state *si;
			if (d == NULL)
				break;
			si = d->ptr;
			if (si->stream_info_dirty) {
				cmd_get_stream_info_emit_unsol_milan_v12(srv,
						AVB_AEM_DESC_STREAM_INPUT, i);
				si->stream_info_dirty = false;
			}
		}
		for (i = 0; i < UINT16_MAX; i++) {
			struct descriptor *d = server_find_descriptor(srv,
					AVB_AEM_DESC_STREAM_OUTPUT, i);
			struct aecp_aem_stream_output_state *so;
			if (d == NULL)
				break;
			so = d->ptr;
			if (so->stream_info_dirty) {
				cmd_get_stream_info_emit_unsol_milan_v12(srv,
						AVB_AEM_DESC_STREAM_OUTPUT, i);
				so->stream_info_dirty = false;
			}
		}
	}
}

void avb_aecp_aem_mark_stream_info_dirty(struct server *server,
		uint16_t desc_type, uint16_t desc_index)
{
	struct descriptor *d = server_find_descriptor(server, desc_type, desc_index);
	if (d == NULL)
		return;
	if (desc_type == AVB_AEM_DESC_STREAM_INPUT) {
		struct aecp_aem_stream_input_state *si = d->ptr;
		si->stream_info_dirty = true;
	} else if (desc_type == AVB_AEM_DESC_STREAM_OUTPUT) {
		struct aecp_aem_stream_output_state *so = d->ptr;
		so->stream_info_dirty = true;
	}
}

void avb_aecp_aem_mark_counters_dirty(struct server *server,
		uint16_t desc_type, uint16_t desc_index)
{
	struct descriptor *d = server_find_descriptor(server, desc_type, desc_index);
	if (d == NULL)
		return;
	if (desc_type == AVB_AEM_DESC_AVB_INTERFACE) {
		struct aecp_aem_avb_interface_state *ifs = d->ptr;
		ifs->counters_dirty = true;
	} else if (desc_type == AVB_AEM_DESC_STREAM_INPUT) {
		struct aecp_aem_stream_input_state *si = d->ptr;
		si->counters_dirty = true;
	} else if (desc_type == AVB_AEM_DESC_STREAM_OUTPUT) {
		struct aecp_aem_stream_output_state *so = d->ptr;
		so->counters_dirty = true;
	}
}

int avb_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len)
{
	return 0;
}
