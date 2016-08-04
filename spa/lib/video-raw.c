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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/video/raw.h>
#include <spa/video/format.h>

static const SpaVideoRawInfo default_info = {
  SPA_VIDEO_FORMAT_UNKNOWN,
  { 320, 240 },
  { 0, 1 },
  { 0, 1 },
  1,
  SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE,
  { 1, 1},
  SPA_VIDEO_MULTIVIEW_MODE_MONO,
  SPA_VIDEO_MULTIVIEW_FLAGS_NONE,
  SPA_VIDEO_CHROMA_SITE_UNKNOWN,
  SPA_VIDEO_COLOR_RANGE_UNKNOWN,
  SPA_VIDEO_COLOR_MATRIX_UNKNOWN,
  SPA_VIDEO_TRANSFER_UNKNOWN,
  SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN
};

static const uint32_t format_values[] = {
  SPA_VIDEO_FORMAT_UNKNOWN,
  SPA_VIDEO_FORMAT_ENCODED,
  SPA_VIDEO_FORMAT_I420,
  SPA_VIDEO_FORMAT_YV12,
  SPA_VIDEO_FORMAT_YUY2,
  SPA_VIDEO_FORMAT_UYVY,
  SPA_VIDEO_FORMAT_AYUV,
  SPA_VIDEO_FORMAT_RGBx,
  SPA_VIDEO_FORMAT_BGRx,
  SPA_VIDEO_FORMAT_xRGB,
  SPA_VIDEO_FORMAT_xBGR,
  SPA_VIDEO_FORMAT_RGBA,
  SPA_VIDEO_FORMAT_BGRA,
  SPA_VIDEO_FORMAT_ARGB,
  SPA_VIDEO_FORMAT_ABGR,
  SPA_VIDEO_FORMAT_RGB,
  SPA_VIDEO_FORMAT_BGR,
  SPA_VIDEO_FORMAT_Y41B,
  SPA_VIDEO_FORMAT_Y42B,
  SPA_VIDEO_FORMAT_YVYU,
  SPA_VIDEO_FORMAT_Y444,
  SPA_VIDEO_FORMAT_v210,
  SPA_VIDEO_FORMAT_v216,
  SPA_VIDEO_FORMAT_NV12,
  SPA_VIDEO_FORMAT_NV21,
  SPA_VIDEO_FORMAT_GRAY8,
  SPA_VIDEO_FORMAT_GRAY16_BE,
  SPA_VIDEO_FORMAT_GRAY16_LE,
  SPA_VIDEO_FORMAT_v308,
  SPA_VIDEO_FORMAT_RGB16,
  SPA_VIDEO_FORMAT_BGR16,
  SPA_VIDEO_FORMAT_RGB15,
  SPA_VIDEO_FORMAT_BGR15,
  SPA_VIDEO_FORMAT_UYVP,
  SPA_VIDEO_FORMAT_A420,
  SPA_VIDEO_FORMAT_RGB8P,
  SPA_VIDEO_FORMAT_YUV9,
  SPA_VIDEO_FORMAT_YVU9,
  SPA_VIDEO_FORMAT_IYU1,
  SPA_VIDEO_FORMAT_ARGB64,
  SPA_VIDEO_FORMAT_AYUV64,
  SPA_VIDEO_FORMAT_r210,
  SPA_VIDEO_FORMAT_I420_10BE,
  SPA_VIDEO_FORMAT_I420_10LE,
  SPA_VIDEO_FORMAT_I422_10BE,
  SPA_VIDEO_FORMAT_I422_10LE,
  SPA_VIDEO_FORMAT_Y444_10BE,
  SPA_VIDEO_FORMAT_Y444_10LE,
  SPA_VIDEO_FORMAT_GBR,
  SPA_VIDEO_FORMAT_GBR_10BE,
  SPA_VIDEO_FORMAT_GBR_10LE,
  SPA_VIDEO_FORMAT_NV16,
  SPA_VIDEO_FORMAT_NV24,
  SPA_VIDEO_FORMAT_NV12_64Z32,
  SPA_VIDEO_FORMAT_A420_10BE,
  SPA_VIDEO_FORMAT_A420_10LE,
  SPA_VIDEO_FORMAT_A422_10BE,
  SPA_VIDEO_FORMAT_A422_10LE,
  SPA_VIDEO_FORMAT_A444_10BE,
  SPA_VIDEO_FORMAT_A444_10LE,
  SPA_VIDEO_FORMAT_NV61,
  SPA_VIDEO_FORMAT_P010_10BE,
  SPA_VIDEO_FORMAT_P010_10LE,
  SPA_VIDEO_FORMAT_IYU2,
};

