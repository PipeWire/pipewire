/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <time.h>
#include <limits.h>

#include "aecp-aem.h"
#include "aecp-aem-state.h"
#include "aecp-aem-descriptors.h"
#include "utils.h"

/** Below is the list of command handlers */
#include "aecp-cmd-resp/aecp-aem-helpers.h"

#include "aecp-cmd-resp/aecp-aem-available.h"
#include "aecp-cmd-resp/aecp-aem-cmd-set-stream-format.h"
#include "aecp-cmd-resp/aecp-aem-cmd-set-sampling-rate.h"
#include "aecp-cmd-resp/aecp-aem-configuration.h"
#include "aecp-cmd-resp/aecp-aem-descriptors.h"
#include "aecp-cmd-resp/aecp-aem-get-avb-info.h"
#include "aecp-cmd-resp/aecp-aem-cmd-get-name.h"
#include "aecp-cmd-resp/aecp-aem-lock-entity.h"
#include "aecp-cmd-resp/aecp-aem-cmd-set-clock-source.h"
#include "aecp-cmd-resp/aecp-aem-cmd-set-name.h"
#include "aecp-cmd-resp/aecp-aem-cmd-get-counters.h"
#include "aecp-cmd-resp/aecp-aem-lock-entity.h"
#include "aecp-cmd-resp/aecp-aem-cmd-set-control.h"
#include "aecp-cmd-resp/aecp-aem-unsol-notifications.h"

/* ACQUIRE_ENTITY */
static int handle_acquire_entity(struct aecp *aecp, int64_t now, const void *m, int len)
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

