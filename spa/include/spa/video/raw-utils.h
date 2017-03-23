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

typedef struct _SpaVideoInfoRaw SpaVideoInfoRaw;

#include <spa/id-map.h>
#include <spa/video/raw.h>

typedef struct
{
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
} SpaVideoFormats;

static inline void
spa_video_formats_map (SpaIDMap *map, SpaVideoFormats *types)
{
  if (types->ENCODED == 0) {
    types->UNKNOWN      = 0;
    types->ENCODED      = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__ENCODED);
    types->I420         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__I420);
    types->YV12         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__YV12);
    types->YUY2         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__YUY2);
    types->UYVY         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__UYVY);
    types->AYUV         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__AYUV);
    types->RGBx         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__RGBx);
    types->BGRx         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__BGRx);
    types->xRGB         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__xRGB);
    types->xBGR         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__xBGR);
    types->RGBA         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__RGBA);
    types->BGRA         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__BGRA);
    types->ARGB         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__ARGB);
    types->ABGR         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__ABGR);
    types->RGB          = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__RGB);
    types->BGR          = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__BGR);
    types->Y41B         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__Y41B);
    types->Y42B         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__Y42B);
    types->YVYU         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__YVYU);
    types->Y444         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__Y444);
    types->v210         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__v210);
    types->v216         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__v216);
    types->NV12         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__NV12);
    types->NV21         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__NV21);
    types->GRAY8        = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__GRAY8);
    types->GRAY16_BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__GRAY16_BE);
    types->GRAY16_LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__GRAY16_LE);
    types->v308         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__v308);
    types->RGB16        = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__RGB16);
    types->BGR16        = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__BGR16);
    types->RGB15        = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__RGB15);
    types->BGR15        = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__BGR15);
    types->UYVP         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__UYVP);
    types->A420         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__A420);
    types->RGB8P        = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__RGB8P);
    types->YUV9         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__YUV9);
    types->YVU9         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__YVU9);
    types->IYU1         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__IYU1);
    types->ARGB64       = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__ARGB64);
    types->AYUV64       = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__AYUV64);
    types->r210         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__r210);
    types->I420_10BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__I420_10BE);
    types->I420_10LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__I420_10LE);
    types->I422_10BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__I422_10BE);
    types->I422_10LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__I422_10LE);
    types->Y444_10BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__Y444_10BE);
    types->Y444_10LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__Y444_10LE);
    types->GBR          = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__GBR);
    types->GBR_10BE     = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__GBR_10BE);
    types->GBR_10LE     = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__GBR_10LE);
    types->NV16         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__NV16);
    types->NV24         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__NV24);
    types->NV12_64Z32   = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__NV12_64Z32);
    types->A420_10BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__A420_10BE);
    types->A420_10LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__A420_10LE);
    types->A422_10BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__A422_10BE);
    types->A422_10LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__A422_10LE);
    types->A444_10BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__A444_10BE);
    types->A444_10LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__A444_10LE);
    types->NV61         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__NV61);
    types->P010_10BE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__P010_10BE);
    types->P010_10LE    = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__P010_10LE);
    types->IYU2         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__IYU2);
    types->VYUY         = spa_id_map_get_id (map, SPA_TYPE_VIDEO_FORMAT__VYUY);
  }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_VIDEO_RAW_UTILS_H__ */
