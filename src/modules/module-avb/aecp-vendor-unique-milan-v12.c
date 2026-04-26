/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

#include <pipewire/pipewire.h>
#include <spa/utils/defs.h>

#include "aecp.h"
#include "aecp-vendor-unique-milan-v12.h"
#include "internal.h"

#include "aecp-vendor-unique-milan-v12-cmds-resps/cmd-resp-helpers.h"
#include "aecp-vendor-unique-milan-v12-cmds-resps/cmd-get-milan-info.h"

struct mvu_cmd_info {
	int (*handle_command) (struct aecp *aecp, int64_t now,
			const void *m, int len);
};

#define MVU_HANDLE_CMD(cmd, handle_exec)		\
	[cmd] = {					\
		.handle_command = handle_exec,		\
	}

static const char * const mvu_cmd_names[] = {
	[AVB_AECP_MVU_CMD_GET_MILAN_INFO] = "get-milan-info",
};

static const struct mvu_cmd_info mvu_cmd_info_milan_v12[] = {
	MVU_HANDLE_CMD(AVB_AECP_MVU_CMD_GET_MILAN_INFO,
			handle_cmd_mvu_get_milan_info_milan_v12),
};

static const uint8_t mvu_protocol_id[6] = {
	AVB_AECP_MVU_PROTOCOL_ID_0,
	AVB_AECP_MVU_PROTOCOL_ID_1,
	AVB_AECP_MVU_PROTOCOL_ID_2,
	AVB_AECP_MVU_PROTOCOL_ID_3,
	AVB_AECP_MVU_PROTOCOL_ID_4,
	AVB_AECP_MVU_PROTOCOL_ID_5,
};

int aecp_vendor_unique_milan_v12_handle_command(struct aecp *aecp,
		const void *m, int len)
{
	const struct avb_ethernet_header *h = m;
	const struct avb_packet_aecp_vendor_unique *vu;
	const struct mvu_cmd_info *info;
	uint16_t command_type, cmd;
	struct timespec ts_now = {0};
	int64_t now;

	if (len < (int)(sizeof(*h) + sizeof(*vu)))
		return 0;

	vu = SPA_PTROFF(h, sizeof(*h), const void);

	if (memcmp(vu->protocol_id, mvu_protocol_id,
			sizeof(mvu_protocol_id)) != 0)
		return 0;

	command_type = ntohs(vu->command_type);
	cmd = command_type & AVB_AECP_MVU_CMD_TYPE_CMD_MASK;

	pw_log_info("mvu command %s (r=%d)",
			cmd < SPA_N_ELEMENTS(mvu_cmd_names) && mvu_cmd_names[cmd]
				? mvu_cmd_names[cmd] : "unknown",
			(command_type & AVB_AECP_MVU_CMD_TYPE_R_FLAG_MASK) ? 1 : 0);

	if (cmd >= SPA_N_ELEMENTS(mvu_cmd_info_milan_v12) ||
	    mvu_cmd_info_milan_v12[cmd].handle_command == NULL) {
		(void)reply_mvu_not_implemented(aecp, m, len);
		return 1;
	}

	info = &mvu_cmd_info_milan_v12[cmd];

	if (clock_gettime(CLOCK_TAI, &ts_now))
		pw_log_warn("clock_gettime(CLOCK_TAI): %m");
	now = SPA_TIMESPEC_TO_NSEC(&ts_now);

	(void)info->handle_command(aecp, now, m, len);
	return 1;
}
