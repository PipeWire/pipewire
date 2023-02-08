/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/utils/string.h>
#include <spa/utils/names.h>
#include <spa/support/plugin.h>

struct impl {
	struct spa_handle handle;
	struct spa_audio_aec aec;
	struct spa_log *log;

	struct spa_hook_list hooks_list;

	uint32_t channels;
};

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.aec.null");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

static int null_init(void *object, const struct spa_dict *args, const struct spa_audio_info_raw *info)
{
	struct impl *impl = object;
	impl->channels = info->channels;
	return 0;
}

static int null_run(void *object, const float *rec[], const float *play[], float *out[], uint32_t n_samples)
{
	struct impl *impl = object;
	uint32_t i;
	for (i = 0; i < impl->channels; i++)
		memcpy(out[i], rec[i], n_samples * sizeof(float));
	return 0;
}

static const struct spa_audio_aec_methods impl_aec = {
	SPA_VERSION_AUDIO_AEC,
	.init = null_init,
	.run = null_run,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	struct impl *impl = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_AUDIO_AEC))
		*interface = &impl->aec;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
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
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	struct impl *impl = (struct impl *) handle;

	impl->aec.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_AUDIO_AEC,
		SPA_VERSION_AUDIO_AEC,
		&impl_aec, impl);
	impl->aec.name = "null";
	impl->aec.info = NULL;
	impl->aec.latency = NULL;

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);

	spa_hook_list_init(&impl->hooks_list);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_AUDIO_AEC,},
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

static const struct spa_handle_factory spa_aec_null_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AEC,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_aec_null_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