static const SpaPropRangeInfo format_range[] = {
  { "ENCODED,", "ENCODED", sizeof (uint32_t), &format_values[1] },
  { "I420", "I420", sizeof (uint32_t), &format_values[2] },
  { "YV12", "YV12", sizeof (uint32_t), &format_values[3] },
  { "YUY2", "YUY2", sizeof (uint32_t), &format_values[4] },
  { "UYVY", "UYVY", sizeof (uint32_t), &format_values[5] },
  { "AYUV", "AYUV", sizeof (uint32_t), &format_values[6] },
  { "S24_32BE", "S24_32BE", sizeof (uint32_t), &format_values[7] },
  { "U24_32LE", "U24_32LE", sizeof (uint32_t), &format_values[8] },
  { "U24_32BE", "U24_32BE", sizeof (uint32_t), &format_values[9] },
  { "S32LE", "S32LE", sizeof (uint32_t), &format_values[10] },
  { "S32BE", "S32BE", sizeof (uint32_t), &format_values[11] },
  { "U32LE", "U32LE", sizeof (uint32_t), &format_values[12] },
  { "U32BE", "U32BE", sizeof (uint32_t), &format_values[13] },
  { "S24LE", "S24LE", sizeof (uint32_t), &format_values[14] },
  { "S24BE", "S24BE", sizeof (uint32_t), &format_values[15] },
  { "U24LE", "U24LE", sizeof (uint32_t), &format_values[16] },
  { "U24BE", "U24BE", sizeof (uint32_t), &format_values[17] },
  { "S20LE", "S20LE", sizeof (uint32_t), &format_values[18] },
  { "S20BE", "S20BE", sizeof (uint32_t), &format_values[19] },
  { "U20LE", "U20LE", sizeof (uint32_t), &format_values[20] },
  { "U20BE", "U20BE", sizeof (uint32_t), &format_values[21] },
  { "S18LE", "S18LE", sizeof (uint32_t), &format_values[22] },
  { "S18BE", "S18BE", sizeof (uint32_t), &format_values[23] },
  { "U18LE", "U18LE", sizeof (uint32_t), &format_values[24] },
  { "U18BE", "U18BE", sizeof (uint32_t), &format_values[25] },
  { "F32LE", "F32LE", sizeof (uint32_t), &format_values[26] },
  { "F32BE", "F32BE", sizeof (uint32_t), &format_values[27] },
  { "F64LE", "F64LE", sizeof (uint32_t), &format_values[28] },
  { "F64BE", "F64BE", sizeof (uint32_t), &format_values[29] },
};

static const SpaRectangle min_size = { 1, 1 };
static const SpaRectangle max_size = { UINT32_MAX, UINT32_MAX };

static const SpaPropRangeInfo size_range[] = {
  { "min", "Minimum value", sizeof (SpaRectangle), &min_size },
  { "max", "Maximum value", sizeof (SpaRectangle), &max_size },
};

static const uint32_t interlace_modes[] = {
  SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE,
  SPA_VIDEO_INTERLACE_MODE_INTERLEAVED,
  SPA_VIDEO_INTERLACE_MODE_MIXED,
  SPA_VIDEO_INTERLACE_MODE_FIELDS
};

static const SpaPropRangeInfo interlace_mode_range[] = {
  { "progressive", "Progressive video", sizeof (uint32_t), &interlace_modes[0] },
  { "interleaved", "Interleaved video", sizeof (uint32_t), &interlace_modes[1] },
  { "mixed", "Mixed interlaced video", sizeof (uint32_t), &interlace_modes[2] },
  { "fields", "Fields interlaced video", sizeof (uint32_t), &interlace_modes[3] },
};


static const uint32_t multiview_modes[] = {
  SPA_VIDEO_MULTIVIEW_MODE_NONE,
  SPA_VIDEO_MULTIVIEW_MODE_MONO,
  SPA_VIDEO_MULTIVIEW_MODE_LEFT,
  SPA_VIDEO_MULTIVIEW_MODE_RIGHT,
  SPA_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE,
  SPA_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE_QUINCUNX,
  SPA_VIDEO_MULTIVIEW_MODE_COLUMN_INTERLEAVED,
  SPA_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED,
  SPA_VIDEO_MULTIVIEW_MODE_TOP_BOTTOM,
  SPA_VIDEO_MULTIVIEW_MODE_CHECKERBOARD,
  SPA_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME,
  SPA_VIDEO_MULTIVIEW_MODE_MULTIVIEW_FRAME_BY_FRAME,
  SPA_VIDEO_MULTIVIEW_MODE_SEPARATED
};

