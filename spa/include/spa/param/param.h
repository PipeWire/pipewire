/* Simple Plugin API
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_PARAM_H__
#define __SPA_PARAM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

/** different parameter types that can be queried */
enum spa_param_type {
	SPA_PARAM_Invalid,		/**< invalid */
	SPA_PARAM_List,			/**< available params */
	SPA_PARAM_PropInfo,		/**< property information */
	SPA_PARAM_Props,		/**< properties */
	SPA_PARAM_EnumFormat,		/**< available formats */
	SPA_PARAM_Format,		/**< configured format */
	SPA_PARAM_Buffers,		/**< buffer configurations */
	SPA_PARAM_Meta,			/**< allowed metadata for buffers */
	SPA_PARAM_IO,			/**< configurable IO areas */
	SPA_PARAM_Profile,		/**< port profile configuration */
};

/** Properties for SPA_TYPE_OBJECT_ParamList */
enum spa_param_list {
	SPA_PARAM_LIST_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_LIST_id,	/**< id of the supported list param */
};

/** properties for SPA_TYPE_OBJECT_ParamBuffers */
enum spa_param_buffers {
	SPA_PARAM_BUFFERS_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_BUFFERS_buffers,	/**< number of buffers */
	SPA_PARAM_BUFFERS_blocks,	/**< number of data blocks per buffer */
	SPA_PARAM_BUFFERS_size,		/**< size of a data block memory */
	SPA_PARAM_BUFFERS_stride,	/**< stride of data block memory */
	SPA_PARAM_BUFFERS_align,	/**< alignment of data block memory */
};

/** properties for SPA_TYPE_OBJECT_ParamMeta */
enum spa_param_meta {
	SPA_PARAM_META_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_META_type,	/**< the metadata, one of enum spa_meta_type */
	SPA_PARAM_META_size,	/**< the expected maximum size the meta */
};

/** properties for SPA_TYPE_OBJECT_ParamIO */
enum spa_param_io {
	SPA_PARAM_IO_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_IO_id,	/**< type ID, uniquely identifies the io area */
	SPA_PARAM_IO_size,	/**< size of the io area */
};

/** properties for SPA_TYPE_OBJECT_ParamProfile */
enum spa_param_profile {
	SPA_PARAM_PROFILE_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_PROFILE_direction,	/**< direction, input/output */
	SPA_PARAM_PROFILE_format,	/**< profile format specification */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_H__ */
