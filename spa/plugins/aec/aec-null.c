/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
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

#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/utils/string.h>
#include <spa/utils/names.h>
#include <spa/support/plugin.h>

struct impl {
	struct spa_handle handle;
	struct spa_log *log;
	uint32_t channels;
};

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.aec.null");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

static int null_create(struct spa_handle *handle, const struct spa_dict *args, const struct spa_audio_info_raw *info)
{
	struct impl *impl;
	impl = (struct impl *) handle;
	impl->channels = info->channels;

	return 0;
}

static int null_run(struct spa_handle *handle, const float *rec[], const float *play[], float *out[], uint32_t n_samples)
{
	struct impl *impl = (struct impl *) handle;
	uint32_t i;
	for (i = 0; i < impl->channels; i++)
		memcpy(out[i], rec[i], n_samples * sizeof(float));
	return 0;
}

struct spa_dict *null_get_properties(SPA_UNUSED struct spa_handle *handle)
{
	/* Not supported */
	return NULL;
}

int null_set_properties(SPA_UNUSED struct spa_handle *handle, SPA_UNUSED const struct spa_dict *args)
{
	/* Not supported */
	return -1;
}

static struct echo_cancel_info echo_cancel_null_impl = {
	.name = "null",
	.info = SPA_DICT_INIT(NULL, 0),
	.latency = NULL,
	.create = null_create,
	.run = null_run,
	.get_properties = null_get_properties,
	.set_properties = null_set_properties,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_AEC))
		*interface = &echo_cancel_null_impl;
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
	struct impl *impl;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	echo_cancel_null_impl.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_AEC,
		SPA_VERSION_AUDIO_AEC,
		NULL,
		NULL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;
	impl = (struct impl *) handle;
	impl->log = (struct spa_log*)spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_AEC,},
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

const struct spa_handle_factory spa_aec_exaudio_factory = {
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
		*factory = &spa_aec_exaudio_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
