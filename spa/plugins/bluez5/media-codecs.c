/*
 * BlueALSA - bluez-a2dp.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <bluetooth/bluetooth.h>

#include <spa/utils/string.h>
#include <spa/utils/cleanup.h>

#include "media-codecs.h"

int media_codec_select_config(const struct media_codec_config configs[], size_t n,
			     uint32_t cap, int preferred_value)
{
	size_t i;
	spa_autofree int *scores = NULL;
	int res;
	unsigned int max_priority;

	if (n == 0)
		return -EINVAL;

	scores = calloc(n, sizeof(int));
	if (scores == NULL)
		return -errno;

	max_priority = configs[0].priority;
	for (i = 1; i < n; ++i) {
		if (configs[i].priority > max_priority)
			max_priority = configs[i].priority;
	}

	for (i = 0; i < n; ++i) {
		if (!(configs[i].config & cap)) {
			scores[i] = -1;
			continue;
		}
		if (configs[i].value == preferred_value)
			scores[i] = 100 * (max_priority + 1);
		else if (configs[i].value > preferred_value)
			scores[i] = 10 * (max_priority + 1);
		else
			scores[i] = 1;

		scores[i] *= configs[i].priority + 1;
	}

	res = 0;
	for (i = 1; i < n; ++i) {
		if (scores[i] > scores[res])
			res = i;
	}

	if (scores[res] < 0)
		return -EINVAL;

	return res;
}

int media_codec_get_config(const struct media_codec_config configs[], size_t n,
		uint32_t conf)
{
	size_t i;

	for (i = 0; i < n; ++i)
		if (configs[i].config == conf)
			return configs[i].value;

	return -EINVAL;
}

bool media_codec_check_caps(const struct media_codec *codec, unsigned int codec_id,
			   const void *caps, size_t caps_size,
			   const struct media_codec_audio_info *info,
			   const struct spa_dict *global_settings)
{
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	int res;

	if (codec->kind == MEDIA_CODEC_HFP)
		return true;

	if (codec_id != codec->codec_id)
		return false;

	if (caps == NULL)
		return false;

	res = codec->select_config(codec, 0, caps, caps_size, info, global_settings, config);
	if (res < 0)
		return false;

	if (codec->kind == MEDIA_CODEC_BAP)
		return true;
	else
		return ((size_t)res == caps_size);
}

void ltv_writer_data(struct ltv_writer *w, uint8_t type, void* value, size_t len)
{
	struct ltv *ltv;
	size_t sz = (size_t)w->size + sizeof(struct ltv) + len;

	if (!w->buf || sz > w->max_size || (uint16_t)sz != sz) {
		w->buf = NULL;
		return;
	}

	ltv = SPA_PTROFF(w->buf, w->size, struct ltv);
	ltv->len = len + 1;
	ltv->type = type;
	memcpy(ltv->value, value, len);

	w->size = sz;
}

void ltv_writer_uint8(struct ltv_writer *w, uint8_t type, uint8_t v)
{
	ltv_writer_data(w, type, &v, sizeof(v));
}

void ltv_writer_uint16(struct ltv_writer *w, uint8_t type, uint16_t value)
{
	uint16_t v = htobs(value);

	ltv_writer_data(w, type, &v, sizeof(v));
}

void ltv_writer_uint32(struct ltv_writer *w, uint8_t type, uint32_t value)
{
	uint32_t v = htobl(value);

	ltv_writer_data(w, type, &v, sizeof(v));
}

int ltv_writer_end(struct ltv_writer *w)
{
	if (!w->buf)
		return -ENOSPC;

	w->buf = NULL;
	return w->size;
}

#ifdef CODEC_PLUGIN

struct impl {
	struct spa_handle handle;
	struct spa_bluez5_codec_a2dp bluez5_codec_a2dp;
};

static int
impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Bluez5CodecMedia))
		*interface = &this->bluez5_codec_a2dp;
	else
		return -ENOENT;

	return 0;
}

static int
impl_clear(struct spa_handle *handle)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory, const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
		struct spa_handle *handle,
		const struct spa_dict *info,
		const struct spa_support *support,
		uint32_t n_support)
{
	struct impl *this;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->bluez5_codec_a2dp.codecs = codec_plugin_media_codecs;
	this->bluez5_codec_a2dp.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_Bluez5CodecMedia,
		SPA_VERSION_BLUEZ5_CODEC_MEDIA,
		NULL,
		this);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
        {SPA_TYPE_INTERFACE_Bluez5CodecMedia,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;

	return 1;
}

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

static const struct spa_dict_item handle_info_items[] = {
        { SPA_KEY_FACTORY_DESCRIPTION, "Bluetooth codec plugin" },
};

static const struct spa_dict handle_info = SPA_DICT_INIT_ARRAY(handle_info_items);

static struct spa_handle_factory handle_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NULL,
	&handle_info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (handle_factory.name == NULL)
		handle_factory.name = codec_plugin_factory_name;

	switch (*index) {
	case 0:
		*factory = &handle_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

#endif