static const SpaPropRangeInfo multiview_mode_range[] = {
  { "mono", "Mono", sizeof (uint32_t), &multiview_modes[1] },
  { "left", "Left", sizeof (uint32_t), &multiview_modes[2] },
  { "right", "Right", sizeof (uint32_t), &multiview_modes[3] },
  { "side-by-side", "Side by side", sizeof (uint32_t), &multiview_modes[4] },
  { "side-by-side-quincunx", "Side by side Cuincunx", sizeof (uint32_t), &multiview_modes[5] },
  { "column-interleaved", "Column Interleaved", sizeof (uint32_t), &multiview_modes[6] },
  { "row-interleaved", "Row Interleaved", sizeof (uint32_t), &multiview_modes[7] },
  { "top-bottom", "Top Bottom", sizeof (uint32_t), &multiview_modes[8] },
  { "checkerboard", "Checkerboard", sizeof (uint32_t), &multiview_modes[9] },
  { "frame-by-frame", "Frame by frame", sizeof (uint32_t), &multiview_modes[10] },
  { "multiview-frame-by-frame", "Multiview Frame by frame", sizeof (uint32_t), &multiview_modes[11] },
  { "separated", "Separated", sizeof (uint32_t), &multiview_modes[12] },
};

static const uint32_t multiview_flags[] = {
  SPA_VIDEO_MULTIVIEW_FLAGS_NONE,
  SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST,
  SPA_VIDEO_MULTIVIEW_FLAGS_LEFT_FLIPPED,
  SPA_VIDEO_MULTIVIEW_FLAGS_LEFT_FLOPPED,
  SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLIPPED,
  SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLOPPED,
  SPA_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT,
  SPA_VIDEO_MULTIVIEW_FLAGS_MIXED_MONO,
};

static const SpaPropRangeInfo multiview_flags_range[] = {
  { "none", "None", sizeof (uint32_t), &multiview_flags[0] },
  { "right-view-first", "Right view first", sizeof (uint32_t), &multiview_flags[1] },
  { "left-flipped", "Left flipped", sizeof (uint32_t), &multiview_flags[2] },
  { "left-flopped", "Left flopped", sizeof (uint32_t), &multiview_flags[3] },
  { "right-flipped", "Right flipped", sizeof (uint32_t), &multiview_flags[4] },
  { "right-flopped", "Right flopped", sizeof (uint32_t), &multiview_flags[5] },
  { "half-aspect", "Half aspect", sizeof (uint32_t), &multiview_flags[6] },
  { "mixed-mono", "Mixed mono", sizeof (uint32_t), &multiview_flags[7] },
};

static const uint32_t chroma_sites[] = {
  SPA_VIDEO_CHROMA_SITE_UNKNOWN,
  SPA_VIDEO_CHROMA_SITE_NONE,
  SPA_VIDEO_CHROMA_SITE_H_COSITED,
  SPA_VIDEO_CHROMA_SITE_V_COSITED,
  SPA_VIDEO_CHROMA_SITE_ALT_LINE,
};

static const SpaPropRangeInfo chroma_site_range[] = {
  { "unknown", "Unknown", sizeof (uint32_t), &chroma_sites[0] },
  { "none", "None", sizeof (uint32_t), &chroma_sites[1] },
  { "h-cosited", "H-cosited", sizeof (uint32_t), &chroma_sites[2] },
  { "v-cosited", "V-cosited", sizeof (uint32_t), &chroma_sites[3] },
  { "alt-line", "Alt line", sizeof (uint32_t), &chroma_sites[4] }
};

static const uint32_t color_ranges[] = {
  SPA_VIDEO_COLOR_RANGE_UNKNOWN,
  SPA_VIDEO_COLOR_RANGE_0_255,
  SPA_VIDEO_COLOR_RANGE_16_235
};

static const SpaPropRangeInfo color_range_range[] = {
  { "unknown", "Unknown color range", sizeof (uint32_t), &color_ranges[0] },
  { "0_255", "0-255", sizeof (uint32_t), &color_ranges[1] },
  { "16_235", "16-235", sizeof (uint32_t), &color_ranges[2] },
};

