/* SPA */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/support/plugin.h>
#include <spa/support/plugin-loader.h>
#include <spa/support/log.h>
#include <spa/support/cpu.h>

#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/buffer/alloc.h>
#include <spa/pod/parser.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/param/param.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/debug/format.h>
#include <spa/debug/pod.h>
#include <spa/debug/log.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.audioadapter");

#define DEFAULT_ALIGN	16

#define MAX_PORTS	(SPA_AUDIO_MAX_CHANNELS+1)
#define MAX_RETRY	64

/** \cond */

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;
	struct spa_plugin_loader *ploader;

	uint32_t max_align;
	enum spa_direction direction;

	struct spa_node *target;

	struct spa_node *follower;
	struct spa_hook follower_listener;
	uint64_t follower_flags;
	struct spa_audio_info follower_current_format;
	struct spa_audio_info default_format;
	int in_set_param;

	struct spa_handle *hnd_convert;
	bool unload_handle;
	struct spa_node *convert;
	struct spa_hook convert_listener;
	uint64_t convert_port_flags;
	char *convertname;

	uint32_t n_buffers;
	struct spa_buffer **buffers;

	struct spa_io_buffers io_buffers;
	struct spa_io_rate_match io_rate_match;
	struct spa_io_position *io_position;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_EnumFormat		0
#define IDX_PropInfo		1
#define IDX_Props		2
#define IDX_Format		3
#define IDX_EnumPortConfig	4
#define IDX_PortConfig		5
#define IDX_Latency		6
#define IDX_ProcessLatency	7
#define IDX_Tag			8
#define N_NODE_PARAMS		9
	struct spa_param_info params[N_NODE_PARAMS];
	uint32_t convert_params_flags[N_NODE_PARAMS];
	uint32_t follower_params_flags[N_NODE_PARAMS];
	uint64_t follower_port_flags;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	unsigned int add_listener:1;
	unsigned int have_rate_match:1;
	unsigned int have_format:1;
	unsigned int recheck_format:1;
	unsigned int started:1;
	unsigned int ready:1;
	unsigned int async:1;
	enum spa_param_port_config_mode mode;
	unsigned int follower_removing:1;
	unsigned int in_recalc;

	unsigned int warned:1;
	unsigned int driver:1;

	int in_enum_sync;
};

/** \endcond */

static int node_enum_params_sync(struct impl *impl, struct spa_node *node,
		uint32_t id, uint32_t *index, const struct spa_pod *filter,
		struct spa_pod **param, struct spa_pod_builder *builder)
{
	int res;
	impl->in_enum_sync++;
	res = spa_node_enum_params_sync(node, id, index, filter, param, builder);
	impl->in_enum_sync--;
	return res;
}

static int node_port_enum_params_sync(struct impl *impl, struct spa_node *node,
		enum spa_direction direction, uint32_t port_id,
		uint32_t id, uint32_t *index, const struct spa_pod *filter,
		struct spa_pod **param, struct spa_pod_builder *builder)
{
	int res;
	impl->in_enum_sync++;
	res = spa_node_port_enum_params_sync(node, direction, port_id, id, index,
			filter, param, builder);
	impl->in_enum_sync--;
	return res;
}

static int follower_enum_params(struct impl *this,
				 uint32_t id,
				 uint32_t idx,
				 struct spa_result_node_params *result,
				 const struct spa_pod *filter,
				 struct spa_pod_builder *builder)
{
	int res;
	if (result->next < 0x100000) {
		if (this->follower != this->target &&
		    this->convert_params_flags[idx] & SPA_PARAM_INFO_READ) {
			if ((res = node_enum_params_sync(this, this->target,
					id, &result->next, filter, &result->param, builder)) == 1)
				return res;
		}
		result->next = 0x100000;
	}
	if (result->next < 0x200000) {
		if (this->follower_params_flags[idx] & SPA_PARAM_INFO_READ) {
			result->next &= 0xfffff;
			if ((res = node_enum_params_sync(this, this->follower,
					id, &result->next, filter, &result->param, builder)) == 1) {
				result->next |= 0x100000;
				return res;
			}
		}
		result->next = 0x200000;
	}
	return 0;
}

static int convert_enum_port_config(struct impl *this,
		int seq, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter, struct spa_pod_builder *builder)
{
	struct spa_pod *f1, *f2 = NULL;
	int res;

	if (this->convert == NULL)
		return 0;

	f1 = spa_pod_builder_add_object(builder,
		SPA_TYPE_OBJECT_ParamPortConfig, id,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(this->direction));

	if (filter) {
		if ((res = spa_pod_filter(builder, &f2, f1, filter)) < 0)
			return res;
	}
	else {
		f2 = f1;
	}
	return spa_node_enum_params(this->convert, seq, id, start, num, f2);
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	uint8_t buffer[4096];
	spa_auto(spa_pod_dynamic_builder) b = { 0 };
	struct spa_pod_builder_state state;
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
	spa_pod_builder_get_state(&b.b, &state);

	result.id = id;
	result.next = start;
next:
	result.index = result.next;

	spa_log_debug(this->log, "%p: %d id:%u", this, seq, id);

	spa_pod_builder_reset(&b.b, &state);

	switch (id) {
	case SPA_PARAM_EnumPortConfig:
	case SPA_PARAM_PortConfig:
		if (this->mode == SPA_PARAM_PORT_CONFIG_MODE_passthrough) {
			switch (result.index) {
			case 0:
				result.param = spa_pod_builder_add_object(&b.b,
					SPA_TYPE_OBJECT_ParamPortConfig, id,
					SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(this->direction),
					SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(
						SPA_PARAM_PORT_CONFIG_MODE_passthrough));
				result.next++;
				res = 1;
				break;
			default:
				return 0;
			}
		} else {
			return convert_enum_port_config(this, seq, id, start, num, filter, &b.b);
		}
		break;
	case SPA_PARAM_PropInfo:
		res = follower_enum_params(this,
				id, IDX_PropInfo, &result, filter, &b.b);
		break;
	case SPA_PARAM_Props:
		res = follower_enum_params(this,
				id, IDX_Props, &result, filter, &b.b);
		break;
	case SPA_PARAM_ProcessLatency:
		res = follower_enum_params(this,
				id, IDX_ProcessLatency, &result, filter, &b.b);
		break;
	case SPA_PARAM_EnumFormat:
	case SPA_PARAM_Format:
	case SPA_PARAM_Latency:
	case SPA_PARAM_Tag:
		res = node_port_enum_params_sync(this, this->follower,
				this->direction, 0,
				id, &result.next, filter, &result.param, &b.b);
		break;
	default:
		return -ENOENT;
	}
	if (res != 1)
		return res;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
	count++;

	if (count != num)
		goto next;

	return 0;
}

