/* PipeWire
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

#include <SDL2/SDL.h>

#include <spa/utils/type.h>
#include <spa/pod/builder.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/format.h>

static struct {
	Uint32 format;
	uint32_t id;
} sdl_video_formats[] = {
	{ SDL_PIXELFORMAT_UNKNOWN, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_INDEX1LSB, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_UNKNOWN, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_INDEX1LSB, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_INDEX1MSB, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_INDEX4LSB, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_INDEX4MSB, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_INDEX8, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGB332, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGB444, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGB555, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_BGR555, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_ARGB4444, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGBA4444, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_ABGR4444, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_BGRA4444, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_ARGB1555, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGBA5551, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_ABGR1555, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_BGRA5551, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGB565, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_BGR565, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGB24, SPA_VIDEO_FORMAT_RGB,},
	{ SDL_PIXELFORMAT_RGB888, SPA_VIDEO_FORMAT_RGB,},
	{ SDL_PIXELFORMAT_RGBX8888, SPA_VIDEO_FORMAT_RGBx,},
	{ SDL_PIXELFORMAT_BGR24, SPA_VIDEO_FORMAT_BGR,},
	{ SDL_PIXELFORMAT_BGR888, SPA_VIDEO_FORMAT_BGR,},
	{ SDL_PIXELFORMAT_BGRX8888, SPA_VIDEO_FORMAT_BGRx,},
	{ SDL_PIXELFORMAT_ARGB2101010, SPA_VIDEO_FORMAT_UNKNOWN,},
	{ SDL_PIXELFORMAT_RGBA8888, SPA_VIDEO_FORMAT_RGBA,},
	{ SDL_PIXELFORMAT_ARGB8888, SPA_VIDEO_FORMAT_ARGB,},
	{ SDL_PIXELFORMAT_BGRA8888, SPA_VIDEO_FORMAT_BGRA,},
	{ SDL_PIXELFORMAT_ABGR8888, SPA_VIDEO_FORMAT_ABGR,},
	{ SDL_PIXELFORMAT_YV12, SPA_VIDEO_FORMAT_YV12,},
	{ SDL_PIXELFORMAT_IYUV, SPA_VIDEO_FORMAT_I420,},
	{ SDL_PIXELFORMAT_YUY2, SPA_VIDEO_FORMAT_YUY2,},
	{ SDL_PIXELFORMAT_UYVY, SPA_VIDEO_FORMAT_UYVY,},
	{ SDL_PIXELFORMAT_YVYU, SPA_VIDEO_FORMAT_YVYU,},
#if SDL_VERSION_ATLEAST(2,0,4)
	{ SDL_PIXELFORMAT_NV12, SPA_VIDEO_FORMAT_NV12,},
	{ SDL_PIXELFORMAT_NV21, SPA_VIDEO_FORMAT_NV21,},
#endif
};

static uint32_t sdl_format_to_id(Uint32 format)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(sdl_video_formats); i++) {
		if (sdl_video_formats[i].format == format)
			return sdl_video_formats[i].id;
	}
	return SPA_VIDEO_FORMAT_UNKNOWN;
}

static Uint32 id_to_sdl_format(uint32_t id)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(sdl_video_formats); i++) {
		if (sdl_video_formats[i].id == id)
			return sdl_video_formats[i].format;
	}
	return SDL_PIXELFORMAT_UNKNOWN;
}


static struct spa_pod *sdl_build_formats(SDL_RendererInfo *info, struct spa_pod_builder *b)
{
	int i, c;

	spa_pod_builder_push_object(b, SPA_PARAM_EnumFormat, SPA_ID_OBJECT_Format);
	spa_pod_builder_enum(b, SPA_MEDIA_TYPE_video);
	spa_pod_builder_enum(b, SPA_MEDIA_SUBTYPE_raw);

	spa_pod_builder_push_prop(b, SPA_FORMAT_VIDEO_format,
				  SPA_POD_PROP_FLAG_UNSET |
				  SPA_POD_PROP_RANGE_ENUM);
	for (i = 0, c = 0; i < info->num_texture_formats; i++) {
		uint32_t id = sdl_format_to_id(info->texture_formats[i]);
		if (id == 0)
			continue;
		if (c++ == 0)
			spa_pod_builder_enum(b, id);
		spa_pod_builder_enum(b, id);
	}
	for (i = 0; i < SPA_N_ELEMENTS(sdl_video_formats); i++) {
		uint32_t id = sdl_video_formats[i].id;
		if (id != SPA_VIDEO_FORMAT_UNKNOWN)
			spa_pod_builder_enum(b, id);
	}
	spa_pod_builder_pop(b);
	spa_pod_builder_add(b,
		":", SPA_FORMAT_VIDEO_size,      "Rru", &SPA_RECTANGLE(WIDTH, HEIGHT),
			SPA_POD_PROP_MIN_MAX(&SPA_RECTANGLE(1,1),
					     &SPA_RECTANGLE(info->max_texture_width,
							    info->max_texture_height)),
		":", SPA_FORMAT_VIDEO_framerate, "Fru", &SPA_FRACTION(25,1),
			SPA_POD_PROP_MIN_MAX(&SPA_FRACTION(0,1),
					     &SPA_FRACTION(30,1)),
		NULL);
	return spa_pod_builder_pop(b);
}
