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

#ifndef __SPA_VIDEO_RAW_H__
#define __SPA_VIDEO_RAW_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaVideoInfoRaw SpaVideoInfoRaw;

#include <spa/props.h>
#include <spa/id-map.h>
#include <spa/video/chroma.h>
#include <spa/video/color.h>
#include <spa/video/multiview.h>

#define SPA_VIDEO_MAX_PLANES 4
#define SPA_VIDEO_MAX_COMPONENTS 4

#define SPA_VIDEO_FORMAT_URI           "http://spaplug.in/ns/video-format"
#define SPA_VIDEO_FORMAT_PREFIX        SPA_VIDEO_FORMAT_URI "#"

#define SPA_VIDEO_FORMAT__ENCODED        SPA_VIDEO_FORMAT_PREFIX "encoded"
#define SPA_VIDEO_FORMAT__I420           SPA_VIDEO_FORMAT_PREFIX "I420"
#define SPA_VIDEO_FORMAT__YV12           SPA_VIDEO_FORMAT_PREFIX "YV12"
#define SPA_VIDEO_FORMAT__YUY2           SPA_VIDEO_FORMAT_PREFIX "YUY2"
#define SPA_VIDEO_FORMAT__UYVY           SPA_VIDEO_FORMAT_PREFIX "UYVY"
#define SPA_VIDEO_FORMAT__AYUV           SPA_VIDEO_FORMAT_PREFIX "AYUV"
#define SPA_VIDEO_FORMAT__RGBx           SPA_VIDEO_FORMAT_PREFIX "RGBx"
#define SPA_VIDEO_FORMAT__BGRx           SPA_VIDEO_FORMAT_PREFIX "BGRx"
#define SPA_VIDEO_FORMAT__xRGB           SPA_VIDEO_FORMAT_PREFIX "xRGB"
#define SPA_VIDEO_FORMAT__xBGR           SPA_VIDEO_FORMAT_PREFIX "xBGR"
#define SPA_VIDEO_FORMAT__RGBA           SPA_VIDEO_FORMAT_PREFIX "RGBA"
#define SPA_VIDEO_FORMAT__BGRA           SPA_VIDEO_FORMAT_PREFIX "BGRA"
#define SPA_VIDEO_FORMAT__ARGB           SPA_VIDEO_FORMAT_PREFIX "ARGB"
#define SPA_VIDEO_FORMAT__ABGR           SPA_VIDEO_FORMAT_PREFIX "ABGR"
#define SPA_VIDEO_FORMAT__RGB            SPA_VIDEO_FORMAT_PREFIX "RGB"
#define SPA_VIDEO_FORMAT__BGR            SPA_VIDEO_FORMAT_PREFIX "BGR"
#define SPA_VIDEO_FORMAT__Y41B           SPA_VIDEO_FORMAT_PREFIX "Y41B"
#define SPA_VIDEO_FORMAT__Y42B           SPA_VIDEO_FORMAT_PREFIX "Y42B"
#define SPA_VIDEO_FORMAT__YVYU           SPA_VIDEO_FORMAT_PREFIX "YVYU"
#define SPA_VIDEO_FORMAT__Y444           SPA_VIDEO_FORMAT_PREFIX "Y444"
#define SPA_VIDEO_FORMAT__v210           SPA_VIDEO_FORMAT_PREFIX "v210"
#define SPA_VIDEO_FORMAT__v216           SPA_VIDEO_FORMAT_PREFIX "v216"
#define SPA_VIDEO_FORMAT__NV12           SPA_VIDEO_FORMAT_PREFIX "NV12"
#define SPA_VIDEO_FORMAT__NV21           SPA_VIDEO_FORMAT_PREFIX "NV21"
#define SPA_VIDEO_FORMAT__GRAY8          SPA_VIDEO_FORMAT_PREFIX "GRAY8"
#define SPA_VIDEO_FORMAT__GRAY16_BE      SPA_VIDEO_FORMAT_PREFIX "GRAY16_BE"
#define SPA_VIDEO_FORMAT__GRAY16_LE      SPA_VIDEO_FORMAT_PREFIX "GRAY16_LE"
#define SPA_VIDEO_FORMAT__v308           SPA_VIDEO_FORMAT_PREFIX "v308"
#define SPA_VIDEO_FORMAT__RGB16          SPA_VIDEO_FORMAT_PREFIX "RGB16"
#define SPA_VIDEO_FORMAT__BGR16          SPA_VIDEO_FORMAT_PREFIX "BGR16"
#define SPA_VIDEO_FORMAT__RGB15          SPA_VIDEO_FORMAT_PREFIX "RGB15"
#define SPA_VIDEO_FORMAT__BGR15          SPA_VIDEO_FORMAT_PREFIX "BGR15"
#define SPA_VIDEO_FORMAT__UYVP           SPA_VIDEO_FORMAT_PREFIX "UYVP"
#define SPA_VIDEO_FORMAT__A420           SPA_VIDEO_FORMAT_PREFIX "A420"
#define SPA_VIDEO_FORMAT__RGB8P          SPA_VIDEO_FORMAT_PREFIX "RGB8P"
#define SPA_VIDEO_FORMAT__YUV9           SPA_VIDEO_FORMAT_PREFIX "YUV9"
#define SPA_VIDEO_FORMAT__YVU9           SPA_VIDEO_FORMAT_PREFIX "YVU9"
#define SPA_VIDEO_FORMAT__IYU1           SPA_VIDEO_FORMAT_PREFIX "IYU1"
#define SPA_VIDEO_FORMAT__ARGB64         SPA_VIDEO_FORMAT_PREFIX "ARGB64"
#define SPA_VIDEO_FORMAT__AYUV64         SPA_VIDEO_FORMAT_PREFIX "AYUV64"
#define SPA_VIDEO_FORMAT__r210           SPA_VIDEO_FORMAT_PREFIX "r210"
#define SPA_VIDEO_FORMAT__I420_10BE      SPA_VIDEO_FORMAT_PREFIX "I420_10BE"
#define SPA_VIDEO_FORMAT__I420_10LE      SPA_VIDEO_FORMAT_PREFIX "I420_10LE"
#define SPA_VIDEO_FORMAT__I422_10BE      SPA_VIDEO_FORMAT_PREFIX "I422_10BE"
#define SPA_VIDEO_FORMAT__I422_10LE      SPA_VIDEO_FORMAT_PREFIX "I422_10LE"
#define SPA_VIDEO_FORMAT__Y444_10BE      SPA_VIDEO_FORMAT_PREFIX "Y444_10BE"
#define SPA_VIDEO_FORMAT__Y444_10LE      SPA_VIDEO_FORMAT_PREFIX "Y444_10LE"
#define SPA_VIDEO_FORMAT__GBR            SPA_VIDEO_FORMAT_PREFIX "GBR"
#define SPA_VIDEO_FORMAT__GBR_10BE       SPA_VIDEO_FORMAT_PREFIX "GBR_10BE"
#define SPA_VIDEO_FORMAT__GBR_10LE       SPA_VIDEO_FORMAT_PREFIX "GBR_10LE"
#define SPA_VIDEO_FORMAT__NV16           SPA_VIDEO_FORMAT_PREFIX "NV16"
#define SPA_VIDEO_FORMAT__NV24           SPA_VIDEO_FORMAT_PREFIX "NV24"
#define SPA_VIDEO_FORMAT__NV12_64Z32     SPA_VIDEO_FORMAT_PREFIX "NV12_64Z32"
#define SPA_VIDEO_FORMAT__A420_10BE      SPA_VIDEO_FORMAT_PREFIX "A420_10BE"
#define SPA_VIDEO_FORMAT__A420_10LE      SPA_VIDEO_FORMAT_PREFIX "A420_10LE"
#define SPA_VIDEO_FORMAT__A422_10BE      SPA_VIDEO_FORMAT_PREFIX "A422_10BE"
#define SPA_VIDEO_FORMAT__A422_10LE      SPA_VIDEO_FORMAT_PREFIX "A422_10LE"
#define SPA_VIDEO_FORMAT__A444_10BE      SPA_VIDEO_FORMAT_PREFIX "A444_10BE"
#define SPA_VIDEO_FORMAT__A444_10LE      SPA_VIDEO_FORMAT_PREFIX "A444_10LE"
#define SPA_VIDEO_FORMAT__NV61           SPA_VIDEO_FORMAT_PREFIX "NV61"
#define SPA_VIDEO_FORMAT__P010_10BE      SPA_VIDEO_FORMAT_PREFIX "P010_10BE"
#define SPA_VIDEO_FORMAT__P010_10LE      SPA_VIDEO_FORMAT_PREFIX "P010_10LE"
#define SPA_VIDEO_FORMAT__IYU2           SPA_VIDEO_FORMAT_PREFIX "IYU2"
#define SPA_VIDEO_FORMAT__VYUY           SPA_VIDEO_FORMAT_PREFIX "VYUY"