static const uint32_t color_matrices[] = {
  SPA_VIDEO_COLOR_MATRIX_UNKNOWN,
  SPA_VIDEO_COLOR_MATRIX_RGB,
  SPA_VIDEO_COLOR_MATRIX_FCC,
  SPA_VIDEO_COLOR_MATRIX_BT709,
  SPA_VIDEO_COLOR_MATRIX_BT601,
  SPA_VIDEO_COLOR_MATRIX_SMPTE240M,
  SPA_VIDEO_COLOR_MATRIX_BT2020
};

static const SpaPropRangeInfo color_matrix_range[] = {
  { "unknown", "Unknown color matrix", sizeof (uint32_t), &color_matrices[0] },
  { "rgb", "identity matrix", sizeof (uint32_t), &color_matrices[1] },
  { "fcc", "FCC color matrix", sizeof (uint32_t), &color_matrices[2] },
  { "bt709", "ITU-R BT.709 color matrix", sizeof (uint32_t), &color_matrices[3] },
  { "bt601", "ITU-R BT.601 color matrix", sizeof (uint32_t), &color_matrices[4] },
  { "smpte240m", "SMPTE 240M color matrix", sizeof (uint32_t), &color_matrices[5] },
  { "bt2020", "ITU-R BT.2020 color matrix", sizeof (uint32_t), &color_matrices[6] },
};

static const uint32_t transfer_functions[] = {
  SPA_VIDEO_TRANSFER_UNKNOWN,
  SPA_VIDEO_TRANSFER_GAMMA10,
  SPA_VIDEO_TRANSFER_GAMMA18,
  SPA_VIDEO_TRANSFER_GAMMA20,
  SPA_VIDEO_TRANSFER_GAMMA22,
  SPA_VIDEO_TRANSFER_BT709,
  SPA_VIDEO_TRANSFER_SMPTE240M,
  SPA_VIDEO_TRANSFER_SRGB,
  SPA_VIDEO_TRANSFER_GAMMA28,
  SPA_VIDEO_TRANSFER_LOG100,
  SPA_VIDEO_TRANSFER_LOG316,
  SPA_VIDEO_TRANSFER_BT2020_12,
  SPA_VIDEO_TRANSFER_ADOBERGB
};

static const SpaPropRangeInfo transfer_function_range[] = {
  { "unknown", "Unknown transfer function", sizeof (uint32_t), &transfer_functions[0] },
  { "gamma10", "linear RGB, gamma 1.0 curve", sizeof (uint32_t), &transfer_functions[1] },
  { "gamma18", "gamma 1.8 curve", sizeof (uint32_t), &transfer_functions[2] },
  { "gamma20", "gamma 2.0 curve", sizeof (uint32_t), &transfer_functions[3] },
  { "gamma22", "gamma 2.2 curve", sizeof (uint32_t), &transfer_functions[4] },
  { "bt709", "Gamma 2.2 curve with a linear segment", sizeof (uint32_t), &transfer_functions[5] },
  { "smpte240m", "Gamma 2.2 curve with a linear segment", sizeof (uint32_t), &transfer_functions[6] },
  { "srgb", "Gamma 2.4 curve with a linear segment", sizeof (uint32_t), &transfer_functions[7] },
  { "gamma28", "Gamma 2.8 curve", sizeof (uint32_t), &transfer_functions[8] },
  { "log100", "Logarithmic transfer characteristic 100:1 range", sizeof (uint32_t), &transfer_functions[9] },
  { "log316", "Logarithmic transfer characteristic 316.22777:1 range", sizeof (uint32_t), &transfer_functions[10] },
  { "bt2020_12", "Gamma 2.2 curve with a linear segment", sizeof (uint32_t), &transfer_functions[11] },
  { "adobergb", "Gamma 2.19921875", sizeof (uint32_t), &transfer_functions[12] },
};

static const uint32_t color_primaries[] = {
  SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN,
  SPA_VIDEO_COLOR_PRIMARIES_BT709,
  SPA_VIDEO_COLOR_PRIMARIES_BT470M,
  SPA_VIDEO_COLOR_PRIMARIES_BT470BG,
  SPA_VIDEO_COLOR_PRIMARIES_SMPTE170M,
  SPA_VIDEO_COLOR_PRIMARIES_SMPTE240M,
  SPA_VIDEO_COLOR_PRIMARIES_FILM,
  SPA_VIDEO_COLOR_PRIMARIES_BT2020,
  SPA_VIDEO_COLOR_PRIMARIES_ADOBERGB
};