static int link_io(struct impl *this)
{
	int res;
	struct spa_io_rate_match *rate_match;
	size_t rate_match_size;

	spa_log_debug(this->log, "%p: controls", this);

	spa_zero(this->io_rate_match);
	this->io_rate_match.rate = 1.0;

	if (this->follower == this->target || !this->have_rate_match) {
		rate_match = NULL;
		rate_match_size = 0;
	} else {
		rate_match = &this->io_rate_match;
		rate_match_size = sizeof(this->io_rate_match);
	}

	if ((res = spa_node_port_set_io(this->follower,
			this->direction, 0,
			SPA_IO_RateMatch,
			rate_match, rate_match_size)) < 0) {
		spa_log_debug(this->log, "%p: set RateMatch on follower disabled %d %s", this,
			res, spa_strerror(res));
	}
	else if (this->follower != this->target) {
		if ((res = spa_node_port_set_io(this->target,
				SPA_DIRECTION_REVERSE(this->direction), 0,
				SPA_IO_RateMatch,
				rate_match, rate_match_size)) < 0) {
			spa_log_warn(this->log, "%p: set RateMatch on target failed %d %s", this,
				res, spa_strerror(res));
		}
	}
	return 0;
}

static int activate_io(struct impl *this, bool active)
{
	int res;
	struct spa_io_buffers *data = active ? &this->io_buffers : NULL;
	uint32_t size = active ? sizeof(this->io_buffers) : 0;

	if (this->follower == this->target)
		return 0;

	if (active)
		this->io_buffers = SPA_IO_BUFFERS_INIT;

	if ((res = spa_node_port_set_io(this->follower,
			this->direction, 0,
			SPA_IO_Buffers, data, size)) < 0) {
		spa_log_warn(this->log, "%p: set Buffers on follower failed %d %s", this,
			res, spa_strerror(res));
		return res;
	}
	else if ((res = spa_node_port_set_io(this->target,
			SPA_DIRECTION_REVERSE(this->direction), 0,
			SPA_IO_Buffers, data, size)) < 0) {
		spa_log_warn(this->log, "%p: set Buffers on convert failed %d %s", this,
			res, spa_strerror(res));
		return res;
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full)
{
	uint32_t i;
	uint64_t old = full ? this->info.change_mask : 0;

	spa_log_debug(this->log, "%p: info full:%d change:%08"PRIx64,
			this, full, this->info.change_mask);

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		struct spa_dict_item *items;
		uint32_t n_items = 0;

		if (this->info.props)
			n_items = this->info.props->n_items;
		items = alloca((n_items + 2) * sizeof(struct spa_dict_item));
		for (i = 0; i < n_items; i++)
			items[i] = this->info.props->items[i];
		items[n_items++] = SPA_DICT_ITEM_INIT("adapter.auto-port-config", NULL);
		items[n_items++] = SPA_DICT_ITEM_INIT("audio.adapt.follower", NULL);
		this->info.props = &SPA_DICT_INIT(items, n_items);

		if (this->info.change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
			for (i = 0; i < this->info.n_params; i++) {
				if (this->params[i].user > 0) {
					this->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
					this->params[i].user = 0;
					spa_log_debug(this->log, "param %d flags:%08x",
							i, this->params[i].flags);
				}
			}
		}
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
		spa_zero(this->info.props);
	}
}

static int debug_params(struct impl *this, struct spa_node *node,
                enum spa_direction direction, uint32_t port_id, uint32_t id, struct spa_pod *filter,
		const char *debug, int err)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	uint32_t state;
	struct spa_pod *param;
	int res, count = 0;

	spa_log_error(this->log, "params %s: %d:%d (%s) %s",
			spa_debug_type_find_name(spa_type_param, id),
			direction, port_id, debug, err ? spa_strerror(err) : "no matching params");
	if (err == -EBUSY)
		return 0;

	if (filter) {
		spa_log_error(this->log, "with this filter:");
		spa_debug_log_pod(this->log, SPA_LOG_LEVEL_ERROR, 2, NULL, filter);
	} else {
		spa_log_error(this->log, "there was no filter");
	}

	state = 0;
	while (true) {
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		res = node_port_enum_params_sync(this, node,
					direction, port_id,
					id, &state,
					NULL, &param, &b);
		if (res != 1) {
			if (res < 0)
				spa_log_error(this->log, "  error: %s", spa_strerror(res));
			break;
		}
		spa_log_error(this->log, "unmatched %s %d:", debug, count);
		spa_debug_log_pod(this->log, SPA_LOG_LEVEL_ERROR, 2, NULL, param);
		count++;
	}
	if (count == 0)
		spa_log_error(this->log, "could not get any %s", debug);

	return 0;
}

static int negotiate_buffers(struct impl *this)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t state;
	struct spa_pod *param;
	int res;
	bool follower_alloc, conv_alloc;
	uint32_t i, size, buffers, blocks, align, flags, stride = 0;
	uint32_t *aligns, data_flags;
	struct spa_data *datas;
	uint64_t follower_flags, conv_flags;
	struct spa_node *alloc_node;
	enum spa_direction alloc_direction;
	uint32_t alloc_flags;

	spa_log_debug(this->log, "%p: n_buffers:%d", this, this->n_buffers);

	if (this->follower == this->target)
		return 0;

	if (this->n_buffers > 0)
		return 0;

	state = 0;
	param = NULL;
	if ((res = node_port_enum_params_sync(this, this->target,
				SPA_DIRECTION_REVERSE(this->direction), 0,
				SPA_PARAM_Buffers, &state,
				param, &param, &b)) < 0) {
		if (res == -ENOENT)
			param = NULL;
		else {
			debug_params(this, this->target,
				SPA_DIRECTION_REVERSE(this->direction), 0,
				SPA_PARAM_Buffers, param, "target buffers", res);
			return res;
		}
	}

	state = 0;
	if ((res = node_port_enum_params_sync(this, this->follower,
				this->direction, 0,
				SPA_PARAM_Buffers, &state,
				param, &param, &b)) != 1) {
		if (res == -ENOENT)
			res = 0;
		else {
			debug_params(this, this->follower, this->direction, 0,
				SPA_PARAM_Buffers, param, "follower buffers", res);
			return res < 0 ? res : -ENOTSUP;
		}
	}
	if (param == NULL)
		return -ENOTSUP;

	spa_pod_fixate(param);

	follower_flags = this->follower_port_flags;
	conv_flags = this->convert_port_flags;

	follower_alloc = SPA_FLAG_IS_SET(follower_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);
	conv_alloc = SPA_FLAG_IS_SET(conv_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);

	flags = alloc_flags = 0;
	if (conv_alloc || follower_alloc) {
		flags |= SPA_BUFFER_ALLOC_FLAG_NO_DATA;
		alloc_flags = SPA_NODE_BUFFERS_FLAG_ALLOC;
	}

	align = DEFAULT_ALIGN;

	if ((res = spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_ParamBuffers, NULL,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(&buffers),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(&blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int(&size),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(&stride),
			SPA_PARAM_BUFFERS_align,   SPA_POD_OPT_Int(&align))) < 0)
		return res;

	if (this->async)
		buffers = SPA_MAX(2u, buffers);

	spa_log_info(this->log, "%p: buffers:%d, blocks:%d, size:%d, stride:%d align:%d %d:%d",
			this, buffers, blocks, size, stride, align, follower_alloc, conv_alloc);

	align = SPA_MAX(align, this->max_align);

	datas = alloca(sizeof(struct spa_data) * blocks);
	memset(datas, 0, sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);

	data_flags = SPA_DATA_FLAG_READWRITE;
	if (SPA_FLAG_IS_SET(follower_flags, SPA_PORT_FLAG_DYNAMIC_DATA) &&
	    SPA_FLAG_IS_SET(conv_flags, SPA_PORT_FLAG_DYNAMIC_DATA))
		data_flags |= SPA_DATA_FLAG_DYNAMIC;

	for (i = 0; i < blocks; i++) {
		datas[i].type = SPA_DATA_MemPtr;
		datas[i].flags = data_flags;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	free(this->buffers);
	this->buffers = spa_buffer_alloc_array(buffers, flags, 0, NULL, blocks, datas, aligns);
	if (this->buffers == NULL)
		return -errno;
	this->n_buffers = buffers;

	/* prefer to let the follower alloc */
	if (follower_alloc) {
		alloc_node = this->follower;
		alloc_direction = this->direction;
	} else {
		alloc_node = this->target;
		alloc_direction = SPA_DIRECTION_REVERSE(this->direction);
	}

	if ((res = spa_node_port_use_buffers(alloc_node,
		       alloc_direction, 0, alloc_flags,
		       this->buffers, this->n_buffers)) < 0)
		return res;

	alloc_node = alloc_node == this->follower ? this->target : this->follower;
	alloc_direction = SPA_DIRECTION_REVERSE(alloc_direction);
	alloc_flags = 0;

	if ((res = spa_node_port_use_buffers(alloc_node,
		       alloc_direction, 0, alloc_flags,
		       this->buffers, this->n_buffers)) < 0)
		return res;

	activate_io(this, true);

	return 0;
}

