/* Spa ALSA Sink */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stddef.h>

#include <alsa/asoundlib.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/monitor/device.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/param/audio/format.h>
#include <spa/pod/filter.h>

#include "alsa-pcm.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_INPUT && (p) == 0)

static const char default_device[] = "hw:0";

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
	props->use_chmap = DEFAULT_USE_CHMAP;
}

static void emit_node_info(struct state *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		struct spa_dict_item items[7];
		uint32_t i, n_items = 0;
		char latency[64], period[64], nperiods[64], headroom[64];

		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "alsa");
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Sink");
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_NODE_DRIVER, "true");
		if (this->have_format) {
			snprintf(latency, sizeof(latency), "%lu/%d", this->buffer_frames / 2, this->rate);
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_NODE_MAX_LATENCY, latency);
			snprintf(period, sizeof(period), "%lu", this->period_frames);
			items[n_items++] = SPA_DICT_ITEM_INIT("api.alsa.period-size", period);
			snprintf(nperiods, sizeof(nperiods), "%lu",
					this->period_frames != 0 ? this->buffer_frames / this->period_frames : 0);
			items[n_items++] = SPA_DICT_ITEM_INIT("api.alsa.period-num", nperiods);
			snprintf(headroom, sizeof(headroom), "%u", this->headroom);
			items[n_items++] = SPA_DICT_ITEM_INIT("api.alsa.headroom", headroom);
		}
		this->info.props = &SPA_DICT_INIT(items, n_items);

		if (this->info.change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
			for (i = 0; i < this->info.n_params; i++) {
				if (this->params[i].user > 0) {
					this->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					this->params[i].user = 0;
				}
			}
		}
		spa_node_emit_info(&this->hooks, &this->info);

		this->info.change_mask = old;
	}
}

static void emit_port_info(struct state *this, bool full)
{
	uint64_t old = full ? this->port_info.change_mask : 0;

	if (full)
		this->port_info.change_mask = this->port_info_all;
	if (this->port_info.change_mask) {
		uint32_t i;

		if (this->port_info.change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
			for (i = 0; i < this->port_info.n_params; i++) {
				if (this->port_params[i].user > 0) {
					this->port_params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					this->port_params[i].user = 0;
				}
			}
		}
		spa_node_emit_port_info(&this->hooks,
				SPA_DIRECTION_INPUT, 0, &this->port_info);
		this->port_info.change_mask = old;
	}
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct state *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_device),
				SPA_PROP_INFO_name, SPA_POD_String(SPA_KEY_API_ALSA_PATH),
				SPA_PROP_INFO_description, SPA_POD_String("The ALSA device"),
				SPA_PROP_INFO_type, SPA_POD_Stringn(p->device, sizeof(p->device)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_deviceName),
				SPA_PROP_INFO_description, SPA_POD_String("The ALSA device name"),
				SPA_PROP_INFO_type, SPA_POD_Stringn(p->device_name, sizeof(p->device_name)));
			break;
		case 2:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_cardName),
				SPA_PROP_INFO_description, SPA_POD_String("The ALSA card name"),
				SPA_PROP_INFO_type, SPA_POD_Stringn(p->card_name, sizeof(p->card_name)));
			break;
		case 3:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_latencyOffsetNsec),
				SPA_PROP_INFO_description, SPA_POD_String("Latency offset (ns)"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Long(0LL, 0LL, 2 * SPA_NSEC_PER_SEC));
			break;
		case 4:
			if (!this->is_iec958 && !this->is_hdmi)
				goto next;
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_iec958Codecs),
				SPA_PROP_INFO_name, SPA_POD_String("iec958.codecs"),
				SPA_PROP_INFO_description, SPA_POD_String("Enabled IEC958 (S/PDIF) codecs"),
				SPA_PROP_INFO_type, SPA_POD_Id(SPA_AUDIO_IEC958_CODEC_UNKNOWN),
                                SPA_PROP_INFO_params, SPA_POD_Bool(true),
                                SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		default:
			param = spa_alsa_enum_propinfo(this, result.index - 5, &b);
			if (param == NULL)
				return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;
		struct spa_pod_frame f;
		uint32_t codecs[16], n_codecs;

		switch (result.index) {
		case 0:
			spa_pod_builder_push_object(&b, &f,
                                SPA_TYPE_OBJECT_Props, id);
			spa_pod_builder_add(&b,
				SPA_PROP_device,       SPA_POD_Stringn(p->device, sizeof(p->device)),
				SPA_PROP_deviceName,   SPA_POD_Stringn(p->device_name, sizeof(p->device_name)),
				SPA_PROP_cardName,     SPA_POD_Stringn(p->card_name, sizeof(p->card_name)),
				SPA_PROP_latencyOffsetNsec,   SPA_POD_Long(this->process_latency.ns),
				0);

			if (this->is_iec958 || this->is_hdmi) {
				n_codecs = spa_alsa_get_iec958_codecs(this, codecs, SPA_N_ELEMENTS(codecs));
				spa_pod_builder_prop(&b, SPA_PROP_iec958Codecs, 0);
				spa_pod_builder_array(&b, sizeof(uint32_t), SPA_TYPE_Id,
						n_codecs, codecs);
			}
			spa_alsa_add_prop_params(this, &b);
			param = spa_pod_builder_pop(&b, &f);
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Clock),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_clock)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Position),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_position)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_ProcessLatency:
		switch (result.index) {
		case 0:
			param = spa_process_latency_build(&b, id, &this->process_latency);
			break;
		default:
			return 0;
		}
		break;

	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		this->clock = data;
		break;
	case SPA_IO_Position:
		this->position = data;
		break;
	default:
		return -ENOENT;
	}
	spa_alsa_reassign_follower(this);

	return 0;
}

