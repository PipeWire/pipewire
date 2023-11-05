/* Spa ALSA Compress-Offload device */
/* SPDX-FileCopyrightText: Copyright @ 2023 Carlos Rafael Giani */
/* SPDX-License-Identifier: MIT */

#include <sys/types.h>
#include <limits.h>
#include <dirent.h>

#include <alsa/asoundlib.h>

#include <spa/debug/dict.h>
#include <spa/debug/log.h>
#include <spa/debug/pod.h>
#include <spa/node/node.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/node/keys.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/dict.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>

#include "compress-offload-api-util.h"
#include "alsa.h"

static const char default_device[] = "hw:0";

struct props {
	char device[64];
	unsigned int card_nr;
};

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
	props->card_nr = 0;
}

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;

	struct spa_hook_list hooks;

	struct props props;
	uint32_t n_nodes;
	uint32_t n_capture;
	uint32_t n_playback;

	uint32_t profile;
};

#define ADD_DICT_ITEM(key, value) do { items[n_items++] = SPA_DICT_ITEM_INIT(key, value); } while (0)

static void emit_node(struct impl *this, const char *device_node, unsigned int device_nr,
                      enum spa_compress_offload_direction direction, snd_ctl_card_info_t *cardinfo,
                      uint32_t id)
{
	struct spa_dict_item items[5];
	uint32_t n_items = 0;
	char alsa_path[128], path[180];
	char node_name[200];
	char node_desc[200];
	struct spa_device_object_info info;
	const char *stream;

	spa_log_debug(this->log, "emitting node info for device %s (card nr %u device nr %u)",
	              device_node, this->props.card_nr, device_nr);

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;

	if (direction == SPA_COMPRESS_OFFLOAD_DIRECTION_PLAYBACK) {
		stream = "playback";
		info.factory_name = SPA_NAME_API_ALSA_COMPRESS_OFFLOAD_SINK;
	} else {
		stream = "capture";
		/* TODO: This is not yet implemented, because getting Compress-Offload
		 * hardware that can capture audio is difficult to do. The only hardware
		 * known is the Wolfson ADSP; the only driver in the kernel that exposes
		 * Compress-Offload capture devices is the one for that hardware. */
		spa_assert_not_reached();
	}

	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;

	snprintf(alsa_path, sizeof(alsa_path), "%s,%u", this->props.device, device_nr);
	snprintf(path, sizeof(path), "alsa:compressed:%s:%u:%s", snd_ctl_card_info_get_id(cardinfo), device_nr, stream);
	snprintf(node_name, sizeof(node_name), "comprC%uD%u", this->props.card_nr, device_nr);
	snprintf(node_desc, sizeof(node_desc), "Compress-Offload sink node (ALSA card %u device %u)", this->props.card_nr, device_nr);

	ADD_DICT_ITEM(SPA_KEY_NODE_NAME,        node_name);
	ADD_DICT_ITEM(SPA_KEY_NODE_DESCRIPTION, node_desc);
	ADD_DICT_ITEM(SPA_KEY_OBJECT_PATH,      path);
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_PATH,    alsa_path);
	/* NOTE: Set alsa.name, since session managers look for this, or for
	 * SPA_KEY_API_ALSA_PCM_NAME, or other items. The best fit in this
	 * case seems to be alsa.name, since SPA_KEY_API_ALSA_PCM_NAME is
	 * PCM specific, as the name suggests. If none of these items are
	 * provided, session managers may not work properly. WirePlumber's
	 * alsa.lua script looks for these for example.
	 * And, since we have no good way of getting a name, just reuse
	 * the alsa_path here. */
	ADD_DICT_ITEM("alsa.name",              alsa_path);

	info.props = &SPA_DICT_INIT(items, n_items);

	spa_log_debug(this->log, "node information:");
	spa_debug_dict(2, info.props);

	spa_device_emit_object_info(&this->hooks, id, &info);
}