static void clear_buffers(struct impl *this)
{
	free(this->buffers);
	this->buffers = NULL;
	this->n_buffers = 0;
}

static int configure_format(struct impl *this, uint32_t flags, const struct spa_pod *format)
{
	uint8_t buffer[4096];
	int res;

	spa_log_debug(this->log, "%p: configure format:", this);

	if (format == NULL) {
		if (!this->have_format)
			return 0;
		activate_io(this, false);
	}
	else {
		spa_debug_log_format(this->log, SPA_LOG_LEVEL_DEBUG, 0, NULL, format);
	}

	if ((res = spa_node_port_set_param(this->follower,
					   this->direction, 0,
					   SPA_PARAM_Format, flags,
					   format)) < 0)
			return res;

	if (res > 0) {
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		uint32_t state = 0;
		struct spa_pod *fmt;

		/* format was changed to nearest compatible format */

		if ((res = node_port_enum_params_sync(this, this->follower,
					this->direction, 0,
					SPA_PARAM_Format, &state,
					NULL, &fmt, &b)) != 1)
			return -EIO;

		format = fmt;
	}

	if (this->target != this->follower) {
		if ((res = spa_node_port_set_param(this->target,
					   SPA_DIRECTION_REVERSE(this->direction), 0,
					   SPA_PARAM_Format, flags,
					   format)) < 0)
				return res;
	}

	this->have_format = format != NULL;
	clear_buffers(this);

	if (format != NULL)
		res = negotiate_buffers(this);

	return res;
}

static int configure_convert(struct impl *this, uint32_t mode)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;

	if (this->convert == NULL)
		return 0;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_log_debug(this->log, "%p: configure convert %p", this, this->target);

	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(this->direction),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(mode));

	return spa_node_set_param(this->convert, SPA_PARAM_PortConfig, 0, param);
}

extern const struct spa_handle_factory spa_audioconvert_factory;

static const struct spa_node_events follower_node_events;

static int recalc_latency(struct impl *this, struct spa_node *src, enum spa_direction direction,
		uint32_t port_id, struct spa_node *dst)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	uint32_t index = 0;
	struct spa_latency_info latency;
	int res;

	spa_log_debug(this->log, "%p: %d:%d", this, direction, port_id);

	if (this->target == this->follower)
		return 0;

	while (true) {
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if ((res = node_port_enum_params_sync(this, src,
						direction, port_id, SPA_PARAM_Latency,
						&index, NULL, &param, &b)) != 1) {
			param = NULL;
			break;
		}
		if ((res = spa_latency_parse(param, &latency)) < 0)
			return res;
		if (latency.direction == direction)
			break;
	}
	if ((res = spa_node_port_set_param(dst,
					SPA_DIRECTION_REVERSE(direction), 0,
					SPA_PARAM_Latency, 0, param)) < 0)
		return res;

	return 0;
}

static int recalc_tag(struct impl *this, struct spa_node *src, enum spa_direction direction,
		uint32_t port_id, struct spa_node *dst)
{
	spa_auto(spa_pod_dynamic_builder) b = { 0 };
	struct spa_pod_builder_state state;
	uint8_t buffer[2048];
	struct spa_pod *param;
	uint32_t index = 0;
	struct spa_tag_info info;
	int res;

	spa_log_debug(this->log, "%p: %d:%d", this, direction, port_id);

	if (this->target == this->follower)
		return 0;

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 2048);
	spa_pod_builder_get_state(&b.b, &state);

	while (true) {
		void *tag_state = NULL;
		spa_pod_builder_reset(&b.b, &state);
		if ((res = node_port_enum_params_sync(this, src,
						direction, port_id, SPA_PARAM_Tag,
						&index, NULL, &param, &b.b)) != 1) {
			param = NULL;
			break;
		}
		if ((res = spa_tag_parse(param, &info, &tag_state)) < 0)
			return res;
		if (info.direction == direction)
			break;
	}
	return spa_node_port_set_param(dst, SPA_DIRECTION_REVERSE(direction), 0,
					SPA_PARAM_Tag, 0, param);
}


static int reconfigure_mode(struct impl *this, enum spa_param_port_config_mode mode,
                enum spa_direction direction, struct spa_pod *format)
{
	int res = 0;
	struct spa_hook l;
	bool passthrough = mode == SPA_PARAM_PORT_CONFIG_MODE_passthrough;
	bool old_passthrough = this->mode == SPA_PARAM_PORT_CONFIG_MODE_passthrough;

	spa_log_debug(this->log, "%p: passthrough mode %d", this, passthrough);

	if (!passthrough && this->convert == NULL)
		return -ENOTSUP;

	if (old_passthrough != passthrough) {
		if (passthrough) {
			/* remove converter split/merge ports */
			configure_convert(this, SPA_PARAM_PORT_CONFIG_MODE_none);
		} else {
			/* remove follower ports */
			this->follower_removing = true;
			spa_zero(l);
			spa_node_add_listener(this->follower, &l, &follower_node_events, this);
			spa_hook_remove(&l);
			this->follower_removing = false;
		}
	}

	/* set new target */
	this->target = passthrough ? this->follower : this->convert;

	if ((res = configure_format(this, SPA_NODE_PARAM_FLAG_NEAREST, format)) < 0)
		return res;

	this->mode = mode;

	if (old_passthrough != passthrough && passthrough) {
		/* add follower ports */
		spa_zero(l);
		spa_node_add_listener(this->follower, &l, &follower_node_events, this);
		spa_hook_remove(&l);
	} else {
		/* add converter ports */
		configure_convert(this, mode);
	}
	link_io(this);

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PARAMS;
	SPA_FLAG_UPDATE(this->info.flags, SPA_NODE_FLAG_NEED_CONFIGURE,
			this->mode == SPA_PARAM_PORT_CONFIG_MODE_none);
	SPA_FLAG_UPDATE(this->info.flags, SPA_NODE_FLAG_ASYNC,
			this->async && this->follower == this->target);
	this->params[IDX_Props].user++;

	emit_node_info(this, false);

