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

#ifndef __SPA_VIDEO_RAW_UTILS_H__
#define __SPA_VIDEO_RAW_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/type-map.h>
#include <spa/param/video/raw.h>

struct spa_type_video_format {
	uint32_t UNKNOWN;
	uint32_t ENCODED;
	uint32_t I420;
	uint32_t YV12;
	uint32_t YUY2;
	uint32_t UYVY;
	uint32_t AYUV;
	uint32_t RGBx;
	uint32_t BGRx;
	uint32_t xRGB;
	uint32_t xBGR;
	uint32_t RGBA;
	uint32_t BGRA;
	uint32_t ARGB;
	uint32_t ABGR;
	uint32_t RGB;
	uint32_t BGR;
	uint32_t Y41B;
	uint32_t Y42B;
	uint32_t YVYU;
	uint32_t Y444;
	uint32_t v210;
	uint32_t v216;
	uint32_t NV12;
	uint32_t NV21;
	uint32_t GRAY8;
	uint32_t GRAY16_BE;
	uint32_t GRAY16_LE;
	uint32_t v308;
	uint32_t RGB16;
	uint32_t BGR16;
	uint32_t RGB15;
	uint32_t BGR15;
	uint32_t UYVP;
	uint32_t A420;
	uint32_t RGB8P;
	uint32_t YUV9;
	uint32_t YVU9;
	uint32_t IYU1;
	uint32_t ARGB64;
	uint32_t AYUV64;
	uint32_t r210;
	uint32_t I420_10BE;
	uint32_t I420_10LE;
	uint32_t I422_10BE;
	uint32_t I422_10LE;
	uint32_t Y444_10BE;
	uint32_t Y444_10LE;
	uint32_t GBR;
	uint32_t GBR_10BE;
	uint32_t GBR_10LE;
	uint32_t NV16;
	uint32_t NV24;
	uint32_t NV12_64Z32;
	uint32_t A420_10BE;
	uint32_t A420_10LE;
	uint32_t A422_10BE;
	uint32_t A422_10LE;
	uint32_t A444_10BE;
	uint32_t A444_10LE;
	uint32_t NV61;
	uint32_t P010_10BE;
	uint32_t P010_10LE;
	uint32_t IYU2;
	uint32_t VYUY;
	uint32_t GBRA;
	uint32_t GBRA_10BE;
	uint32_t GBRA_10LE;
	uint32_t GBR_12BE;
	uint32_t GBR_12LE;
	uint32_t GBRA_12BE;
	uint32_t GBRA_12LE;
	uint32_t I420_12BE;
	uint32_t I420_12LE;
	uint32_t I422_12BE;
	uint32_t I422_12LE;
	uint32_t Y444_12BE;
	uint32_t Y444_12LE;
};

static inline void
spa_type_video_format_map(struct spa_type_map *map, struct spa_type_video_format *type)
{
	if (type->ENCODED == 0) {
		type->UNKNOWN = 0;
		type->ENCODED = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__ENCODED);
		type->I420 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I420);
		type->YV12 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__YV12);
		type->YUY2 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__YUY2);
		type->UYVY = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__UYVY);
		type->AYUV = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__AYUV);
		type->RGBx = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__RGBx);
		type->BGRx = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__BGRx);
		type->xRGB = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__xRGB);
		type->xBGR = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__xBGR);
		type->RGBA = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__RGBA);
		type->BGRA = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__BGRA);
		type->ARGB = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__ARGB);
		type->ABGR = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__ABGR);
		type->RGB = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__RGB);
		type->BGR = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__BGR);
		type->Y41B = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__Y41B);
		type->Y42B = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__Y42B);
		type->YVYU = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__YVYU);
		type->Y444 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__Y444);
		type->v210 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__v210);
		type->v216 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__v216);
		type->NV12 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__NV12);
		type->NV21 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__NV21);
		type->GRAY8 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GRAY8);
		type->GRAY16_BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GRAY16_BE);
		type->GRAY16_LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GRAY16_LE);
		type->v308 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__v308);
		type->RGB16 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__RGB16);
		type->BGR16 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__BGR16);
		type->RGB15 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__RGB15);
		type->BGR15 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__BGR15);
		type->UYVP = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__UYVP);
		type->A420 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__A420);
		type->RGB8P = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__RGB8P);
		type->YUV9 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__YUV9);
		type->YVU9 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__YVU9);
		type->IYU1 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__IYU1);
		type->ARGB64 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__ARGB64);
		type->AYUV64 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__AYUV64);
		type->r210 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__r210);
		type->I420_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I420_10BE);
		type->I420_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I420_10LE);
		type->I422_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I422_10BE);
		type->I422_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I422_10LE);
		type->Y444_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__Y444_10BE);
		type->Y444_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__Y444_10LE);
		type->GBR = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBR);
		type->GBR_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBR_10BE);
		type->GBR_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBR_10LE);
		type->NV16 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__NV16);
		type->NV24 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__NV24);
		type->NV12_64Z32 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__NV12_64Z32);
		type->A420_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__A420_10BE);
		type->A420_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__A420_10LE);
		type->A422_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__A422_10BE);
		type->A422_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__A422_10LE);
		type->A444_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__A444_10BE);
		type->A444_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__A444_10LE);
		type->NV61 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__NV61);
		type->P010_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__P010_10BE);
		type->P010_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__P010_10LE);
		type->IYU2 = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__IYU2);
		type->VYUY = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__VYUY);
		type->GBRA = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBRA);
		type->GBRA_10BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBRA_10BE);
		type->GBRA_10LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBRA_10LE);
		type->GBR_12BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBR_12BE);
		type->GBR_12LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBR_12LE);
		type->GBRA_12BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBRA_12BE);
		type->GBRA_12LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__GBRA_12LE);
		type->I420_12BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I420_12BE);
		type->I420_12LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I420_12LE);
		type->I422_12BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I422_12BE);
		type->I422_12LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__I422_12LE);
		type->Y444_12BE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__Y444_12BE);
		type->Y444_12LE = spa_type_map_get_id(map, SPA_TYPE_VIDEO_FORMAT__Y444_12LE);
	}
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_VIDEO_RAW_UTILS_H__ */
