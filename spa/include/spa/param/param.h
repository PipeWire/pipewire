/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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