	spa_log_debug(this->log, "%p: passthrough mode %d", this, passthrough);

	return 0;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	int res = 0, res2 = 0;
	struct impl *this = object;
	struct spa_audio_info info = { 0 };

	spa_log_debug(this->log, "%p: set param %d", this, id);

	switch (id) {
	case SPA_PARAM_Format:
		if (this->started) {
			spa_log_error(this->log, "%p: cannot set Format param: "
					"node already started", this);
			return -EIO;
		}
		if (param == NULL) {
			spa_log_error(this->log, "%p: attempted to set NULL Format POD", this);
			return -EINVAL;
		}

		if (spa_format_audio_parse(param, &info) < 0) {
			spa_log_error(this->log, "%p: cannot set Format param: "
					"parsing the POD failed", this);
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_ERROR, 0, NULL, param);
			return -EINVAL;
		}
		if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
			const char *subtype_name = spa_type_to_short_name(info.media_subtype,
									spa_type_media_subtype,
									"<unknown>");
			spa_log_error(this->log, "%p: cannot set Format param: "
					"expected raw subtype, got subtype \"%s\"", this, subtype_name);
			return -EINVAL;
		}

		this->follower_current_format = info;
		break;

	case SPA_PARAM_PortConfig:
	{
		enum spa_direction dir;
		enum spa_param_port_config_mode mode;
		struct spa_pod *format = NULL;

		if (this->started) {
			spa_log_error(this->log, "%p: cannot set PortConfig param: "
					"node already started", this);
			return -EIO;
		}

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamPortConfig, NULL,
				SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(&dir),
				SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(&mode),
				SPA_PARAM_PORT_CONFIG_format,		SPA_POD_OPT_Pod(&format)) < 0) {
			spa_log_error(this->log, "%p: cannot set PortConfig param: "
					"parsing the POD failed", this);
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_ERROR, 0, NULL, param);
			return -EINVAL;
		}

		if (format) {
			struct spa_audio_info info;

			spa_zero(info);
			if ((res = spa_format_audio_parse(format, &info)) < 0) {
				spa_log_error(this->log, "%p: cannot set PortConfig param: "
						"parsing format failed: %s", this, spa_strerror(res));
				spa_debug_log_pod(this->log, SPA_LOG_LEVEL_ERROR, 0, NULL, format);
				return res;
			}

			if (info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
				info.info.raw.rate = 0;
			} else {
				const char *subtype_name = spa_type_to_short_name(info.media_subtype,
										spa_type_media_subtype,
										"<unknown>");
				spa_log_error(this->log, "%p: cannot set PortConfig param: "
						"subtype \"%s\" is not supported", this, subtype_name);
				return -ENOTSUP;
			}

			this->default_format = info;
		}

		switch (mode) {
		case SPA_PARAM_PORT_CONFIG_MODE_none:
			spa_log_error(this->log, "%p: cannot set PortConfig param: "
					"\"none\" config mode is not supported", this);
			return -ENOTSUP;
		case SPA_PARAM_PORT_CONFIG_MODE_passthrough:
			if ((res = reconfigure_mode(this, mode, dir, format)) < 0)
				return res;
			break;
		case SPA_PARAM_PORT_CONFIG_MODE_convert:
		case SPA_PARAM_PORT_CONFIG_MODE_dsp:
			if ((res = reconfigure_mode(this, mode, dir, NULL)) < 0)
				return res;
			break;
		default:
			spa_log_error(this->log, "%p: invalid config mode when setting PortConfig param",
					this);
			return -EINVAL;
		}

		if (this->target != this->follower) {
			if ((res = spa_node_set_param(this->target, id, flags, param)) < 0)
				return res;

			res = recalc_latency(this, this->follower, this->direction, 0, this->target);
		}
		break;
	}

	case SPA_PARAM_Props:
	{
		int in_set_param = ++this->in_set_param;
		res = spa_node_set_param(this->follower, id, flags, param);
		if (this->target != this->follower && this->in_set_param == in_set_param)
			res2 = spa_node_set_param(this->target, id, flags, param);
		if (res < 0 && res2 < 0)
			return res;
		res = 0;
		break;
	}
	case SPA_PARAM_ProcessLatency:
		res = spa_node_set_param(this->follower, id, flags, param);
		break;
	default:
		res = -ENOTSUP;
		break;
	}
	return res;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	int res = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Position:
		this->io_position = data;
		this->recheck_format = true;
		break;
	default:
		break;
	}

	if (this->target)
		res = spa_node_set_io(this->target, id, data, size);

	if (this->target != this->follower)
		res = spa_node_set_io(this->follower, id, data, size);

	return res;
}

static struct spa_pod *merge_objects(struct impl *this, struct spa_pod_builder *b, uint32_t id,
			struct spa_pod_object *o1, struct spa_pod_object *o2)
{
	const struct spa_pod_prop *p1, *p2;
	struct spa_pod_frame f;
	struct spa_pod_builder_state state;
	int res = 0;

	if (o2 == NULL || o1->pod.type != o2->pod.type)
		return (struct spa_pod*)o1;

	spa_pod_builder_push_object(b, &f, o1->body.type, o1->body.id);
	p2 = NULL;
	SPA_POD_OBJECT_FOREACH(o1, p1) {
		p2 = spa_pod_object_find_prop(o2, p2, p1->key);
		if (p2 != NULL) {
			spa_pod_builder_get_state(b, &state);
			res = spa_pod_filter_prop(b, p2, p1);
			if (res < 0)
		                spa_pod_builder_reset(b, &state);
		}
		if (p2 == NULL || res < 0)
			spa_pod_builder_raw_padded(b, p1, SPA_POD_PROP_SIZE(p1));
	}
	p1 = NULL;
	SPA_POD_OBJECT_FOREACH(o2, p2) {
		p1 = spa_pod_object_find_prop(o1, p1, p2->key);
		if (p1 != NULL)
			continue;
		spa_pod_builder_raw_padded(b, p2, SPA_POD_PROP_SIZE(p2));
	}
	return spa_pod_builder_pop(b, &f);
}

static int negotiate_format(struct impl *this)
{
	uint32_t fstate, tstate;
	struct spa_pod *format, *def;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	int res, fres;

	spa_log_debug(this->log, "%p: have_format:%d recheck:%d", this, this->have_format,
			this->recheck_format);

	if (this->target == this->follower)
		return 0;

	if (this->have_format && !this->recheck_format)
		return 0;

	this->recheck_format = false;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_node_send_command(this->follower,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_ParamBegin));

	/* The target has been negotiated on its other ports and so it can propose
	 * a passthrough format or an ideal conversion. We use the suggestions of the
	 * target to find the best follower format */
	for (tstate = 0;;) {
		format = NULL;
		res = node_port_enum_params_sync(this, this->target,
					SPA_DIRECTION_REVERSE(this->direction), 0,
					SPA_PARAM_EnumFormat, &tstate,
					NULL, &format, &b);

		if (res == -ENOENT)
			format = NULL;
		else if (res <= 0)
			break;

		if (format != NULL)
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_DEBUG, 0, NULL, format);

		fstate = 0;
		fres = node_port_enum_params_sync(this, this->follower,
					this->direction, 0,
					SPA_PARAM_EnumFormat, &fstate,
					format, &format, &b);
		if (fres == 0 && res == 1)
			continue;

		if (format != NULL)
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_DEBUG, 0, NULL, format);

		res = fres;
		break;
	}
	if (format == NULL) {
		debug_params(this, this->follower, this->direction, 0,
				SPA_PARAM_EnumFormat, format, "follower format", res);
		debug_params(this, this->target,
				SPA_DIRECTION_REVERSE(this->direction), 0,
				SPA_PARAM_EnumFormat, format, "convert format", res);
		res = -ENOTSUP;
		goto done;
	}
	def = spa_format_audio_build(&b,
			SPA_PARAM_Format, &this->default_format);

	format = merge_objects(this, &b, SPA_PARAM_Format,
			(struct spa_pod_object*)format,
			(struct spa_pod_object*)def);
	if (format == NULL)
		return -ENOSPC;

	spa_pod_fixate(format);

	res = configure_format(this, SPA_NODE_PARAM_FLAG_NEAREST, format);

