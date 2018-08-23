/* Spa V4l2 support
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

#include <errno.h>
#include <stdio.h>

#include <spa/support/plugin.h>
#include <spa/node/node.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

int spa_ffmpeg_dec_init(struct spa_handle *handle, const struct spa_dict *info,
			const struct spa_support *support, uint32_t n_support);
int spa_ffmpeg_enc_init(struct spa_handle *handle, const struct spa_dict *info,
			const struct spa_support *support, uint32_t n_support);

static int
ffmpeg_dec_init(const struct spa_handle_factory *factory,
		struct spa_handle *handle,
		const struct spa_dict *info,
		const struct spa_support *support,
		uint32_t n_support)
{
	if (factory == NULL || handle == NULL)
		return -EINVAL;

	return spa_ffmpeg_dec_init(handle, info, support, n_support);
}

static int
ffmpeg_enc_init(const struct spa_handle_factory *factory,
		struct spa_handle *handle,
		const struct spa_dict *info,
		const struct spa_support *support,
		uint32_t n_support)
{
	if (factory == NULL || handle == NULL)
		return -EINVAL;

	return spa_ffmpeg_enc_init(handle, info, support, n_support);
}

static const struct spa_interface_info ffmpeg_interfaces[] = {
	{SPA_ID_INTERFACE_Node, },
};

static int
ffmpeg_enum_interface_info(const struct spa_handle_factory *factory,
			   const struct spa_interface_info **info,
			   uint32_t *index)
{
	if (factory == NULL || info == NULL || index == NULL)
		return -EINVAL;

	if (*index < SPA_N_ELEMENTS(ffmpeg_interfaces))
		*info = &ffmpeg_interfaces[(*index)++];
	else
		return 0;

	return 1;
}

int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	static const AVCodec *c = NULL;
	static int ci = 0;
	static struct spa_handle_factory f;
	static char name[128];

	av_register_all();

	if (*index == 0) {
		c = av_codec_next(NULL);
		ci = 0;
	}
	while (*index > ci && c) {
		c = av_codec_next(c);
		ci++;
	}
	if (c == NULL)
		return 0;

	if (av_codec_is_encoder(c)) {
		snprintf(name, 128, "ffenc_%s", c->name);
		f.init = ffmpeg_enc_init;
	} else {
		snprintf(name, 128, "ffdec_%s", c->name);
		f.init = ffmpeg_dec_init;
	}
	f.name = name;
	f.info = NULL;
	f.enum_interface_info = ffmpeg_enum_interface_info;

	*factory = &f;
	(*index)++;

	return 1;
}
