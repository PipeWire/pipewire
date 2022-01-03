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

#include "config.h"

#include <spa/utils/string.h>

#include "defs.h"
#include "codec-loader.h"

#define A2DP_CODEC_LIB_BASE	"bluez5/libspa-codec-bluez5-"

/* AVDTP allows 0x3E endpoints, can't have more codecs than that */
#define MAX_CODECS	0x3E
#define MAX_HANDLES	MAX_CODECS

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.codecs");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct impl {
	const struct a2dp_codec *codecs[MAX_CODECS + 1];
	struct spa_handle *handles[MAX_HANDLES];
	size_t n_codecs;
	size_t n_handles;
	struct spa_plugin_loader *loader;
	struct spa_log *log;
};

static int codec_order(const struct a2dp_codec *c)
{
	static const enum spa_bluetooth_audio_codec order[] = {
		SPA_BLUETOOTH_AUDIO_CODEC_LDAC,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX_HD,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX,
		SPA_BLUETOOTH_AUDIO_CODEC_AAC,
		SPA_BLUETOOTH_AUDIO_CODEC_MPEG,
		SPA_BLUETOOTH_AUDIO_CODEC_SBC,
		SPA_BLUETOOTH_AUDIO_CODEC_SBC_XQ,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL_DUPLEX,
		SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM,
		SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM_DUPLEX,
	};
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(order); ++i)
		if (c->id == order[i])
			return i;
	return SPA_N_ELEMENTS(order);
}

static int codec_order_cmp(const void *a, const void *b)
{
	const struct a2dp_codec * const *ca = a;
	const struct a2dp_codec * const *cb = b;
	int ia = codec_order(*ca);
	int ib = codec_order(*cb);
	if (*ca == *cb)
		return 0;
	return (ia == ib) ? (*ca < *cb ? -1 : 1) : ia - ib;
}

static int load_a2dp_codecs_from(struct impl *impl, const char *factory_name, const char *libname)
{
	struct spa_handle *handle = NULL;
	void *iface;
	const struct spa_bluez5_codec_a2dp *bluez5_codec_a2dp;
	int n_codecs = 0;
	int res;
	size_t i;
	struct spa_dict_item info_items[] = {
		{ SPA_KEY_LIBRARY_NAME, libname },
	};
	struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

	handle = spa_plugin_loader_load(impl->loader, factory_name, &info);
	if (handle == NULL) {
		spa_log_info(impl->log, "Bluetooth codec plugin %s not available", factory_name);
		return -ENOENT;
	}

	spa_log_debug(impl->log, "loading codecs from %s", factory_name);

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Bluez5CodecA2DP, &iface)) < 0) {
		spa_log_info(impl->log, "Bluetooth codec plugin %s has no codec interface",
				factory_name);
		goto fail;
	}

	bluez5_codec_a2dp = iface;

	if (bluez5_codec_a2dp->iface.version != SPA_VERSION_BLUEZ5_CODEC_A2DP) {
		spa_log_info(impl->log, "codec plugin %s has incompatible ABI version (%d != %d)",
				factory_name, bluez5_codec_a2dp->iface.version, SPA_VERSION_BLUEZ5_CODEC_A2DP);
		res = -ENOENT;
		goto fail;
	}

	for (i = 0; bluez5_codec_a2dp->codecs[i]; ++i) {
		const struct a2dp_codec *c = bluez5_codec_a2dp->codecs[i];
		size_t j;

		if (impl->n_codecs >= MAX_CODECS) {
			spa_log_error(impl->log, "too many A2DP codecs");
			break;
		}

		/* Don't load duplicate endpoints */
		for (j = 0; j < impl->n_codecs; ++j) {
			const struct a2dp_codec *c2 = impl->codecs[j];
			const char *ep1 = c->endpoint_name ? c->endpoint_name : c->name;
			const char *ep2 = c2->endpoint_name ? c2->endpoint_name : c2->name;
			if (spa_streq(ep1, ep2))
				goto next_codec;
		}

		spa_log_debug(impl->log, "loaded A2DP codec %s from %s", c->name, factory_name);

		impl->codecs[impl->n_codecs++] = c;
		++n_codecs;

	next_codec:
		continue;
	}

	if (n_codecs > 0)
		impl->handles[impl->n_handles++] = handle;
	else
		spa_plugin_loader_unload(impl->loader, handle);

	return 0;

fail:
	if (handle)
		spa_plugin_loader_unload(impl->loader, handle);
	return res;
}

const struct a2dp_codec * const *load_a2dp_codecs(struct spa_plugin_loader *loader, struct spa_log *log)
{
	struct impl *impl;
	bool has_sbc;
	size_t i;
	const struct { const char *factory; const char *lib; } plugins[] = {
#define A2DP_CODEC_FACTORY_LIB(basename) \
		{ A2DP_CODEC_FACTORY_NAME(basename), A2DP_CODEC_LIB_BASE basename }
		A2DP_CODEC_FACTORY_LIB("aac"),
		A2DP_CODEC_FACTORY_LIB("aptx"),
		A2DP_CODEC_FACTORY_LIB("faststream"),
		A2DP_CODEC_FACTORY_LIB("ldac"),
		A2DP_CODEC_FACTORY_LIB("sbc")
#undef A2DP_CODEC_FACTORY_LIB
	};

	impl = calloc(sizeof(struct impl), 1);
	if (impl == NULL)
		return NULL;

	impl->loader = loader;
	impl->log = log;

	spa_log_topic_init(impl->log, &log_topic);

	for (i = 0; i < SPA_N_ELEMENTS(plugins); ++i)
		load_a2dp_codecs_from(impl, plugins[i].factory, plugins[i].lib);

	has_sbc = false;
	for (i = 0; i < impl->n_codecs; ++i)
		if (impl->codecs[i]->id == SPA_BLUETOOTH_AUDIO_CODEC_SBC)
			has_sbc = true;

	if (!has_sbc) {
		spa_log_error(impl->log, "failed to load A2DP SBC codec from plugins");
		free_a2dp_codecs(impl->codecs);
		errno = ENOENT;
		return NULL;
	}

	qsort(impl->codecs, impl->n_codecs, sizeof(const struct a2dp_codec *), codec_order_cmp);

	return impl->codecs;
}

void free_a2dp_codecs(const struct a2dp_codec * const *a2dp_codecs)
{
	struct impl *impl = SPA_CONTAINER_OF(a2dp_codecs, struct impl, codecs);
	size_t i;

	for (i = 0; i < impl->n_handles; ++i)
		spa_plugin_loader_unload(impl->loader, impl->handles[i]);

	free(impl);
}