static int handle_get_stream_format(struct aecp *aecp, int64_t now, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_set_stream_info(struct aecp *aecp, int64_t now, const void *m, int len)
{
	// TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_stream_info(struct aecp *aecp, int64_t now, const void *m, int len)
{
	// TODO difference with the stream input or the stream output
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_sampling_rate(struct aecp *aecp, int64_t now, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_clock_source(struct aecp *aecp, int64_t now, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_control(struct aecp *aecp, int64_t now, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);

}

static int handle_start_streaming(struct aecp *aecp, int64_t now, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);

}

static int handle_stop_streaming(struct aecp *aecp, int64_t now, const void *m, int len)
{
	//TODO
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_as_path(struct aecp *aecp, int64_t now, const void *m,
	 int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_audio_map(struct aecp *aecp, int64_t now,
	 const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_add_audio_mappings(struct aecp *aecp, int64_t now,
	 const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_remove_audio_mappings(struct aecp *aecp, int64_t now,
	 const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

static int handle_get_dynamic_info(struct aecp *aecp, int64_t now,
	 const void *m, int len)
{
	pw_log_warn("%s: +%d: has to be implemented\n", __func__, __LINE__);
	return reply_not_implemented(aecp, m, len);
}

/* AEM_COMMAND */
struct cmd_info {
	const char *name;
	const bool is_readonly;
	int (*handle_command) (struct aecp *aecp, int64_t now, const void *p,
		 int len);
	int (*handle_response) (struct aecp *aecp, int64_t now, const void *p,
		 int len);
	int (*handle_unsol) (struct aecp *aecp, int64_t now);
};

#define AECP_AEM_HANDLE_CMD(cmd, readonly_desc, name_str, handle_exec)			\
	[cmd] = { .name = name_str, .is_readonly = readonly_desc, 	 				\
				.handle_command = handle_exec }

#define AECP_AEM_HANDLE_CMD_UNSOL(cmd, readonly_desc, name_str, handle_exec,	\
	 handle_exec_unsol) 														\
	[cmd] = { .name = name_str, .is_readonly = readonly_desc,					\
			  .handle_command = handle_exec, .handle_unsol = handle_exec_unsol }

#define AECP_AEM_HANDLE_RESP(cmd, name_str, handle_cmd,							\
		handle_exec_unsol)														\
	[cmd] = { .name = name_str, .is_readonly = false,							\
			  .handle_response = handle_cmd }

/** Helper to create a handler for response and unsolicited notifications */
#define AECP_AEM_HANDLE_RESP_UNSOL(cmd, name_str, handle_cmd,					\
		handle_exec_unsol)														\
	[cmd] = { .name = name_str, .is_readonly = false,							\
			  .handle_response = handle_cmd, handle_unsol = handle_exec_unsol }


#define AECP_AEM_CMD_RESP_AND_UNSOL(cmd, readonly_desc, name_str, handle_exec, \
	 handle_exec_unsol) \
	[cmd] = { .name = name_str, .is_readonly = readonly_desc, 	 \
	.handle = handle_exec, .handle_unsol = handle_exec_unsol }

static const struct cmd_info cmd_info[] = {
	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_ACQUIRE_ENTITY, false,
						"acquire-entity", handle_acquire_entity),

	AECP_AEM_HANDLE_CMD_UNSOL( AVB_AECP_AEM_CMD_LOCK_ENTITY, false,
						"lock-entity", handle_cmd_lock_entity,
						 handle_unsol_lock_entity),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_ENTITY_AVAILABLE, true,
						"entity-available", handle_cmd_entity_available),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_CONTROLLER_AVAILABLE, true,
						"controller-available", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_READ_DESCRIPTOR, true,
						"read-descriptor", handle_cmd_read_descriptor),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_WRITE_DESCRIPTOR, false,
						"write-descriptor", NULL),

	AECP_AEM_HANDLE_CMD_UNSOL( AVB_AECP_AEM_CMD_SET_CONFIGURATION, false,
						"set-configuration", handle_cmd_set_configuration,
						handle_unsol_set_configuration),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_CONFIGURATION, true,
						"get-configuration", handle_cmd_get_configuration),

	AECP_AEM_HANDLE_CMD_UNSOL( AVB_AECP_AEM_CMD_SET_STREAM_FORMAT, false,
						"set-stream-format", handle_cmd_set_stream_format,
						handle_unsol_set_stream_format),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_STREAM_FORMAT, true,
						"get-stream-format", handle_get_stream_format),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_SET_VIDEO_FORMAT, false,
						"set-video-format", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_VIDEO_FORMAT, true,
						"get-video-format", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_SET_SENSOR_FORMAT, false,
						"set-sensor-format", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_SENSOR_FORMAT, true,
						"get-sensor-format", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_SET_STREAM_INFO, false,
						"set-stream-info", handle_set_stream_info),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_STREAM_INFO, true,
						"get-stream-info", handle_get_stream_info),

	AECP_AEM_HANDLE_CMD_UNSOL(AVB_AECP_AEM_CMD_SET_NAME, false,
						"set-name", handle_cmd_set_name, handle_unsol_set_name),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_NAME, true,
						"get-name", handle_cmd_get_name),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_SET_ASSOCIATION_ID, false,
						"set-association-id", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_ASSOCIATION_ID, true,
						"get-association-id", NULL),

	AECP_AEM_HANDLE_CMD_UNSOL( AVB_AECP_AEM_CMD_SET_SAMPLING_RATE, false,
						"set-sampling-rate", handle_cmd_set_sampling_rate,
						handle_unsol_sampling_rate),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_SAMPLING_RATE, true,
						"get-sampling-rate", handle_get_sampling_rate),

	AECP_AEM_HANDLE_CMD_UNSOL( AVB_AECP_AEM_CMD_SET_CLOCK_SOURCE, false,
						"set-clock-source", handle_cmd_set_clock_source,
						handle_unsol_set_clock_source),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_CLOCK_SOURCE, true,
						"get-clock-source", handle_get_clock_source),

	AECP_AEM_HANDLE_CMD_UNSOL( AVB_AECP_AEM_CMD_SET_CONTROL, false,
						"set-control", handle_cmd_set_control,
						handle_unsol_set_control),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_CONTROL, true,
						"get-control", handle_get_control),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_INCREMENT_CONTROL, false,
						"increment-control", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_DECREMENT_CONTROL, false,
						"decrement-control", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_SET_SIGNAL_SELECTOR, false,
						"set-signal-selector", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_SIGNAL_SELECTOR, true,
						"get-signal-selector", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_SET_MIXER, false,
						"set-mixer", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_MIXER, true,
						"get-mixer", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_SET_MATRIX, false,
						"set-matrix", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_MATRIX, true,
						"get-matrix", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_START_STREAMING, false,
						"start-streaming", handle_start_streaming),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_STOP_STREAMING, false,
						"stop-streaming", handle_stop_streaming),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_REGISTER_UNSOLICITED_NOTIFICATION,
						 true, "register-unsolicited-notification",
						 handle_cmd_register_unsol_notifications),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_DEREGISTER_UNSOLICITED_NOTIFICATION,
						 true, "deregister-unsolicited-notification",
						 handle_cmd_deregister_unsol_notifications),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_IDENTIFY_NOTIFICATION, true,
						"identify-notification", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_AVB_INFO, true,
						"get-avb-info", handle_cmd_get_avb_info),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_AS_PATH, true,
						"get-as-path", handle_get_as_path),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_COUNTERS, true,
						"get-counters", handle_cmd_get_counters),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_REBOOT, false,
						"reboot", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_AUDIO_MAP, true,
						"get-audio-map", handle_get_audio_map),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_ADD_AUDIO_MAPPINGS, false,
						"add-audio-mappings", handle_add_audio_mappings),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_REMOVE_AUDIO_MAPPINGS, false,
						"remove-audio-mappings", handle_remove_audio_mappings),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_VIDEO_MAP, true,
						"get-video-map", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_ADD_VIDEO_MAPPINGS, false,
						"add-video-mappings", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_REMOVE_VIDEO_MAPPINGS, false,
						"remove-video-mappings", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_SENSOR_MAP, true,
						"get-sensor-map", NULL),

	AECP_AEM_HANDLE_CMD( AVB_AECP_AEM_CMD_GET_DYNAMIC_INFO, true,
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

static inline bool check_locked(struct aecp *aecp, int64_t now,
	const struct avb_packet_aecp_aem *p, const struct cmd_info *cmd)
{
	pw_log_info("Accessing %s, current entity is %s\n",
		cmd->is_readonly? "ro" : "wo", "locked");

	struct aecp_aem_lock_state lock = { 0 };
	int rc = aecp_aem_get_state_var(aecp, htobe64(p->aecp.target_guid),
								  aecp_aem_lock, 0, &lock);

								  	// No lock was found for the entity, that is an error, return issue
	if (rc) {
		pw_log_error("Issue while retrieving lock\n");
		spa_assert(0);
	}

	if (lock.base_info.expire_timeout < now) {
		return false;
	}

	/**
	 * Return lock if the lock is locked and the controller id is different
	 * than the controller locking the entity
	 */
	return lock.is_locked &&
			(lock.locked_id != htobe64(p->aecp.controller_guid));
}

int avb_aecp_aem_handle_command(struct aecp *aecp, const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	uint16_t cmd_type;
	int rc = -1;
	const struct cmd_info *info;
	struct timespec ts_now;
	int64_t now;

	/**
	 * Time is always using the monotonic time
	 */
	if (clock_gettime(CLOCK_TAI, &ts_now)) {
		pw_log_error("while getting CLOCK_MONOTONIC time\n");
	}

	now = SPA_TIMESPEC_TO_NSEC(&ts_now);
	cmd_type = AVB_PACKET_AEM_GET_COMMAND_TYPE(p);

	info = find_cmd_info(cmd_type, NULL);
	if (info == NULL)
		return reply_not_implemented(aecp, m, len);

	pw_log_info("aem command %s %ld", info->name, now);

	if (info->handle_command == NULL)
		return reply_not_implemented(aecp, m, len);

	if (info->is_readonly) {
		return info->handle_command(aecp, now, m, len);
	} else {
		/** If not locked then continue below */
		if (!check_locked(aecp, now,p, &cmd_info[cmd_type])) {
			rc = info->handle_command(aecp, now, m, len);
			if (rc) {
				pw_log_error("handling returned %d\n", rc);
				return -1;
			}

			if (info->handle_unsol) {
				rc = info->handle_unsol(aecp, now);
			}
		} else {
			rc = reply_locked(aecp, m, len);
		}
	}

	return rc;
}

int avb_aecp_aem_handle_timeouts(struct aecp *aecp, uint64_t now)
{
	size_t array_sz = ARRAY_SIZE(cmd_info);
	for (size_t index = 0; index < array_sz; index++) {
		if (!cmd_info[index].handle_unsol) {
			continue;
		}
		if ((cmd_info[index].handle_unsol(aecp, now))) {
			pw_log_error("unexpected failure in perdioc unsols %s\n",
						  cmd_info[index].name);
			spa_assert(0);
		}
	}

	return 0;
}

int avb_aecp_aem_handle_response(struct aecp *aecp, const void *m, int len)
{
#if 0
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_aem *p = SPA_PTROFF(h, sizeof(*h), void);
	uint16_t cmd_type;
	int rc = -1;
	const struct cmd_info *info;
	struct timespec ts_now;
	int64_t now;

	/**
	 * Time is always using the monotonic time
	 */
	if (clock_gettime(CLOCK_TAI, &ts_now)) {
		pw_log_error("while getting CLOCK_MONOTONIC time\n");
	}
	now = SPA_TIMESPEC_TO_NSEC(&ts_now);
#endif
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