static void handle_process_latency(struct state *this,
		const struct spa_process_latency_info *info)
{
	bool ns_changed = this->process_latency.ns != info->ns;

	if (this->process_latency.quantum == info->quantum &&
	    this->process_latency.rate == info->rate &&
	    !ns_changed)
		return;

	this->process_latency = *info;

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	if (ns_changed)
		this->params[NODE_Props].user++;
	this->params[NODE_ProcessLatency].user++;

	this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	this->port_params[PORT_Latency].user++;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct state *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;
		struct spa_pod *iec958_codecs = NULL, *params = NULL;
		int64_t lat_ns = -1;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}

		spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_device,       SPA_POD_OPT_Stringn(p->device, sizeof(p->device)),
			SPA_PROP_latencyOffsetNsec,   SPA_POD_OPT_Long(&lat_ns),
			SPA_PROP_iec958Codecs, SPA_POD_OPT_Pod(&iec958_codecs),
			SPA_PROP_params,       SPA_POD_OPT_Pod(&params));

		if ((this->is_iec958 || this->is_hdmi) && iec958_codecs != NULL) {
			uint32_t i, codecs[16], n_codecs;
			n_codecs = spa_pod_copy_array(iec958_codecs, SPA_TYPE_Id,
					codecs, SPA_N_ELEMENTS(codecs));
			this->iec958_codecs = 1ULL << SPA_AUDIO_IEC958_CODEC_PCM;
			for (i = 0; i < n_codecs; i++)
				this->iec958_codecs |= 1ULL << codecs[i];

			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			this->params[NODE_Props].user++;

			this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			this->port_params[PORT_EnumFormat].user++;
		}
		spa_alsa_parse_prop_params(this, params);
		if (lat_ns != -1) {
			struct spa_process_latency_info info;
			info = this->process_latency;
			info.ns = lat_ns;
			handle_process_latency(this, &info);
		}
		emit_node_info(this, false);
		emit_port_info(this, false);
		break;
	}
	case SPA_PARAM_ProcessLatency:
	{
		struct spa_process_latency_info info;
		if (param == NULL)
			spa_zero(info);
		else if ((res = spa_process_latency_parse(param, &info)) < 0)
			return res;

		handle_process_latency(this, &info);

		emit_node_info(this, false);
		emit_port_info(this, false);
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct state *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_ParamBegin:
		if ((res = spa_alsa_open(this, NULL)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_ParamEnd:
		if (this->have_format)
			return 0;
		if ((res = spa_alsa_close(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Start:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if ((res = spa_alsa_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		if ((res = spa_alsa_pause(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}


static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct state *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
impl_node_sync(void *object, int seq)
{
	struct state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_node_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{

	struct state *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		return spa_alsa_enum_format(this, seq, start, num, filter);

	case SPA_PARAM_Format:
		if (!this->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		switch (this->current_format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
			param = spa_format_audio_raw_build(&b, id,
					&this->current_format.info.raw);
			break;
		case SPA_MEDIA_SUBTYPE_iec958:
			param = spa_format_audio_iec958_build(&b, id,
					&this->current_format.info.iec958);
			break;
		case SPA_MEDIA_SUBTYPE_dsd:
			param = spa_format_audio_dsd_build(&b, id,
					&this->current_format.info.dsd);
			break;
		default:
			return -EIO;
		}
		break;

	case SPA_PARAM_Buffers:
		if (!this->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(this->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							this->quantum_limit * this->frame_size * this->frame_scale,
							16 * this->frame_size * this->frame_scale,
							INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(this->frame_size));
		break;

	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_RateMatch),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_rate_match)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0: case 1:
		{
			struct spa_latency_info latency = this->latency[result.index];
			if (latency.direction == SPA_DIRECTION_INPUT)
				spa_process_latency_info_add(&this->process_latency, &latency);
			param = spa_latency_build(&b, id, &latency);
			break;
		}
		default:
			return 0;
		}
		break;

	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct state *this)
{
	if (this->n_buffers > 0) {
		spa_list_init(&this->ready);
		this->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(void *object,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct state *this = object;
	int err = 0;

	if (format == NULL) {
		if (!this->have_format)
			return 0;

		spa_log_debug(this->log, "clear format");
		spa_alsa_close(this);
		clear_buffers(this);
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_audio)
			return -EINVAL;

		switch (info.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
			if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
				return -EINVAL;
			break;
		case SPA_MEDIA_SUBTYPE_iec958:
			if (spa_format_audio_iec958_parse(format, &info.info.iec958) < 0)
				return -EINVAL;
			break;
		case SPA_MEDIA_SUBTYPE_dsd:
			if (spa_format_audio_dsd_parse(format, &info.info.dsd) < 0)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}

		if ((err = spa_alsa_set_format(this, &info, flags)) < 0)
			return err;

		this->current_format = info;
	}

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
	emit_node_info(this, false);

	this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
	this->port_info.rate = SPA_FRACTION(1, this->rate);
	this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (this->have_format) {
		this->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		this->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
		this->port_params[PORT_Latency].user++;
	} else {
		this->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		this->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, false);

	return err;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct state *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Format:
		res = port_set_format(this, direction, port_id, flags, param);
		break;
	case SPA_PARAM_Latency:
	{
		struct spa_latency_info info;
		if (param == NULL)
			info = SPA_LATENCY_INFO(SPA_DIRECTION_REVERSE(direction));
		else if ((res = spa_latency_parse(param, &info)) < 0)
			return res;
		if (direction == info.direction)
			return -EINVAL;

		this->latency[info.direction] = info;
		this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		this->port_params[PORT_Latency].user++;
		emit_port_info(this, false);
		res = 0;
		break;
	}
	default:
		res = -ENOENT;
		break;
	}
	return res;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct state *this = object;
	uint32_t i;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	spa_log_debug(this->log, "%p: use %d buffers", this, n_buffers);

	if (this->n_buffers > 0) {
		spa_alsa_pause(this);
		if ((res = clear_buffers(this)) < 0)
			return res;
	}
	if (n_buffers > 0 && !this->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		struct spa_data *d = buffers[i]->datas;

		b->buf = buffers[i];
		b->id = i;
		b->flags = BUFFER_FLAG_OUT;

		b->h = spa_buffer_find_meta_data(b->buf, SPA_META_Header, sizeof(*b->h));

		if (d[0].data == NULL) {
			spa_log_error(this->log, "%p: need mapped memory", this);
			return -EINVAL;
		}
		spa_log_debug(this->log, "%p: %d %p data:%p", this, i, b->buf, d[0].data);
	}
	this->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct state *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	spa_log_debug(this->log, "%p: io %d %p %zd", this, id, data, size);

	switch (id) {
	case SPA_IO_Buffers:
		this->io = data;
		break;
	case SPA_IO_RateMatch:
		this->rate_match = data;
		spa_alsa_update_rate_match(this);
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	return -ENOTSUP;
}

static int impl_node_process(void *object)
{
	struct state *this = object;
	struct spa_io_buffers *io;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if ((io = this->io) == NULL)
		return -EIO;

	spa_log_trace_fp(this->log, "%p: process %d %d/%d", this, io->status,
			io->buffer_id, this->n_buffers);

	if (this->position && this->position->clock.flags & SPA_IO_CLOCK_FLAG_FREEWHEEL) {
		io->status = SPA_STATUS_NEED_DATA;
		return SPA_STATUS_HAVE_DATA;
	}
	if (io->status == SPA_STATUS_HAVE_DATA &&
	    io->buffer_id < this->n_buffers) {
		struct buffer *b = &this->buffers[io->buffer_id];

		if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
			spa_log_warn(this->log, "%p: buffer %u in use",
					this, io->buffer_id);
			io->status = -EINVAL;
			return -EINVAL;
		}
		spa_log_trace_fp(this->log, "%p: queue buffer %u", this, io->buffer_id);
		spa_list_append(&this->ready, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
		io->buffer_id = SPA_ID_INVALID;

		spa_alsa_write(this);

		io->status = SPA_STATUS_OK;
	}
	else if (!spa_list_is_empty(&this->ready)) {
		spa_alsa_write(this);

		io->status = SPA_STATUS_OK;
	}
	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct state *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct state *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct state *this;
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	this = (struct state *) handle;
	spa_alsa_close(this);
	spa_alsa_clear(this);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct state);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle, const struct spa_dict *info, const struct spa_support *support, uint32_t n_support)
{
	struct state *this;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct state *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	alsa_log_topic_init(this->log);

	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data system is needed");
		return -EINVAL;
	}

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	spa_hook_list_init(&this->hooks);

	this->stream = SND_PCM_STREAM_PLAYBACK;
	this->port_direction = SPA_DIRECTION_INPUT;
	this->latency[this->port_direction] = SPA_LATENCY_INFO(
			this->port_direction,
			.min_quantum = 1.0f,
			.max_quantum = 1.0f);
	this->latency[SPA_DIRECTION_OUTPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[NODE_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[NODE_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[NODE_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->params[NODE_ProcessLatency] = SPA_PARAM_INFO(SPA_PARAM_ProcessLatency, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	reset_props(&this->props);

	this->port_info_all = SPA_PORT_CHANGE_MASK_FLAGS |
				 SPA_PORT_CHANGE_MASK_PARAMS;
	this->port_info = SPA_PORT_INFO_INIT();
	this->port_info.flags = SPA_PORT_FLAG_LIVE |
			   SPA_PORT_FLAG_PHYSICAL |
			   SPA_PORT_FLAG_TERMINAL;
	this->port_params[PORT_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->port_params[PORT_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	this->port_params[PORT_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	this->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	this->port_params[PORT_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	this->port_info.params = this->port_params;
	this->port_info.n_params = N_PORT_PARAMS;

	spa_list_init(&this->ready);

	return spa_alsa_init(this, info);
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info, uint32_t *index)
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

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Play audio with the alsa API" },
	{ SPA_KEY_FACTORY_USAGE, "["SPA_KEY_API_ALSA_PATH"=<path>]" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_alsa_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_ALSA_PCM_SINK,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