/**
 * SpaVideoFlags:
 * @SPA_VIDEO_FLAG_NONE: no flags
 * @SPA_VIDEO_FLAG_VARIABLE_FPS: a variable fps is selected, fps_n and fps_d
 *     denote the maximum fps of the video
 * @SPA_VIDEO_FLAG_PREMULTIPLIED_ALPHA: Each color has been scaled by the alpha
 *     value.
 *
 * Extra video flags
 */
typedef enum {
  SPA_VIDEO_FLAG_NONE                = 0,
  SPA_VIDEO_FLAG_VARIABLE_FPS        = (1 << 0),
  SPA_VIDEO_FLAG_PREMULTIPLIED_ALPHA = (1 << 1)
} SpaVideoFlags;

/**
 * SpaVideoInterlaceMode:
 * @SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE: all frames are progressive
 * @SPA_VIDEO_INTERLACE_MODE_INTERLEAVED: 2 fields are interleaved in one video
 *     frame. Extra buffer flags describe the field order.
 * @SPA_VIDEO_INTERLACE_MODE_MIXED: frames contains both interlaced and
 *     progressive video, the buffer flags describe the frame and fields.
 * @SPA_VIDEO_INTERLACE_MODE_FIELDS: 2 fields are stored in one buffer, use the
 *     frame ID to get access to the required field. For multiview (the
 *     'views' property > 1) the fields of view N can be found at frame ID
 *     (N * 2) and (N * 2) + 1.
 *     Each field has only half the amount of lines as noted in the
 *     height property. This mode requires multiple SpaVideoMeta metadata
 *     to describe the fields.
 *
 * The possible values of the #SpaVideoInterlaceMode describing the interlace
 * mode of the stream.
 */