done:
	spa_node_send_command(this->follower,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_ParamEnd));

	return res;
}


static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: command %d", this, SPA_NODE_COMMAND_ID(command));

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		spa_log_debug(this->log, "%p: starting %d", this, this->started);
		if ((res = negotiate_format(this)) < 0)
			return res;
		this->ready = true;
		this->warned = false;
		break;
	case SPA_NODE_COMMAND_Suspend:
		spa_log_debug(this->log, "%p: suspending", this);
		break;
	case SPA_NODE_COMMAND_Pause:
		spa_log_debug(this->log, "%p: pausing", this);
		break;
	case SPA_NODE_COMMAND_Flush:
		spa_log_debug(this->log, "%p: flushing", this);
		this->io_buffers.status = SPA_STATUS_OK;
		break;
	default:
		break;
	}

	res = spa_node_send_command(this->target, command);
	if (res == -ENOTSUP && this->target != this->follower)
		res = 0;
	if (res < 0) {
		spa_log_error(this->log, "%p: can't send command %d: %s",
				this, SPA_NODE_COMMAND_ID(command),
				spa_strerror(res));
	}

	if (res >= 0 && this->target != this->follower) {
		if ((res = spa_node_send_command(this->follower, command)) < 0) {
			spa_log_error(this->log, "%p: can't send command %d: %s",
					this, SPA_NODE_COMMAND_ID(command),
					spa_strerror(res));
		}
	}
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (res < 0) {
			spa_log_debug(this->log, "%p: start failed", this);
			this->ready = false;
			configure_format(this, 0, NULL);
		} else {
			this->started = true;
			spa_log_debug(this->log, "%p: started", this);
		}
		break;
	case SPA_NODE_COMMAND_Suspend:
		configure_format(this, 0, NULL);
		this->started = false;
		this->warned = false;
		this->ready = false;
		spa_log_debug(this->log, "%p: suspended", this);
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		this->warned = false;
		this->ready = false;
		spa_log_debug(this->log, "%p: paused", this);
		break;
	case SPA_NODE_COMMAND_Flush:
		spa_log_debug(this->log, "%p: flushed", this);
		break;
	}
	return res;
}

static void convert_node_info(void *data, const struct spa_node_info *info)
{
	struct impl *this = data;
	uint32_t i;

	spa_log_debug(this->log, "%p: info change:%08"PRIx64, this,
			info->change_mask);

	if (info->change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t idx;

			switch (info->params[i].id) {
			case SPA_PARAM_EnumPortConfig:
				idx = IDX_EnumPortConfig;
				break;
			case SPA_PARAM_PortConfig:
				idx = IDX_PortConfig;
				break;
			case SPA_PARAM_PropInfo:
				idx = IDX_PropInfo;
				break;
			case SPA_PARAM_Props:
				idx = IDX_Props;
				break;
			default:
				continue;
			}
			if (!this->add_listener &&
			    this->convert_params_flags[idx] == info->params[i].flags)
				continue;

			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			this->convert_params_flags[idx] = info->params[i].flags;
			this->params[idx].flags =
				(this->params[idx].flags & SPA_PARAM_INFO_SERIAL) |
				(info->params[i].flags & SPA_PARAM_INFO_READWRITE);

			if (this->add_listener)
				continue;

			this->params[idx].user++;
			spa_log_debug(this->log, "param %d changed", info->params[i].id);
		}
	}
	emit_node_info(this, false);
}

static void follower_convert_port_info(void *data,
		enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct impl *this = data;
	uint32_t i;
	int res;

	if (info == NULL)
		return;

	spa_log_debug(this->log, "%p: convert port info %s %p %08"PRIx64, this,
			this->direction == SPA_DIRECTION_INPUT ?
				"Input" : "Output", info, info->change_mask);

	this->convert_port_flags = info->flags;

	if (info->change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t idx;

			switch (info->params[i].id) {
			case SPA_PARAM_Latency:
				idx = IDX_Latency;
				break;
			case SPA_PARAM_Tag:
				idx = IDX_Tag;
				break;
			default:
				continue;
			}

			if (!this->add_listener &&
			    this->convert_params_flags[idx] == info->params[i].flags)
				continue;

			this->convert_params_flags[idx] = info->params[i].flags;

			if (this->add_listener)
				continue;

			if (idx == IDX_Latency) {
				this->in_recalc++;
				res = recalc_latency(this, this->target, direction, port_id, this->follower);
				this->in_recalc--;
				spa_log_debug(this->log, "latency: %d (%s)", res,
						spa_strerror(res));
			}
			if (idx == IDX_Tag) {
				this->in_recalc++;
				res = recalc_tag(this, this->target, direction, port_id, this->follower);
				this->in_recalc--;
				spa_log_debug(this->log, "tag: %d (%s)", res,
						spa_strerror(res));
			}
			spa_log_debug(this->log, "param %d changed", info->params[i].id);
		}
	}
}

static void convert_port_info(void *data,
		enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct impl *this = data;
	struct spa_port_info pi;

	if (direction != this->direction) {
		if (port_id == 0) {
			/* handle the converter output port into the follower separately */
			follower_convert_port_info(this, direction, port_id, info);
			return;
		} else
			/* the monitor ports are exposed */
			port_id--;
	} else if (info) {
		pi = *info;
		pi.flags |= this->follower_port_flags &
			(SPA_PORT_FLAG_LIVE |
			 SPA_PORT_FLAG_PHYSICAL |
			 SPA_PORT_FLAG_TERMINAL);
		info = &pi;
	}

	spa_log_debug(this->log, "%p: port info %d:%d", this,
			direction, port_id);

	if (this->target != this->follower)
		spa_node_emit_port_info(&this->hooks, direction, port_id, info);
}

static void convert_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *this = data;

	if (this->target == this->follower || this->in_enum_sync)
		return;

	spa_log_trace(this->log, "%p: result %d %d", this, seq, res);
	spa_node_emit_result(&this->hooks, seq, res, type, result);
}

static const struct spa_node_events convert_node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = convert_node_info,
	.port_info = convert_port_info,
	.result = convert_result,
};

