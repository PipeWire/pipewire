/* Spa A2DP codec API */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <spa/utils/string.h>

#include "defs.h"
#include "codec-loader.h"

#define MEDIA_CODEC_LIB_BASE	"bluez5/libspa-codec-bluez5-"

/* AVDTP allows 0x3E endpoints, can't have more codecs than that */
#define MAX_CODECS	0x3E
#define MAX_HANDLES	MAX_CODECS

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.codecs");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct impl {
	const struct media_codec *codecs[MAX_CODECS + 1];
	struct spa_handle *handles[MAX_HANDLES];
	size_t n_codecs;
	size_t n_handles;
	struct spa_plugin_loader *loader;
	struct spa_log *log;
};

static int codec_order(const struct media_codec *c)
{
	static const enum spa_bluetooth_audio_codec order[] = {
		SPA_BLUETOOTH_AUDIO_CODEC_LC3,
		SPA_BLUETOOTH_AUDIO_CODEC_LDAC,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX_HD,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX,
		SPA_BLUETOOTH_AUDIO_CODEC_AAC,
		SPA_BLUETOOTH_AUDIO_CODEC_LC3PLUS_HR,
		SPA_BLUETOOTH_AUDIO_CODEC_MPEG,
		SPA_BLUETOOTH_AUDIO_CODEC_SBC,
		SPA_BLUETOOTH_AUDIO_CODEC_SBC_XQ,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL,
		SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL_DUPLEX,
		SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM,
		SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM_DUPLEX,
		SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05,
		SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_51,
		SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_71,
		SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_DUPLEX,
		SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO,
	};
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(order); ++i)
		if (c->id == order[i])
			return i;
	return SPA_N_ELEMENTS(order);
}

static int codec_order_cmp(const void *a, const void *b)
{
	const struct media_codec * const *ca = a;
	const struct media_codec * const *cb = b;
	int ia = codec_order(*ca);
	int ib = codec_order(*cb);
	if (*ca == *cb)
		return 0;
	return (ia == ib) ? (*ca < *cb ? -1 : 1) : ia - ib;
}

static int load_media_codecs_from(struct impl *impl, const char *factory_name, const char *libname)
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

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Bluez5CodecMedia, &iface)) < 0) {
		spa_log_warn(impl->log, "Bluetooth codec plugin %s has no codec interface",
				factory_name);
		goto fail;
	}

	bluez5_codec_a2dp = iface;

	if (bluez5_codec_a2dp->iface.version != SPA_VERSION_BLUEZ5_CODEC_MEDIA) {
		spa_log_warn(impl->log, "codec plugin %s has incompatible ABI version (%d != %d)",
				factory_name, bluez5_codec_a2dp->iface.version, SPA_VERSION_BLUEZ5_CODEC_MEDIA);
		res = -ENOENT;
		goto fail;
	}

	for (i = 0; bluez5_codec_a2dp->codecs[i]; ++i) {
		const struct media_codec *c = bluez5_codec_a2dp->codecs[i];
		const char *ep = c->endpoint_name ? c->endpoint_name : c->name;
		size_t j;

		if (!ep)
			goto next_codec;

		if (impl->n_codecs >= MAX_CODECS) {
			spa_log_error(impl->log, "too many A2DP codecs");
			break;
		}

		/* Don't load duplicate endpoints */
		for (j = 0; j < impl->n_codecs; ++j) {
			const struct media_codec *c2 = impl->codecs[j];
			const char *ep2 = c2->endpoint_name ? c2->endpoint_name : c2->name;
			if (spa_streq(ep, ep2) && c->fill_caps && c2->fill_caps) {
				spa_log_debug(impl->log, "media codec %s from %s duplicate endpoint %s",
						c->name, factory_name, ep);
				goto next_codec;
			}
		}

		spa_log_debug(impl->log, "loaded media codec %s from %s, endpoint:%s",
				c->name, factory_name, ep);

		if (c->set_log)
			c->set_log(impl->log);

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

const struct media_codec * const *load_media_codecs(struct spa_plugin_loader *loader, struct spa_log *log)
{
	struct impl *impl;
	bool has_sbc;
	size_t i;
	const struct { const char *factory; const char *lib; } plugins[] = {
#define MEDIA_CODEC_FACTORY_LIB(basename) \
		{ MEDIA_CODEC_FACTORY_NAME(basename), MEDIA_CODEC_LIB_BASE basename }
		MEDIA_CODEC_FACTORY_LIB("aac"),
		MEDIA_CODEC_FACTORY_LIB("aptx"),
		MEDIA_CODEC_FACTORY_LIB("faststream"),
		MEDIA_CODEC_FACTORY_LIB("ldac"),
		MEDIA_CODEC_FACTORY_LIB("sbc"),
		MEDIA_CODEC_FACTORY_LIB("lc3plus"),
		MEDIA_CODEC_FACTORY_LIB("opus"),
		MEDIA_CODEC_FACTORY_LIB("lc3")
#undef MEDIA_CODEC_FACTORY_LIB
	};

	impl = calloc(sizeof(struct impl), 1);
	if (impl == NULL)
		return NULL;

	impl->loader = loader;
	impl->log = log;

	spa_log_topic_init(impl->log, &log_topic);

	for (i = 0; i < SPA_N_ELEMENTS(plugins); ++i)
		load_media_codecs_from(impl, plugins[i].factory, plugins[i].lib);

	has_sbc = false;
	for (i = 0; i < impl->n_codecs; ++i)
		if (impl->codecs[i]->id == SPA_BLUETOOTH_AUDIO_CODEC_SBC)
			has_sbc = true;

	if (!has_sbc) {
		spa_log_error(impl->log, "failed to load A2DP SBC codec from plugins");
		free_media_codecs(impl->codecs);
		errno = ENOENT;
		return NULL;
	}

	qsort(impl->codecs, impl->n_codecs, sizeof(const struct media_codec *), codec_order_cmp);

	return impl->codecs;
}

void free_media_codecs(const struct media_codec * const *media_codecs)
{
	struct impl *impl = SPA_CONTAINER_OF(media_codecs, struct impl, codecs);
	size_t i;

	for (i = 0; i < impl->n_handles; ++i)
		spa_plugin_loader_unload(impl->loader, impl->handles[i]);

	free(impl);
}