typedef enum {
  SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE = 0,
  SPA_VIDEO_INTERLACE_MODE_INTERLEAVED,
  SPA_VIDEO_INTERLACE_MODE_MIXED,
  SPA_VIDEO_INTERLACE_MODE_FIELDS
} SpaVideoInterlaceMode;

/**
 * SpaVideoInfoRaw:
 * @format: the format
 * @size: the frame size of the video
 * @framerate: the framerate of the video 0/1 means variable rate
 * @max_framerate: the maximum framerate of the video. This is only valid when
 *             @framerate is 0/1
 * @views: the number of views in this video
 * @interlace_mode: the interlace mode
 * @pixel_aspect_ratio: The pixel aspect ratio
 * @multiview_mode: multiview mode
 * @multiview_flags: multiview flags
 * @chroma_site: the chroma siting
 * @color_range: the color range. This is the valid range for the samples.
 *         It is used to convert the samples to Y'PbPr values.
 * @color_matrix: the color matrix. Used to convert between Y'PbPr and
 *         non-linear RGB (R'G'B')
 * @transfer_function: the transfer function. used to convert between R'G'B' and RGB
 * @color_primaries: color primaries. used to convert between R'G'B' and CIE XYZ
 */
struct _SpaVideoInfoRaw {
  uint32_t                  format;
  SpaRectangle              size;
  SpaFraction               framerate;
  SpaFraction               max_framerate;
  uint32_t                  views;
  SpaVideoInterlaceMode     interlace_mode;
  SpaFraction               pixel_aspect_ratio;
  SpaVideoMultiviewMode     multiview_mode;
  SpaVideoMultiviewFlags    multiview_flags;
  SpaVideoChromaSite        chroma_site;
  SpaVideoColorRange        color_range;
  SpaVideoColorMatrix       color_matrix;
  SpaVideoTransferFunction  transfer_function;
  SpaVideoColorPrimaries    color_primaries;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_VIDEO_RAW_H__ */
