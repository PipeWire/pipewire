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

#if 0
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
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_VIDEO_PADDING_H__ */
