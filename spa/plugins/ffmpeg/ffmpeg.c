/* Spa FFmpeg support
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

#include <errno.h>
#include <stdio.h>

#include <spa/support/plugin.h>
#include <spa/node/node.h>

#include <libavcodec/avcodec.h>

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
	{SPA_TYPE_INTERFACE_Node, },
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

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 10, 100)
static const AVCodec *find_codec_by_index(uint32_t index)
{
	static void *av_iter_data;
	static uint32_t next_index;

	const AVCodec *c = NULL;

	if (index == 0) {
		av_iter_data = NULL;
		next_index = 0;
	}

	while (next_index <= index) {
		c = av_codec_iterate(&av_iter_data);
		next_index += 1;

		if (!c)
			break;
	}

	return c;
}
#else
static const AVCodec *find_codec_by_index(uint32_t index)
{
	static const AVCodec *last_codec;
	static uint32_t next_index;

	if (index == 0) {
		last_codec = NULL;
		next_index = 0;
	}

	while (next_index <= index) {
		last_codec = av_codec_next(last_codec);
		next_index += 1;

		if (!last_codec)
			break;
	}

	return last_codec;
}
#endif

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	static struct spa_handle_factory f;
	static char name[128];

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
	avcodec_register_all();
#endif

	const AVCodec *c = find_codec_by_index(*index);

	if (c == NULL)
		return 0;

	if (av_codec_is_encoder(c)) {
		snprintf(name, sizeof(name), "encoder.%s", c->name);
		f.init = ffmpeg_enc_init;
	} else {
		snprintf(name, sizeof(name), "decoder.%s", c->name);
		f.init = ffmpeg_dec_init;
	}

	f.name = name;
	f.info = NULL;
	f.enum_interface_info = ffmpeg_enum_interface_info;

	*factory = &f;
	(*index)++;

	return 1;
}
