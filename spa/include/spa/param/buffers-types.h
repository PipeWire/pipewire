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

#ifndef __SPA_PARAM_BUFFERS_TYPES_H__
#define __SPA_PARAM_BUFFERS_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param-types.h>

#define SPA_TYPE_PARAM__Buffers			SPA_TYPE_PARAM_BASE "Buffers"
#define SPA_TYPE_PARAM_BUFFERS_BASE		SPA_TYPE_PARAM__Buffers ":"

#define SPA_TYPE_PARAM_BUFFERS__buffers		SPA_TYPE_PARAM_BUFFERS_BASE "buffers"
#define SPA_TYPE_PARAM_BUFFERS__blocks		SPA_TYPE_PARAM_BUFFERS_BASE "blocks"

#define SPA_TYPE_PARAM__BlockInfo		SPA_TYPE_PARAM_BASE "BlockInfo"
#define SPA_TYPE_PARAM_BLOCK_INFO_BASE		SPA_TYPE_PARAM__BlockInfo ":"

static const struct spa_type_info spa_type_param_buffers_items[] = {
	{ SPA_PARAM_BUFFERS_buffers, SPA_TYPE_PARAM_BUFFERS_BASE "buffers",  SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_blocks,  SPA_TYPE_PARAM_BUFFERS_BASE "blocks",   SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_size,    SPA_TYPE_PARAM_BLOCK_INFO_BASE "size",   SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_stride,  SPA_TYPE_PARAM_BLOCK_INFO_BASE "stride", SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_align,   SPA_TYPE_PARAM_BLOCK_INFO_BASE "align",  SPA_ID_Int, },
	{ 0, NULL, },
};

static const struct spa_type_info spa_type_param_buffers[] = {
	{ SPA_ID_OBJECT_ParamBuffers, SPA_TYPE_PARAM__Buffers,  SPA_ID_Object,
		spa_type_param_buffers_items },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_BUFFERS_TYPES_H__ */