static int set_profile(struct impl *this, uint32_t id)
{
	int ret = 0;
	uint32_t i, n_cap, n_play;
	char prefix[32];
	int prefix_length;
	struct dirent *entry;
	DIR *snd_dir = NULL;
	snd_ctl_t *ctl_handle = NULL;
	snd_ctl_card_info_t *cardinfo;

	spa_log_debug(this->log, "enumerate Compress-Offload nodes for card %s; profile: %d",
	              this->props.device, id);

	if ((ret = snd_ctl_open(&ctl_handle, this->props.device, 0)) < 0) {
		spa_log_error(this->log, "can't open control for card %s: %s",
		              this->props.device, snd_strerror(ret));
		goto finish;
	}

	this->profile = id;

	snd_ctl_card_info_alloca(&cardinfo);
	if ((ret = snd_ctl_card_info(ctl_handle, cardinfo)) < 0) {
		spa_log_error(this->log, "error card info: %s", snd_strerror(ret));
		goto finish;
	}

	/* Clear any previous node object info. */
	for (i = 0; i < this->n_nodes; i++)
		spa_device_emit_object_info(&this->hooks, i, NULL);

	this->n_nodes = this->n_capture = this->n_playback = 0;

	/* Profile ID 0 is the "off" profile, that is, the profile where the device
	 * is "disabled". To implement such a disabled state, simply exit here without
	 * adding any nodes after we removed any existing one (see above). */
	if (id == 0)
	{
		spa_log_debug(this->log, "\"Off\" profile selected - exiting without "
		              "creating any nodes after all previous ones were removed");
		goto finish;
	}

	spa_scnprintf(prefix, sizeof(prefix), "comprC%uD", this->props.card_nr);
	prefix_length = strlen(prefix);

	/* There is no API to enumerate all Compress-Offload devices, so we have
	 * to stick to walking through the /dev/snd directory entries and looking
	 * for device nodes that match the comprC<card number>D prefix. */
	snd_dir = opendir("/dev/snd");
	if (snd_dir == NULL)
		goto errno_error;

	i = 0;
	i = n_cap = n_play = 0;
	while ((errno = 0, entry = readdir(snd_dir)) != NULL) {
		long long device_nr;
		enum spa_compress_offload_direction direction;

		if (!(entry->d_type == DT_CHR && spa_strstartswith(entry->d_name, prefix)))
			continue;

		/* Parse the device number from the device filename. We know that the filename
		 * is always structured like this: comprC<card number>D<device number>
		 * We consider "comprC<card number>D" to form the "prefix" here. Right after
		 * that prefix, the device number can be parsed, so skip the prefix. */
		device_nr = strtol(entry->d_name + prefix_length, NULL, 10);
		if ((device_nr < 0) || (device_nr > UINT_MAX)) {
			spa_log_warn(this->log, "device %s contains unusable device number; "
			             "skipping", entry->d_name);
			continue;
		}

		if (get_compress_offload_device_direction(this->props.card_nr, device_nr,
		                                          this->log, &direction) < 0)
			goto finish;

		switch (direction) {
		case SPA_COMPRESS_OFFLOAD_DIRECTION_PLAYBACK:
			n_play++;
			emit_node(this, entry->d_name, device_nr, direction, cardinfo, i++);
			break;
		case SPA_COMPRESS_OFFLOAD_DIRECTION_CAPTURE:
			/* TODO: Disabled for now. See the TODO in emit_node() for details. */
#if 0
			n_cap++;
			emit_node(this, entry->d_name, device_nr, direction, cardinfo, i++);
#endif
			break;
		}
	}

	this->n_capture = n_cap;
	this->n_playback = n_play;
	this->n_nodes = i;

finish:
	if (snd_dir != NULL)
		closedir(snd_dir);
	if (ctl_handle != NULL)
		snd_ctl_close(ctl_handle);
	return ret;

errno_error:
	ret = -errno;
	goto finish;
}

static int emit_info(struct impl *this, bool full)
{
	int err = 0;
	struct spa_dict_item items[20];
	uint32_t n_items = 0;
	snd_ctl_t *ctl_hndl;
	snd_ctl_card_info_t *info;
	struct spa_device_info dinfo;
	struct spa_param_info params[2];
	char path[128];
	char device_name[200];
	char device_desc[200];

	spa_log_debug(this->log, "open card %s", this->props.device);
	if ((err = snd_ctl_open(&ctl_hndl, this->props.device, 0)) < 0) {
		spa_log_error(this->log, "can't open control for card %s: %s",
		              this->props.device, snd_strerror(err));
		return err;
	}

	snd_ctl_card_info_alloca(&info);
	if ((err = snd_ctl_card_info(ctl_hndl, info)) < 0) {
		spa_log_error(this->log, "error hardware info: %s", snd_strerror(err));
		goto finish;
	}

	dinfo = SPA_DEVICE_INFO_INIT();

	dinfo.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS;

	snprintf(path, sizeof(path), "alsa:compressed:%s", snd_ctl_card_info_get_id(info));
	snprintf(device_name, sizeof(device_name), "comprC%u", this->props.card_nr);
	snprintf(device_desc, sizeof(device_desc), "Compress-Offload device (ALSA card %u)", this->props.card_nr);

	ADD_DICT_ITEM(SPA_KEY_OBJECT_PATH,              path);
	ADD_DICT_ITEM(SPA_KEY_DEVICE_API,               "alsa:compressed");
	ADD_DICT_ITEM(SPA_KEY_DEVICE_NICK,              "alsa:compressed");
	ADD_DICT_ITEM(SPA_KEY_DEVICE_NAME,              device_name);
	ADD_DICT_ITEM(SPA_KEY_DEVICE_DESCRIPTION,       device_desc);
	ADD_DICT_ITEM(SPA_KEY_MEDIA_CLASS,              "Audio/Device");
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_PATH,	        (char *)this->props.device);
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_CARD_ID,         snd_ctl_card_info_get_id(info));
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_CARD_COMPONENTS, snd_ctl_card_info_get_components(info));
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_CARD_DRIVER,     snd_ctl_card_info_get_driver(info));
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_CARD_NAME,       snd_ctl_card_info_get_name(info));
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_CARD_LONGNAME,   snd_ctl_card_info_get_longname(info));
	ADD_DICT_ITEM(SPA_KEY_API_ALSA_CARD_MIXERNAME,  snd_ctl_card_info_get_mixername(info));

	dinfo.props = &SPA_DICT_INIT(items, n_items);

	dinfo.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
	params[1] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_READWRITE);
	dinfo.n_params = SPA_N_ELEMENTS(params);
	dinfo.params = params;

	spa_device_emit_info(&this->hooks, &dinfo);

