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

#ifndef SPA_PARAM_H
#define SPA_PARAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

/** different parameter types that can be queried */
enum spa_param_type {
	SPA_PARAM_Invalid,		/**< invalid */
	SPA_PARAM_PropInfo,		/**< property information as SPA_TYPE_OBJECT_PropInfo */
	SPA_PARAM_Props,		/**< properties as SPA_TYPE_OBJECT_Props */
	SPA_PARAM_EnumFormat,		/**< available formats as SPA_TYPE_OBJECT_Format */
	SPA_PARAM_Format,		/**< configured format as SPA_TYPE_OBJECT_Format */
	SPA_PARAM_Buffers,		/**< buffer configurations as SPA_TYPE_OBJECT_ParamBuffers*/
	SPA_PARAM_Meta,			/**< allowed metadata for buffers as SPA_TYPE_OBJECT_ParamMeta*/
	SPA_PARAM_IO,			/**< configurable IO areas as SPA_TYPE_OBJECT_ParamIO */
	SPA_PARAM_EnumProfile,		/**< profile enumeration as SPA_TYPE_OBJECT_ParamProfile */
	SPA_PARAM_Profile,		/**< profile configuration as SPA_TYPE_OBJECT_ParamProfile */
};

/** Properties for SPA_TYPE_OBJECT_ParamList */
enum spa_param_list {
	SPA_PARAM_LIST_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_LIST_id,	/**< id of the supported list param (Id enum spa_param_type) */
};

/** properties for SPA_TYPE_OBJECT_ParamBuffers */
enum spa_param_buffers {
	SPA_PARAM_BUFFERS_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_BUFFERS_buffers,	/**< number of buffers (Int) */
	SPA_PARAM_BUFFERS_blocks,	/**< number of data blocks per buffer (Int) */
	SPA_PARAM_BUFFERS_size,		/**< size of a data block memory (Int)*/
	SPA_PARAM_BUFFERS_stride,	/**< stride of data block memory (Int) */
	SPA_PARAM_BUFFERS_align,	/**< alignment of data block memory (Int) */
};

/** properties for SPA_TYPE_OBJECT_ParamMeta */
enum spa_param_meta {
	SPA_PARAM_META_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_META_type,	/**< the metadata, one of enum spa_meta_type (Id enum spa_meta_type) */
	SPA_PARAM_META_size,	/**< the expected maximum size the meta (Int) */
};

/** properties for SPA_TYPE_OBJECT_ParamIO */
enum spa_param_io {
	SPA_PARAM_IO_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_IO_id,	/**< type ID, uniquely identifies the io area (Id enum spa_io_type) */
	SPA_PARAM_IO_size,	/**< size of the io area (Int) */
};

/** properties for SPA_TYPE_OBJECT_ParamProfile */
enum spa_param_profile {
	SPA_PARAM_PROFILE_START,	/**< object id, one of enum spa_param_type */
	SPA_PARAM_PROFILE_index,	/**< profile index (Int) */
	SPA_PARAM_PROFILE_name,		/**< profile name (String) */
	SPA_PARAM_PROFILE_direction,	/**< direction, input/output (Id enum spa_direction) */
	SPA_PARAM_PROFILE_format,	/**< profile format specification (Object) */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_H */
