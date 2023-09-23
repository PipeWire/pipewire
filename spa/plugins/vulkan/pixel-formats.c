/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
/* SPDX-License-Identifier: MIT */

#include "pixel-formats.h"

#include <spa/utils/defs.h>
#include <spa/param/video/raw.h>

struct pixel_info {
	uint32_t format;
	uint32_t bpp;
} pixel_infos[] = {
	{ SPA_VIDEO_FORMAT_RGBA_F32, 16 },
	{ SPA_VIDEO_FORMAT_BGRA, 4 },
	{ SPA_VIDEO_FORMAT_RGBA, 4 },
	{ SPA_VIDEO_FORMAT_BGRx, 4 },
	{ SPA_VIDEO_FORMAT_RGBx, 4 },
	{ SPA_VIDEO_FORMAT_BGR, 3 },
	{ SPA_VIDEO_FORMAT_RGB, 3 },
};

bool get_pixel_format_info(uint32_t format, struct pixel_format_info *info)
{
	struct pixel_info *p;
	SPA_FOR_EACH_ELEMENT(pixel_infos, p) {
		if (p->format != format)
			continue;
		info->bpp = p->bpp;
		return true;
	}
	return false;
}