static const SpaPropRangeInfo color_primaries_range[] = {
  { "unknown", "Unknown color primaries", sizeof (uint32_t), &color_primaries[0] },
  { "bt709", "BT709 primaries", sizeof (uint32_t), &color_primaries[1] },
  { "bt470M", "BT470M primaries", sizeof (uint32_t), &color_primaries[2] },
  { "bt470BG", "BT470BG primaries", sizeof (uint32_t), &color_primaries[3] },
  { "smpte170m", "SMPTE170M primaries", sizeof (uint32_t), &color_primaries[4] },
  { "smpte240m", "SMPTE240M primaries", sizeof (uint32_t), &color_primaries[5] },
  { "film", "Generic film primaries", sizeof (uint32_t), &color_primaries[6] },
  { "bt2020", "BT2020 primaries", sizeof (uint32_t), &color_primaries[7] },
  { "adobergb", "Adobe RGB primaries", sizeof (uint32_t), &color_primaries[8] },
};

static const uint32_t min_uint32 = 1;
static const uint32_t max_uint32 = UINT32_MAX;

static const SpaPropRangeInfo uint32_range[] = {
  { "min", "Minimum value", sizeof (uint32_t), &min_uint32 },
  { "max", "Maximum value", sizeof (uint32_t), &max_uint32 },
};

static const SpaFraction min_framerate = { 0, 1 };
static const SpaFraction max_framerate = { UINT32_MAX, 1 };

static const SpaPropRangeInfo framerate_range[] = {
  { "min", "Minimum value", sizeof (SpaFraction), &min_framerate },
  { "max", "Maximum value", sizeof (SpaFraction), &max_framerate },
};

static const SpaPropInfo raw_format_prop_info[] =
{
  { SPA_PROP_ID_VIDEO_FORMAT,       "format", "The media format",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.format,
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (format_range), format_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.format),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 0,
                                    NULL },
  { SPA_PROP_ID_VIDEO_SIZE,         "size", "Video size",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_RECTANGLE, sizeof (SpaRectangle),
                                    sizeof (SpaRectangle), &default_info.size,
                                    SPA_PROP_RANGE_TYPE_MIN_MAX, 2, size_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.size),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 1,
                                    NULL },
  { SPA_PROP_ID_VIDEO_FRAMERATE,    "framerate", "Video framerate",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_FRACTION, sizeof (SpaFraction),
                                    sizeof (SpaFraction), &default_info.framerate,
                                    SPA_PROP_RANGE_TYPE_MIN_MAX, 2, framerate_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.framerate),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 2,
                                    NULL },
  { SPA_PROP_ID_VIDEO_MAX_FRAMERATE, "max-framerate", "Video max framerate",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_FRACTION, sizeof (SpaFraction),
                                    sizeof (SpaFraction), &default_info.max_framerate,
                                    SPA_PROP_RANGE_TYPE_MIN_MAX, 2, framerate_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.max_framerate),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 3,
                                    NULL },
  { SPA_PROP_ID_VIDEO_VIEWS,        "views", "Video number of views",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.views,
                                    SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.views),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 4,
                                    NULL },
  { SPA_PROP_ID_VIDEO_INTERLACE_MODE, "interlace-mode", "Interlace mode",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.interlace_mode,
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (interlace_mode_range), interlace_mode_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.interlace_mode),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 5,
                                    NULL },
  { SPA_PROP_ID_VIDEO_PIXEL_ASPECT_RATIO, "pixel-aspect-ratio", "Video pixel aspect ratio",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_FRACTION, sizeof (SpaFraction),
                                    sizeof (SpaFraction), &default_info.pixel_aspect_ratio,
                                    SPA_PROP_RANGE_TYPE_MIN_MAX, 2, framerate_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.pixel_aspect_ratio),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 6,
                                    NULL },
  { SPA_PROP_ID_VIDEO_MULTIVIEW_MODE, "multiview-mode", "Multiview mode",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.multiview_mode,
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (multiview_mode_range), multiview_mode_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.multiview_mode),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 7,
                                    NULL },
  { SPA_PROP_ID_VIDEO_MULTIVIEW_FLAGS, "multiview-flags", "Multiview flags",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.multiview_flags,
                                    SPA_PROP_RANGE_TYPE_FLAGS, SPA_N_ELEMENTS (multiview_flags_range), multiview_flags_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.multiview_flags),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 8,
                                    NULL },
  { SPA_PROP_ID_VIDEO_CHROMA_SITE,  "chroma-site", "Chroma site",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.chroma_site,
                                    SPA_PROP_RANGE_TYPE_FLAGS, SPA_N_ELEMENTS (chroma_site_range), chroma_site_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.chroma_site),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 9,
                                    NULL },
  { SPA_PROP_ID_VIDEO_COLOR_RANGE,  "color-range", "Color range",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.color_range,
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (color_range_range), color_range_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.color_range),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 10,
                                    NULL },
  { SPA_PROP_ID_VIDEO_COLOR_MATRIX,  "color-matrix", "Color matrix",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.color_matrix,
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (color_matrix_range), color_matrix_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.color_matrix),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 11,
                                    NULL },
  { SPA_PROP_ID_VIDEO_TRANSFER_FUNCTION,  "transfer-function", "Transfer function",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.transfer_function,
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (transfer_function_range), transfer_function_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.transfer_function),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 12,
                                    NULL },
  { SPA_PROP_ID_VIDEO_COLOR_PRIMARIES,  "color-primaries", "Color primaries",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    sizeof (uint32_t), &default_info.color_primaries,
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (color_primaries_range), color_primaries_range,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info.color_primaries),
                                    offsetof (SpaVideoRawFormat, unset_mask), 1 << 13,
                                    NULL },
  { SPA_PROP_ID_VIDEO_RAW_INFO,     "info", "the SpaVideoRawInfo structure",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL,
                                    SPA_PROP_TYPE_POINTER, sizeof (SpaVideoRawInfo),
                                    0, NULL,
                                    SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                    NULL,
                                    offsetof (SpaVideoRawFormat, info),
                                    offsetof (SpaVideoRawFormat, unset_mask), ~0,
                                    NULL },
};

