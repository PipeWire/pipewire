/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_PARAM_VIDEO_PADDING_H__
#define __SPA_PARAM_VIDEO_PADDING_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/support/type-map.h>

#define SPA_TYPE_PARAM__VideoPadding		SPA_TYPE_PARAM_BASE "VideoPadding"
#define SPA_TYPE_PARAM_VIDEO_PADDING_BASE	SPA_TYPE_PARAM__VideoPadding ":"

#define SPA_TYPE_PARAM_VIDEO_PADDING__top		SPA_TYPE_PARAM_VIDEO_PADDING_BASE "top"
#define SPA_TYPE_PARAM_VIDEO_PADDING__bottom		SPA_TYPE_PARAM_VIDEO_PADDING_BASE "bottom"
#define SPA_TYPE_PARAM_VIDEO_PADDING__left		SPA_TYPE_PARAM_VIDEO_PADDING_BASE "left"
#define SPA_TYPE_PARAM_VIDEO_PADDING__right		SPA_TYPE_PARAM_VIDEO_PADDING_BASE "right"
#define SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign0	SPA_TYPE_PARAM_VIDEO_PADDING_BASE "strideAlign0"
#define SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign1	SPA_TYPE_PARAM_VIDEO_PADDING_BASE "strideAlign1"
#define SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign2	SPA_TYPE_PARAM_VIDEO_PADDING_BASE "strideAlign2"
#define SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign3	SPA_TYPE_PARAM_VIDEO_PADDING_BASE "strideAlign3"

struct spa_type_param_video_padding {
	uint32_t VideoPadding;
	uint32_t top;
	uint32_t bottom;
	uint32_t left;
	uint32_t right;
	uint32_t strideAlign[4];
};

static inline void
spa_type_param_video_padding_map(struct spa_type_map *map,
				 struct spa_type_param_video_padding *type)
{
	if (type->VideoPadding == 0) {
		type->VideoPadding = spa_type_map_get_id(map, SPA_TYPE_PARAM__VideoPadding);
		type->top = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__top);
		type->bottom = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__bottom);
		type->left = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__left);
		type->right = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__right);
		type->strideAlign[0] = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign0);
		type->strideAlign[1] = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign1);
		type->strideAlign[2] = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign2);
		type->strideAlign[3] = spa_type_map_get_id(map, SPA_TYPE_PARAM_VIDEO_PADDING__strideAlign3);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_VIDEO_PADDING_H__ */
