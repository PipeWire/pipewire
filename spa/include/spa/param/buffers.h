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

#ifndef __SPA_PARAM_BUFFERS_H__
#define __SPA_PARAM_BUFFERS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

/** properties for SPA_ID_OBJECT_ParamBuffers */
enum spa_param_buffers {
	SPA_PARAM_BUFFERS_buffers,	/**< number of buffers */
	SPA_PARAM_BUFFERS_blocks,	/**< number of data blocks per buffer */
	SPA_PARAM_BUFFERS_size,		/**< size of a data block memory */
	SPA_PARAM_BUFFERS_stride,	/**< stride of data block memory */
	SPA_PARAM_BUFFERS_align,	/**< alignment of data block memory */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_BUFFERS_H__ */
