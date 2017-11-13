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

#ifndef __SPA_VIDEO_CHROMA_H__
#define __SPA_VIDEO_CHROMA_H__

#ifdef __cplusplus
extern "C" {
#endif

/** Various Chroma sitings.
 * @SPA_VIDEO_CHROMA_SITE_UNKNOWN: unknown cositing
 * @SPA_VIDEO_CHROMA_SITE_NONE: no cositing
 * @SPA_VIDEO_CHROMA_SITE_H_COSITED: chroma is horizontally cosited
 * @SPA_VIDEO_CHROMA_SITE_V_COSITED: chroma is vertically cosited
 * @SPA_VIDEO_CHROMA_SITE_ALT_LINE: choma samples are sited on alternate lines
 * @SPA_VIDEO_CHROMA_SITE_COSITED: chroma samples cosited with luma samples
 * @SPA_VIDEO_CHROMA_SITE_JPEG: jpeg style cositing, also for mpeg1 and mjpeg
 * @SPA_VIDEO_CHROMA_SITE_MPEG2: mpeg2 style cositing
 * @SPA_VIDEO_CHROMA_SITE_DV: DV style cositing
 */
enum spa_video_chroma_site {
	SPA_VIDEO_CHROMA_SITE_UNKNOWN = 0,
	SPA_VIDEO_CHROMA_SITE_NONE = (1 << 0),
	SPA_VIDEO_CHROMA_SITE_H_COSITED = (1 << 1),
	SPA_VIDEO_CHROMA_SITE_V_COSITED = (1 << 2),
	SPA_VIDEO_CHROMA_SITE_ALT_LINE = (1 << 3),
	/* some common chroma cositing */
	SPA_VIDEO_CHROMA_SITE_COSITED = (SPA_VIDEO_CHROMA_SITE_H_COSITED | SPA_VIDEO_CHROMA_SITE_V_COSITED),
	SPA_VIDEO_CHROMA_SITE_JPEG = (SPA_VIDEO_CHROMA_SITE_NONE),
	SPA_VIDEO_CHROMA_SITE_MPEG2 = (SPA_VIDEO_CHROMA_SITE_H_COSITED),
	SPA_VIDEO_CHROMA_SITE_DV = (SPA_VIDEO_CHROMA_SITE_COSITED | SPA_VIDEO_CHROMA_SITE_ALT_LINE),
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_VIDEO_CHROMA_H__ */