static void follower_info(void *data, const struct spa_node_info *info)
{
	struct impl *this = data;
	uint32_t i;

	spa_log_debug(this->log, "%p: info change:%08"PRIx64" %d:%d", this,
			info->change_mask, info->max_input_ports, info->max_output_ports);

	if (this->follower_removing)
		return;

	this->async = (info->flags & SPA_NODE_FLAG_ASYNC) != 0;

	if (info->max_input_ports > 0)
		this->direction = SPA_DIRECTION_INPUT;
	else
		this->direction = SPA_DIRECTION_OUTPUT;

	if (this->direction == SPA_DIRECTION_INPUT) {
		this->info.flags |= SPA_NODE_FLAG_IN_PORT_CONFIG;
		this->info.max_input_ports = MAX_PORTS;
	} else {
		this->info.flags |= SPA_NODE_FLAG_OUT_PORT_CONFIG;
		this->info.max_output_ports = MAX_PORTS;
	}
	SPA_FLAG_UPDATE(this->info.flags, SPA_NODE_FLAG_ASYNC,
			this->async && this->follower == this->target);

	spa_log_debug(this->log, "%p: follower info %s", this,
			this->direction == SPA_DIRECTION_INPUT ?
				"Input" : "Output");

	if (info->change_mask & SPA_NODE_CHANGE_MASK_PROPS) {
		this->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
		this->info.props = info->props;
	}
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t idx;

			switch (info->params[i].id) {
			case SPA_PARAM_PropInfo:
				idx = IDX_PropInfo;
				break;
			case SPA_PARAM_Props:
				idx = IDX_Props;
				break;
			case SPA_PARAM_ProcessLatency:
				idx = IDX_ProcessLatency;
				break;
			default:
				continue;
			}
			if (!this->add_listener &&
			    this->follower_params_flags[idx] == info->params[i].flags)
				continue;

			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			this->follower_params_flags[idx] = info->params[i].flags;
			this->params[idx].flags =
				(this->params[idx].flags & SPA_PARAM_INFO_SERIAL) |
				(info->params[i].flags & SPA_PARAM_INFO_READWRITE);

			if (this->add_listener)
				continue;

			this->params[idx].user++;
			spa_log_debug(this->log, "param %d changed", info->params[i].id);
		}
	}
	emit_node_info(this, false);

	spa_zero(this->info.props);
	this->info.change_mask &= ~SPA_NODE_CHANGE_MASK_PROPS;
}

static void follower_port_info(void *data,
		enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct impl *this = data;
	uint32_t i;
	int res;

	if (info == NULL)
		return;

	if (this->follower_removing) {
	      spa_node_emit_port_info(&this->hooks, direction, port_id, NULL);
	      return;
	}

	this->follower_port_flags = info->flags;

	spa_log_debug(this->log, "%p: follower port info %s %p %08"PRIx64" recalc:%u", this,
			this->direction == SPA_DIRECTION_INPUT ?
				"Input" : "Output", info, info->change_mask,
				this->in_recalc);

	if (info->change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t idx;

			switch (info->params[i].id) {
			case SPA_PARAM_EnumFormat:
				idx = IDX_EnumFormat;
				break;
			case SPA_PARAM_Format:
				idx = IDX_Format;
				break;
			case SPA_PARAM_Latency:
				idx = IDX_Latency;
				break;
			case SPA_PARAM_Tag:
				idx = IDX_Tag;
				break;
			default:
				continue;
			}

			if (!this->add_listener &&
			    this->follower_params_flags[idx] == info->params[i].flags)
				continue;

			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			this->follower_params_flags[idx] = info->params[i].flags;
			this->params[idx].flags =
				(this->params[idx].flags & SPA_PARAM_INFO_SERIAL) |
				(info->params[i].flags & SPA_PARAM_INFO_READWRITE);

			if (this->add_listener)
				continue;

			if (idx == IDX_Latency && this->in_recalc == 0) {
				res = recalc_latency(this, this->follower, direction, port_id, this->target);
				spa_log_debug(this->log, "latency: %d (%s)", res,
						spa_strerror(res));
			}
			if (idx == IDX_Tag && this->in_recalc == 0) {
				res = recalc_tag(this, this->follower, direction, port_id, this->target);
				spa_log_debug(this->log, "tag: %d (%s)", res,
						spa_strerror(res));
			}
			if (idx == IDX_EnumFormat) {
				spa_log_debug(this->log, "new formats");
				/* we will renegotiate when restarting */
				this->recheck_format = true;
			}

			this->params[idx].user++;
			spa_log_debug(this->log, "param %d changed", info->params[i].id);
		}
	}
	emit_node_info(this, false);

	if (this->target == this->follower)
	      spa_node_emit_port_info(&this->hooks, direction, port_id, info);
}

static void follower_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *this = data;

	if (this->target != this->follower || this->in_enum_sync)
		return;

	spa_log_trace(this->log, "%p: result %d %d", this, seq, res);
	spa_node_emit_result(&this->hooks, seq, res, type, result);
}

static void follower_event(void *data, const struct spa_event *event)
{
	struct impl *this = data;

	spa_log_trace(this->log, "%p: event %d", this, SPA_EVENT_TYPE(event));

	switch (SPA_NODE_EVENT_ID(event)) {
	case SPA_NODE_EVENT_Error:
	case SPA_NODE_EVENT_RequestProcess:
		/* Forward errors and process requests */
		spa_node_emit_event(&this->hooks, event);
		break;
	default:
		/* Ignore other events */
		break;
	}
}

static const struct spa_node_events follower_node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = follower_info,
	.port_info = follower_port_info,
	.result = follower_result,
	.event = follower_event,
};

static void follower_probe_info(void *data, const struct spa_node_info *info)
{
	struct impl *this = data;
	if (info->max_input_ports > 0)
		this->direction = SPA_DIRECTION_INPUT;
        else
		this->direction = SPA_DIRECTION_OUTPUT;
}

static const struct spa_node_events follower_probe_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = follower_probe_info,
};

static int follower_ready(void *data, int status)
{
	struct impl *this = data;

	spa_log_trace_fp(this->log, "%p: ready %d", this, status);

	if (!this->ready) {
		spa_log_info(this->log, "%p: ready stopped node", this);
		return -EIO;
	}

	if (this->target != this->follower) {
		this->driver = true;

		if (this->direction == SPA_DIRECTION_OUTPUT) {
			int retry = MAX_RETRY;
			while (retry--) {
				status = spa_node_process_fast(this->target);
				if (status & SPA_STATUS_HAVE_DATA)
					break;

				if (status & SPA_STATUS_NEED_DATA) {
					status = spa_node_process_fast(this->follower);
					if (!(status & SPA_STATUS_HAVE_DATA))
						break;
				}
			}

		}
	}

	return spa_node_call_ready(&this->callbacks, status);
}

static int follower_reuse_buffer(void *data, uint32_t port_id, uint32_t buffer_id)
{
	int res;
	struct impl *this = data;

	if (this->target != this->follower)
		res = spa_node_port_reuse_buffer(this->target, port_id, buffer_id);
	else
		res = spa_node_call_reuse_buffer(&this->callbacks, port_id, buffer_id);

	return res;
}

static int follower_xrun(void *data, uint64_t trigger, uint64_t delay, struct spa_pod *info)
{
	struct impl *this = data;
	return spa_node_call_xrun(&this->callbacks, trigger, delay, info);
}

