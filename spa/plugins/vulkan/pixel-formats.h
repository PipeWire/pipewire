/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <stdbool.h>

struct pixel_format_info {
	uint32_t bpp;	// bytes per pixel
};

bool get_pixel_format_info(uint32_t format, struct pixel_format_info *info);