finish:
	spa_log_debug(this->log, "close card %s", this->props.device);
	snd_ctl_close(ctl_hndl);
	return err;
}

static int impl_add_listener(void *object,
                             struct spa_hook *listener,
                             const struct spa_device_events *events,
                             void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	if (events->info || events->object_info)
		emit_info(this, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static struct spa_pod *build_profile(struct impl *this, struct spa_pod_builder *b,
                                     uint32_t id, uint32_t index)
{
	struct spa_pod_frame f[2];
	const char *name, *desc;

	switch (index) {
	case 0:
		name = "off";
		desc = "Off";
		break;
	case 1:
		name = "on";
		desc = "On";
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamProfile, id);
	spa_pod_builder_add(b,
		SPA_PARAM_PROFILE_index,   SPA_POD_Int(index),
		SPA_PARAM_PROFILE_name, SPA_POD_String(name),
		SPA_PARAM_PROFILE_description, SPA_POD_String(desc),
		0);
	if (index == 1) {
		spa_pod_builder_prop(b, SPA_PARAM_PROFILE_classes, 0);
		spa_pod_builder_push_struct(b, &f[1]);
		if (this->n_capture) {
			spa_pod_builder_add_struct(b,
				SPA_POD_String("Audio/Source"),
				SPA_POD_Int(this->n_capture));
		}
		if (this->n_playback) {
			spa_pod_builder_add_struct(b,
				SPA_POD_String("Audio/Sink"),
				SPA_POD_Int(this->n_playback));
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	return spa_pod_builder_pop(b, &f[0]);

}

static int impl_enum_params(void *object, int seq,
                            uint32_t id, uint32_t start, uint32_t num,
                            const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_device_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
	{
		switch (result.index) {
		case 0:
		case 1:
			param = build_profile(this, &b, id, result.index);
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Profile:
	{
		switch (result.index) {
		case 0:
			param = build_profile(this, &b, id, this->profile);
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0,
			SPA_RESULT_TYPE_DEVICE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_set_param(void *object,
                          uint32_t id, uint32_t flags,
                          const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Profile:
	{
		uint32_t idx;

		if ((res = spa_pod_parse_object(param,
		                                SPA_TYPE_OBJECT_ParamProfile, NULL,
		                                SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx))) < 0) {
			spa_log_warn(this->log, "can't parse profile");
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_DEBUG, 0, NULL, param);
			return res;
		}

		set_profile(this, idx);
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
	.enum_params = impl_enum_params,
	.set_param = impl_set_param,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Device))
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory,
                            const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int impl_init(const struct spa_handle_factory *factory,
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

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	alsa_log_topic_init(this->log);

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);
	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	snd_config_update_free_global();

	if (info) {
		uint32_t i;
		for (i = 0; info && i < info->n_items; i++) {
			const char *k = info->items[i].key;
			const char *s = info->items[i].value;

			if (spa_streq(k, SPA_KEY_API_ALSA_PATH)) {
				snprintf(this->props.device, 64, "%s", s);
				spa_log_debug(this->log, "using ALSA path \"%s\"", this->props.device);
			} else if (spa_streq(k, SPA_KEY_API_ALSA_CARD)) {
				long long card_nr = strtol(s, NULL, 10);
				if ((card_nr >= 0) && (card_nr <= UINT_MAX)) {
					this->props.card_nr = card_nr;
					spa_log_debug(this->log, "using ALSA card number %u", this->props.card_nr);
				} else
					spa_log_warn(this->log, "invalid ALSA card number \"%s\"; using default", s);
			}
		}
	}

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_alsa_compress_offload_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_ALSA_COMPRESS_OFFLOAD_DEVICE,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