static const struct spa_node_callbacks follower_node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = follower_ready,
	.reuse_buffer = follower_reuse_buffer,
	.xrun = follower_xrun,
};

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook l;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, "%p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);


	if (events->info || events->port_info) {
		this->add_listener = true;

		spa_zero(l);
		spa_node_add_listener(this->follower, &l, &follower_node_events, this);
		spa_hook_remove(&l);

		if (this->follower != this->target) {
			spa_zero(l);
			spa_node_add_listener(this->target, &l, &convert_node_events, this);
			spa_hook_remove(&l);
		}
		this->add_listener = false;

		emit_node_info(this, true);
	}
	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
impl_node_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	return spa_node_sync(this->follower, seq);
}

static int
impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (direction != this->direction)
		return -EINVAL;

	return spa_node_add_port(this->target, direction, port_id, props);
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (direction != this->direction)
		return -EINVAL;

	return spa_node_remove_port(this->target, direction, port_id);
}

static int
port_enum_formats_for_convert(struct impl *this, int seq, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	int res;
	uint32_t count = 0;
	struct spa_result_node_params result;

	result.id = id;
	result.next = start;
next:
	result.index = result.next;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (result.next < 0x100000) {
		/* Enumerate follower formats first, until we have enough or we run out */
		if ((res = node_port_enum_params_sync(this, this->follower, direction, port_id, id,
						&result.next, filter, &result.param, &b)) != 1) {
			if (res == 0 || res == -ENOENT) {
				result.next = 0x100000;
				goto next;
			} else {
				spa_log_error(this->log, "could not enum follower format: %s", spa_strerror(res));
				return res;
			}
		}
	} else if (result.next < 0x200000) {
		/* Then enumerate converter formats */
		result.next &= 0xfffff;
		if ((res = node_port_enum_params_sync(this, this->convert, direction, port_id, id,
						&result.next, filter, &result.param, &b)) != 1) {
			return res;
		} else {
			result.next |= 0x100000;
		}
	}

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count < num)
		goto next;

	return 0;
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	if (direction != this->direction)
		port_id++;

	spa_log_debug(this->log, "%p: %d %u %u %u", this, seq, id, start, num);

	/* We only need special handling for EnumFormat in convert mode */
	if (id == SPA_PARAM_EnumFormat && this->mode == SPA_PARAM_PORT_CONFIG_MODE_convert)
		return port_enum_formats_for_convert(this, seq, direction, port_id, id,
				start, num, filter);
	else
		return spa_node_port_enum_params(this->target, seq, direction, port_id, id,
				start, num, filter);
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, " %d %d %d %d", port_id, id, direction, this->direction);

	if (direction != this->direction)
		port_id++;

	return spa_node_port_set_param(this->target, direction, port_id, id,
			flags, param);
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "set io %d %d %d %d", port_id, id, direction, this->direction);

	if (direction != this->direction)
		port_id++;

	return spa_node_port_set_io(this->target, direction, port_id, id, data, size);
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (direction != this->direction)
		port_id++;

	spa_log_debug(this->log, "%p: %d %d:%d", this,
			n_buffers, direction, port_id);

	if ((res = spa_node_port_use_buffers(this->target,
					direction, port_id, flags, buffers, n_buffers)) < 0)
		return res;

	return res;
}

static int
impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	return spa_node_port_reuse_buffer(this->target, port_id, buffer_id);
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	int status = 0, fstatus, retry = MAX_RETRY;

	if (!this->ready) {
		if (!this->warned)
			spa_log_warn(this->log, "%p: scheduling stopped node", this);
		this->warned = true;
		return -EIO;
	}

	spa_log_trace_fp(this->log, "%p: process convert:%p driver:%d",
			this, this->convert, this->driver);

	if (this->target == this->follower) {
		if (this->io_position)
			this->io_rate_match.size = this->io_position->clock.duration;
		return spa_node_process_fast(this->follower);
	}

	if (this->direction == SPA_DIRECTION_INPUT) {
		/* an input node (sink).
		 * First we run the converter to process the input for the follower
		 * then if it produced data, we run the follower. */
		while (retry--) {
			status = spa_node_process_fast(this->target);
			/* schedule the follower when the converter needed
			 * a recycled buffer */
			if (status == -EPIPE || status == 0)
				status = SPA_STATUS_HAVE_DATA;
			else if (status < 0)
				break;

			if (status & (SPA_STATUS_HAVE_DATA | SPA_STATUS_DRAINED)) {
				/* as long as the converter produced something or
				 * is drained, process the follower. */
				fstatus = spa_node_process_fast(this->follower);
				if (fstatus < 0) {
					status = fstatus;
					break;
				}
				/* if the follower doesn't need more data or is
				 * drained we can stop */
				if ((fstatus & SPA_STATUS_NEED_DATA) == 0 ||
				    (fstatus & SPA_STATUS_DRAINED))
					break;
			}
			/* the converter needs more data */
			if ((status & SPA_STATUS_NEED_DATA))
				break;
		}
	} else if (!this->driver) {
		bool done = false;
		while (retry--) {
			/* output node (source). First run the converter to make
			 * sure we push out any queued data. Then when it needs
			 * more data, schedule the follower. */
			status = spa_node_process_fast(this->target);
			if (status == 0)
				status = SPA_STATUS_NEED_DATA;
			else if (status < 0)
				break;

			done = (status & (SPA_STATUS_HAVE_DATA | SPA_STATUS_DRAINED));
			if (done)
				break;

			if (status & SPA_STATUS_NEED_DATA) {
				/* the converter needs more data, schedule the
				 * follower */
				fstatus = spa_node_process_fast(this->follower);
				if (fstatus < 0) {
					status = fstatus;
					break;
				}
				/* if the follower didn't produce more data or is
				 * not drained we can stop now */
				if ((fstatus & (SPA_STATUS_HAVE_DATA | SPA_STATUS_DRAINED)) == 0)
					break;
			}
		}
		if (!done)
			spa_node_call_xrun(&this->callbacks, 0, 0, NULL);

	} else {
		status = spa_node_process_fast(this->follower);
	}
	spa_log_trace_fp(this->log, "%p: process status:%d", this, status);

	this->driver = false;

	return status;
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

static int load_converter(struct impl *this, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	const char* factory_name = NULL;
	struct spa_handle *hnd_convert = NULL;
	void *iface_conv = NULL;
	bool unload_handle = false;
	struct spa_dict_item *items;
	struct spa_dict cinfo;
	char direction[16];
	uint32_t i;

	items = alloca((info->n_items + 1) * sizeof(struct spa_dict_item));
	cinfo = SPA_DICT(items, 0);
	for (i = 0; i < info->n_items; i++)
		items[cinfo.n_items++] = info->items[i];

	snprintf(direction, sizeof(direction), "%s",
			SPA_DIRECTION_REVERSE(this->direction) == SPA_DIRECTION_INPUT ?
			"input" : "output");
	items[cinfo.n_items++] = SPA_DICT_ITEM("convert.direction", direction);

	factory_name = spa_dict_lookup(&cinfo, "audio.adapt.converter");
	if (factory_name == NULL)
		factory_name = SPA_NAME_AUDIO_CONVERT;

	if (spa_streq(factory_name, SPA_NAME_AUDIO_CONVERT)) {
		size_t size = spa_handle_factory_get_size(&spa_audioconvert_factory, &cinfo);

		hnd_convert = calloc(1, size);
		if (hnd_convert == NULL)
			return -errno;

		spa_handle_factory_init(&spa_audioconvert_factory,
				hnd_convert, &cinfo, support, n_support);
	} else if (this->ploader) {
		hnd_convert = spa_plugin_loader_load(this->ploader, factory_name, &cinfo);
		if (!hnd_convert)
			return -EINVAL;
		unload_handle = true;
	} else {
		return -ENOTSUP;
	}

	spa_handle_get_interface(hnd_convert, SPA_TYPE_INTERFACE_Node, &iface_conv);
	if (iface_conv == NULL) {
		if (unload_handle)
			spa_plugin_loader_unload(this->ploader, hnd_convert);
		else {
			spa_handle_clear(hnd_convert);
			free(hnd_convert);
		}
		return -EINVAL;
	}

	this->hnd_convert = hnd_convert;
	this->convert = iface_conv;
	this->unload_handle = unload_handle;
	this->convertname = strdup(factory_name);

	return 0;
}


