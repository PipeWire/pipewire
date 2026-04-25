/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki */
/* SPDX-License-Identifier: MIT */

#ifndef __AVB_AECP_AEM_CMD_AUDIO_MAPPINGS_H__
#define __AVB_AECP_AEM_CMD_AUDIO_MAPPINGS_H__

#include "../aecp-aem.h"

int handle_cmd_get_audio_map_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

int handle_cmd_add_audio_mappings_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

int handle_cmd_remove_audio_mappings_milan_v12(struct aecp *aecp, int64_t now,
	const void *m, int len);

#endif /* __AVB_AECP_AEM_CMD_AUDIO_MAPPINGS_H__ */