SpaResult
spa_video_raw_format_init (SpaVideoRawFormat *format)
{
  format->format.media_type = SPA_MEDIA_TYPE_VIDEO;
  format->format.media_subtype = SPA_MEDIA_SUBTYPE_RAW;
  format->format.props.n_prop_info = SPA_N_ELEMENTS (raw_format_prop_info);
  format->format.props.prop_info = raw_format_prop_info;
  format->unset_mask = (1 << 14)-1;
  format->info = default_info;

  return SPA_RESULT_OK;
}

SpaResult
spa_video_raw_format_parse (const SpaFormat *format,
                            SpaVideoRawFormat *rawformat)
{
  SpaPropValue value;
  const SpaProps *props;
  SpaResult res;

  if ((void *)format == (void *)rawformat)
    return SPA_RESULT_OK;

  if (format->media_type != SPA_MEDIA_TYPE_VIDEO ||
      format->media_subtype != SPA_MEDIA_SUBTYPE_RAW)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  spa_video_raw_format_init (rawformat);

  props = &format->props;
  if ((res = spa_props_get_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_RAW_INFO), &value)) < 0)
    goto fallback;

  if (value.type != SPA_PROP_TYPE_POINTER || value.size != sizeof (SpaVideoRawInfo))
    goto fallback;

  memcpy (&rawformat->info, value.value, sizeof (SpaVideoRawInfo));

  return SPA_RESULT_OK;

fallback:
  res = spa_props_copy (props, &rawformat->format.props);

  return res;
}

SpaResult
spa_video_raw_fill_default_info (SpaVideoRawInfo *info)
{
  if (info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  memcpy (info, &default_info, sizeof (SpaVideoRawInfo));

  return SPA_RESULT_OK;
}


SpaResult
spa_video_raw_fill_prop_info (SpaPropInfo    *info,
                              SpaPropIdVideo  id,
                              size_t          offset)
{
  unsigned int i;

  if (info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < SPA_N_ELEMENTS (raw_format_prop_info); i++) {
    if (raw_format_prop_info[i].id == id) {
      memcpy (info, &raw_format_prop_info[i], sizeof (SpaPropInfo));
      info->offset = offset;
      info->mask_offset = 0;
      info->unset_mask = 0;
      return SPA_RESULT_OK;
    }
  }
  return SPA_RESULT_INVALID_PROPERTY_INDEX;
}
