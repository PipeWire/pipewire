/* Spa A2DP codec API
 *
 * Copyright © 2020 Wim Taymans
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
#ifndef SPA_BLUEZ5_A2DP_CODECS_H_
#define SPA_BLUEZ5_A2DP_CODECS_H_

#include <stdint.h>
#include <stddef.h>

#include <spa/param/audio/format.h>
#include <spa/param/bluetooth/audio.h>
#include <spa/utils/names.h>
#include <spa/support/plugin.h>
#include <spa/pod/pod.h>
#include <spa/pod/builder.h>

#include "a2dp-codec-caps.h"

/*
 * The codec plugin SPA interface is private.  The version should be incremented
 * when any of the structs or semantics change.
 */

#define SPA_TYPE_INTERFACE_Bluez5CodecA2DP	SPA_TYPE_INFO_INTERFACE_BASE "Bluez5:Codec:A2DP:Private"

#define SPA_VERSION_BLUEZ5_CODEC_A2DP		0

struct spa_bluez5_codec_a2dp {
	struct spa_interface iface;
	const struct a2dp_codec * const *codecs;	/**< NULL terminated array */
};

#define A2DP_CODEC_FACTORY_NAME(basename)		(SPA_NAME_API_CODEC_BLUEZ5_A2DP "." basename)

#ifdef CODEC_PLUGIN
#define A2DP_CODEC_EXPORT_DEF(basename,...)	\
	const char *codec_plugin_factory_name = A2DP_CODEC_FACTORY_NAME(basename); \
	static const struct a2dp_codec * const codec_plugin_a2dp_codec_list[] = { __VA_ARGS__, NULL };	\
	const struct a2dp_codec * const * const codec_plugin_a2dp_codecs = codec_plugin_a2dp_codec_list;

extern const struct a2dp_codec * const * const codec_plugin_a2dp_codecs;
extern const char *codec_plugin_factory_name;
#endif


#define A2DP_CODEC_DEFAULT_RATE		48000
#define A2DP_CODEC_DEFAULT_CHANNELS	2

struct a2dp_codec_audio_info {
	uint32_t rate;
	uint32_t channels;
};

struct a2dp_codec {
	enum spa_bluetooth_audio_codec id;
	uint8_t codec_id;
	a2dp_vendor_codec_t vendor;

	const char *name;
	const char *description;
	const char *endpoint_name;	/**< Endpoint name. If NULL, same as name */
	const struct spa_dict *info;

	const size_t send_buf_size;

	const struct a2dp_codec *duplex_codec;	/**< Codec for non-standard A2DP duplex channel */

	int (*fill_caps) (const struct a2dp_codec *codec, uint32_t flags,
			uint8_t caps[A2DP_MAX_CAPS_SIZE]);
	int (*select_config) (const struct a2dp_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			const struct a2dp_codec_audio_info *info,
			const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE]);
	int (*enum_config) (const struct a2dp_codec *codec,
			const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
			struct spa_pod_builder *builder, struct spa_pod **param);
	int (*validate_config) (const struct a2dp_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info);

	/** qsort comparison sorting caps in order of preference for the codec.
	 * Used in codec switching to select best remote endpoints.
	 * The caps handed in correspond to this codec_id, but are
	 * otherwise not checked beforehand.
	 */
	int (*caps_preference_cmp) (const struct a2dp_codec *codec, const void *caps1, size_t caps1_size,
			const void *caps2, size_t caps2_size, const struct a2dp_codec_audio_info *info);

	void *(*init_props) (const struct a2dp_codec *codec, const struct spa_dict *settings);
	void (*clear_props) (void *);
	int (*enum_props) (void *props, const struct spa_dict *settings, uint32_t id, uint32_t idx,
			struct spa_pod_builder *builder, struct spa_pod **param);
	int (*set_props) (void *props, const struct spa_pod *param);

	void *(*init) (const struct a2dp_codec *codec, uint32_t flags, void *config, size_t config_size,
			const struct spa_audio_info *info, void *props, size_t mtu);
	void (*deinit) (void *data);

	int (*update_props) (void *data, void *props);

	int (*get_block_size) (void *data);

	int (*abr_process) (void *data, size_t unsent);

	int (*start_encode) (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp);
	int (*encode) (void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush);

	int (*start_decode) (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp);
	int (*decode) (void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out);

	int (*reduce_bitpool) (void *data);
	int (*increase_bitpool) (void *data);
};

struct a2dp_codec_config {
	uint32_t config;
	int value;
	unsigned int priority;
};

int a2dp_codec_select_config(const struct a2dp_codec_config configs[], size_t n,
	uint32_t cap, int preferred_value);

bool a2dp_codec_check_caps(const struct a2dp_codec *codec, unsigned int codec_id,
	const void *caps, size_t caps_size, const struct a2dp_codec_audio_info *info);

#endif