static int do_auto_port_config(struct impl *this, const char *str)
{
	uint32_t state = 0, i;
	uint8_t buffer[4096];
	struct spa_pod_builder b;
#define POSITION_PRESERVE 0
#define POSITION_AUX 1
#define POSITION_UNKNOWN 2
	int l, res, position = POSITION_PRESERVE;
	struct spa_pod *param;
	bool have_format = false, monitor = false, control = false;
	struct spa_audio_info format = { 0, };
	enum spa_param_port_config_mode mode = SPA_PARAM_PORT_CONFIG_MODE_none;
	struct spa_json it[1];
	char key[1024], val[256];
	const char *v;

	if (spa_json_begin_object(&it[0], str, strlen(str)) <= 0)
		return -EINVAL;

	while ((l = spa_json_object_next(&it[0], key, sizeof(key), &v)) > 0) {
		if (spa_json_parse_stringn(v, l, val, sizeof(val)) <= 0)
			continue;

		if (spa_streq(key, "mode")) {
			mode = spa_debug_type_find_type_short(spa_type_param_port_config_mode, val);
			if (mode == SPA_ID_INVALID)
				mode = SPA_PARAM_PORT_CONFIG_MODE_none;
		} else if (spa_streq(key, "monitor")) {
			monitor = spa_atob(val);
		} else if (spa_streq(key, "control")) {
			control = spa_atob(val);
		} else if (spa_streq(key, "position")) {
			if (spa_streq(val, "unknown"))
				position = POSITION_UNKNOWN;
			else if (spa_streq(val, "aux"))
				position = POSITION_AUX;
			else
				position = POSITION_PRESERVE;
		}
        }

	while (true) {
		struct spa_audio_info info = { 0, };

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if ((res = node_port_enum_params_sync(this, this->follower,
					this->direction, 0,
					SPA_PARAM_EnumFormat, &state,
					NULL, &param, &b)) != 1)
			break;

		if ((res = spa_format_audio_parse(param, &info)) < 0)
			continue;

		spa_pod_object_fixate((struct spa_pod_object*)param);

		if (info.media_subtype == SPA_MEDIA_SUBTYPE_raw &&
		    format.media_subtype == SPA_MEDIA_SUBTYPE_raw &&
		    format.info.raw.channels >= info.info.raw.channels)
			continue;

		format = info;
		have_format = true;
	}
	if (!have_format)
		return -ENOENT;

	if (format.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		uint32_t n_pos = SPA_MIN(SPA_N_ELEMENTS(format.info.raw.position), format.info.raw.channels);
		if (position == POSITION_AUX) {
			for (i = 0; i < n_pos; i++)
				format.info.raw.position[i] = SPA_AUDIO_CHANNEL_START_Aux + i;
		} else if (position == POSITION_UNKNOWN) {
			for (i = 0; i < n_pos; i++)
				format.info.raw.position[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
		}
	}

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_format_audio_build(&b, SPA_PARAM_Format, &format);
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(this->direction),
		SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(mode),
		SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(monitor),
		SPA_PARAM_PORT_CONFIG_control,   SPA_POD_Bool(control),
		SPA_PARAM_PORT_CONFIG_format,    SPA_POD_Pod(param));

	return impl_node_set_param(this, SPA_PARAM_PortConfig, 0, param);
}

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	spa_hook_remove(&this->follower_listener);
	spa_node_set_callbacks(this->follower, NULL, NULL);

	if (this->hnd_convert) {
		if (this->unload_handle)
			spa_plugin_loader_unload(this->ploader, this->hnd_convert);
		else {
			spa_handle_clear(this->hnd_convert);
			free(this->hnd_convert);
		}
		free(this->convertname);
	}

	clear_buffers(this);
	return 0;
}


static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	size_t size = sizeof(struct impl);

	return size;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	const char *str;
	int ret;
	struct spa_hook probe_listener;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(this->log, &log_topic);

	/* FIXME, we should check the IO params for SPA_IO_RateMatch */
	this->have_rate_match = true;

	this->cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);

	this->ploader = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_PluginLoader);

	if (info == NULL ||
	    (str = spa_dict_lookup(info, "audio.adapt.follower")) == NULL)
		return -EINVAL;

	sscanf(str, "pointer:%p", &this->follower);
	if (this->follower == NULL)
		return -EINVAL;

	if (this->cpu)
		this->max_align = spa_cpu_get_max_align(this->cpu);

	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	/* just probe the ports to get the direction */
	spa_zero(probe_listener);
	spa_node_add_listener(this->follower, &probe_listener, &follower_probe_events, this);
	spa_hook_remove(&probe_listener);

	ret = load_converter(this, info, support, n_support);
	spa_log_info(this->log, "%p: loaded converter %s, hnd %p, convert %p", this,
			this->convertname, this->hnd_convert, this->convert);
	if (ret < 0)
		return ret;

	if (this->convert == NULL) {
		this->target = this->follower;
		this->mode = SPA_PARAM_PORT_CONFIG_MODE_passthrough;
	} else {
		this->target = this->convert;
		/* the actual mode is selected below */
		this->mode = SPA_PARAM_PORT_CONFIG_MODE_none;
		configure_convert(this, this->mode);
	}

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
		SPA_NODE_CHANGE_MASK_PROPS |
		SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.flags = SPA_NODE_FLAG_RT |
		SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	this->params[IDX_EnumPortConfig] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->params[IDX_PortConfig] = SPA_PARAM_INFO(SPA_PARAM_PortConfig, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_ProcessLatency] = SPA_PARAM_INFO(SPA_PARAM_ProcessLatency, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_Tag] = SPA_PARAM_INFO(SPA_PARAM_Tag, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	spa_node_add_listener(this->follower,
			&this->follower_listener, &follower_node_events, this);
	spa_node_set_callbacks(this->follower, &follower_node_callbacks, this);

	if (this->convert) {
		spa_node_add_listener(this->convert,
				&this->convert_listener, &convert_node_events, this);
		if (info && (str = spa_dict_lookup(info, "adapter.auto-port-config")) != NULL)
			do_auto_port_config(this, str);
		else {
			reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_none, this->direction, NULL);
		}
	} else {
		reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_passthrough, this->direction, NULL);
	}
	link_io(this);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
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

const struct spa_handle_factory spa_audioadapter_factory = {
	.version = SPA_VERSION_HANDLE_FACTORY,
	.name = SPA_NAME_AUDIO_ADAPT,
	.get_size = impl_get_size,
	.init = impl_init,
	.enum_interface_info = impl_enum_interface_info,
};
