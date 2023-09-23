/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
/* SPDX-License-Identifier: MIT */

#include "vulkan-types.h"
#include "spa/pod/builder.h"

bool find_EnumFormatInfo(struct vulkan_format_infos *fmtInfos, uint32_t index, uint32_t caps, uint32_t *fmt_idx, bool *has_modifier);

struct spa_pod *build_dsp_EnumFormat(const struct vulkan_format_info *fmt, bool with_modifiers, struct spa_pod_builder *builder);
struct spa_pod *build_raw_EnumFormat(const struct vulkan_format_info *fmt, bool with_modifiers, struct spa_pod_builder *builder);
