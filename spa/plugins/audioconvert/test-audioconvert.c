/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/support/plugin.h>
#include <spa/param/param.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/debug/mem.h>
#include <spa/debug/log.h>
#include <spa/support/log-impl.h>

SPA_LOG_IMPL(logger);

extern const struct spa_handle_factory test_source_factory;

#define MAX_PORTS (SPA_AUDIO_MAX_CHANNELS+1)

struct context {
	struct spa_handle *convert_handle;
	struct spa_node *convert_node;

	bool got_node_info;
	uint32_t n_port_info[2];
	bool got_port_info[2][MAX_PORTS];
};

static const struct spa_handle_factory *find_factory(const char *name)
{
	uint32_t index = 0;
	const struct spa_handle_factory *factory;

	while (spa_handle_factory_enum(&factory, &index) == 1) {
		if (spa_streq(factory->name, name))
			return factory;
	}
	return NULL;
}

static int setup_context(struct context *ctx)
{
	size_t size;
	int res;
	struct spa_support support[1];
	struct spa_dict_item items[6];
	const struct spa_handle_factory *factory;
	void *iface;

	logger.log.level = SPA_LOG_LEVEL_TRACE;
	support[0] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, &logger);

	/* make convert */
	factory = find_factory(SPA_NAME_AUDIO_CONVERT);
	spa_assert_se(factory != NULL);

	size = spa_handle_factory_get_size(factory, NULL);

	ctx->convert_handle = calloc(1, size);
	spa_assert_se(ctx->convert_handle != NULL);

	items[0] = SPA_DICT_ITEM_INIT("clock.quantum-limit", "8192");
	items[1] = SPA_DICT_ITEM_INIT("channelmix.upmix", "true");
	items[2] = SPA_DICT_ITEM_INIT("channelmix.upmix-method", "psd");
	items[3] = SPA_DICT_ITEM_INIT("channelmix.lfe-cutoff", "150");
	items[4] = SPA_DICT_ITEM_INIT("channelmix.fc-cutoff", "12000");
	items[5] = SPA_DICT_ITEM_INIT("channelmix.rear-delay", "12.0");

	res = spa_handle_factory_init(factory,
			ctx->convert_handle,
			&SPA_DICT_INIT(items, 6),
			support, 1);
	spa_assert_se(res >= 0);

	res = spa_handle_get_interface(ctx->convert_handle,
			SPA_TYPE_INTERFACE_Node, &iface);
	spa_assert_se(res >= 0);
	ctx->convert_node = iface;

	return 0;
}

static int clean_context(struct context *ctx)
{
	spa_handle_clear(ctx->convert_handle);
	free(ctx->convert_handle);
	return 0;
}

static void node_info_check(void *data, const struct spa_node_info *info)
{
	struct context *ctx = data;

	fprintf(stderr, "input %d, output %d\n",
			info->max_input_ports,
			info->max_output_ports);

	spa_assert_se(info->max_input_ports == MAX_PORTS);
	spa_assert_se(info->max_output_ports == MAX_PORTS);

	ctx->got_node_info = true;
}

static void port_info_check(void *data,
		enum spa_direction direction, uint32_t port,
		const struct spa_port_info *info)
{
	struct context *ctx = data;

	fprintf(stderr, "port %d %d %p\n", direction, port, info);

	ctx->got_port_info[direction][port] = true;
	ctx->n_port_info[direction]++;
}

static int test_init_state(struct context *ctx)
{
	struct spa_hook listener;
	static const struct spa_node_events init_events = {
		SPA_VERSION_NODE_EVENTS,
		.info = node_info_check,
		.port_info = port_info_check,
	};

	spa_zero(ctx->got_node_info);
	spa_zero(ctx->n_port_info);
	spa_zero(ctx->got_port_info);

	spa_zero(listener);
	spa_node_add_listener(ctx->convert_node,
			&listener, &init_events, ctx);
	spa_hook_remove(&listener);

	spa_assert_se(ctx->got_node_info);
	spa_assert_se(ctx->n_port_info[0] == 1);
	spa_assert_se(ctx->n_port_info[1] == 1);
	spa_assert_se(ctx->got_port_info[0][0] == true);
	spa_assert_se(ctx->got_port_info[1][0] == true);

	return 0;
}

static int test_set_in_format(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;

	/* other format */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	info = (struct spa_audio_info_raw) {
		.format = SPA_AUDIO_FORMAT_S16,
		.rate = 44100,
		.channels = 2,
		.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, }
	};
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	res = spa_node_port_set_param(ctx->convert_node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, param);
	spa_assert_se(res == 0);

	return 0;
}

static int test_split_setup1(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;
	struct spa_hook listener;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.port_info = port_info_check,
	};

	spa_zero(listener);
	spa_node_add_listener(ctx->convert_node,
			&listener, &node_events, ctx);

	/* port config, output as DSP */
	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_F32P;
	info.rate = 48000;
	info.channels = 6;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	info.position[2] = SPA_AUDIO_CHANNEL_FC;
	info.position[3] = SPA_AUDIO_CHANNEL_LFE;
	info.position[4] = SPA_AUDIO_CHANNEL_SL;
	info.position[5] = SPA_AUDIO_CHANNEL_SR;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(SPA_DIRECTION_OUTPUT),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
		SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(param));

	res = spa_node_set_param(ctx->convert_node, SPA_PARAM_PortConfig, 0, param);
	spa_assert_se(res == 0);

	spa_hook_remove(&listener);

	return 0;
}

static int test_split_setup2(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;
	struct spa_hook listener;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.port_info = port_info_check,
	};

	spa_zero(listener);
	spa_node_add_listener(ctx->convert_node,
			&listener, &node_events, ctx);

	/* port config, output as DSP */
	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_F32P;
	info.rate = 48000;
	info.channels = 4;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	info.position[2] = SPA_AUDIO_CHANNEL_RL;
	info.position[3] = SPA_AUDIO_CHANNEL_RR;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(SPA_DIRECTION_OUTPUT),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
		SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(param));

	res = spa_node_set_param(ctx->convert_node, SPA_PARAM_PortConfig, 0, param);
	spa_assert_se(res == 0);

	spa_hook_remove(&listener);

	return 0;
}

static int test_convert_setup1(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	int res;
	struct spa_hook listener;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.port_info = port_info_check,
	};

	spa_zero(listener);
	spa_node_add_listener(ctx->convert_node,
			&listener, &node_events, ctx);

	/* port config, output convert */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(SPA_DIRECTION_OUTPUT),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_convert));

	res = spa_node_set_param(ctx->convert_node, SPA_PARAM_PortConfig, 0, param);
	spa_assert_se(res == 0);

	spa_hook_remove(&listener);

	return 0;
}

static int test_set_out_format(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;

	/* out format */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	info = (struct spa_audio_info_raw) {
		.format = SPA_AUDIO_FORMAT_S32P,
		.rate = 96000,
		.channels = 8,
		.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR,
			SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, }
	};
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	res = spa_node_port_set_param(ctx->convert_node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, param);
	spa_assert_se(res == 0);

	return 0;
}

static int test_merge_setup1(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;
	struct spa_hook listener;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.port_info = port_info_check,
	};

	spa_zero(listener);
	spa_node_add_listener(ctx->convert_node,
			&listener, &node_events, ctx);

	/* port config, output as DSP */
	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_F32P;
	info.rate = 48000;
	info.channels = 6;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	info.position[2] = SPA_AUDIO_CHANNEL_FC;
	info.position[3] = SPA_AUDIO_CHANNEL_LFE;
	info.position[4] = SPA_AUDIO_CHANNEL_RL;
	info.position[5] = SPA_AUDIO_CHANNEL_RR;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(SPA_DIRECTION_INPUT),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
		SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(param));

	res = spa_node_set_param(ctx->convert_node, SPA_PARAM_PortConfig, 0, param);
	spa_assert_se(res == 0);

	spa_hook_remove(&listener);

	return 0;
}

static int test_set_out_format2(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;

	/* out format */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	info = (struct spa_audio_info_raw) {
		.format = SPA_AUDIO_FORMAT_S16,
		.rate = 32000,
		.channels = 2,
		.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, }
	};
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	res = spa_node_port_set_param(ctx->convert_node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, param);
	spa_assert_se(res == 0);

	return 0;
}

static int test_merge_setup2(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;
	struct spa_hook listener;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.port_info = port_info_check,
	};

	spa_zero(listener);
	spa_node_add_listener(ctx->convert_node,
			&listener, &node_events, ctx);

	/* port config, output as DSP */
	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_F32P;
	info.rate = 96000;
	info.channels = 4;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	info.position[2] = SPA_AUDIO_CHANNEL_FC;
	info.position[3] = SPA_AUDIO_CHANNEL_LFE;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(SPA_DIRECTION_INPUT),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
		SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(param));

	res = spa_node_set_param(ctx->convert_node, SPA_PARAM_PortConfig, 0, param);
	spa_assert_se(res == 0);

	spa_hook_remove(&listener);

	return 0;
}

static int test_convert_setup2(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	int res;
	struct spa_hook listener;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.port_info = port_info_check,
	};

	spa_zero(listener);
	spa_node_add_listener(ctx->convert_node,
			&listener, &node_events, ctx);

	/* port config, input convert */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(SPA_DIRECTION_INPUT),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_convert));

	res = spa_node_set_param(ctx->convert_node, SPA_PARAM_PortConfig, 0, param);
	spa_assert_se(res == 0);

	spa_hook_remove(&listener);

	return 0;
}

static int test_set_in_format2(struct context *ctx)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_audio_info_raw info;
	int res;

	/* other format */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	info = (struct spa_audio_info_raw) {
		.format = SPA_AUDIO_FORMAT_S24,
		.rate = 48000,
		.channels = 3,
		.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_LFE, }
	};
        param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	res = spa_node_port_set_param(ctx->convert_node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, param);
	spa_assert_se(res == 0);

	return 0;
}

static int setup_direction(struct context *ctx, enum spa_direction direction, uint32_t mode,
		struct spa_audio_info_raw *info)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param, *format;
	int res;
	uint32_t i;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_format_audio_raw_build(&b, SPA_PARAM_Format, info);

	switch (mode) {
	case SPA_PARAM_PORT_CONFIG_MODE_dsp:
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
			SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(direction),
			SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(mode),
			SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(format));
		break;

	case SPA_PARAM_PORT_CONFIG_MODE_convert:
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
			SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(direction),
			SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(mode));
		break;
	default:
		return -EINVAL;
	}
	res = spa_node_set_param(ctx->convert_node, SPA_PARAM_PortConfig, 0, param);
	spa_assert_se(res == 0);

	switch (mode) {
	case SPA_PARAM_PORT_CONFIG_MODE_convert:
		res = spa_node_port_set_param(ctx->convert_node, direction, 0,
			SPA_PARAM_Format, 0, format);
		spa_assert_se(res == 0);
		break;
	case SPA_PARAM_PORT_CONFIG_MODE_dsp:
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		format = spa_format_audio_dsp_build(&b, SPA_PARAM_Format,
	                &SPA_AUDIO_INFO_DSP_INIT(
				.format = SPA_AUDIO_FORMAT_F32P));
		for (i = 0; i < info->channels; i++) {
			res = spa_node_port_set_param(ctx->convert_node, direction, i,
				SPA_PARAM_Format, 0, format);
			spa_assert_se(res == 0);
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

struct buffer {
	struct spa_buffer buffer;
        struct spa_data datas[MAX_PORTS];
        struct spa_chunk chunks[MAX_PORTS];
};

struct data {
	uint32_t mode;
	struct spa_audio_info_raw info;
	uint32_t ports;
	uint32_t planes;
	const void *data[MAX_PORTS];
	uint32_t size;
};

static int run_convert(struct context *ctx, struct data *in_data,
		struct data *out_data)
{
	struct spa_command cmd;
	int res;
	uint32_t i, j, k;
	struct buffer in_buffers[in_data->ports];
	struct buffer out_buffers[out_data->ports];
	struct spa_io_buffers in_io[in_data->ports];
	struct spa_io_buffers out_io[out_data->ports];

	setup_direction(ctx, SPA_DIRECTION_INPUT, in_data->mode, &in_data->info);
	setup_direction(ctx, SPA_DIRECTION_OUTPUT, out_data->mode, &out_data->info);

	cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	res = spa_node_send_command(ctx->convert_node, &cmd);
	spa_assert_se(res == 0);

	for (i = 0, k = 0; i < in_data->ports; i++) {
		struct buffer *b = &in_buffers[i];
		struct spa_buffer *buffers[1];
		spa_zero(*b);
		b->buffer.datas = b->datas;
                b->buffer.n_datas = in_data->planes;

		for (j = 0; j < in_data->planes; j++, k++) {
			b->datas[j].type = SPA_DATA_MemPtr;
			b->datas[j].flags = 0;
			b->datas[j].fd = -1;
			b->datas[j].mapoffset = 0;
			b->datas[j].maxsize = in_data->size;
			b->datas[j].data = (void *)in_data->data[k];
			b->datas[j].chunk = &b->chunks[j];
			b->datas[j].chunk->offset = 0;
			b->datas[j].chunk->size = in_data->size;
			b->datas[j].chunk->stride = 0;
		}
		buffers[0] = &b->buffer;
		res = spa_node_port_use_buffers(ctx->convert_node, SPA_DIRECTION_INPUT, i,
				0, buffers, 1);
		spa_assert_se(res == 0);

		in_io[i].status = SPA_STATUS_HAVE_DATA;
		in_io[i].buffer_id = 0;

		res = spa_node_port_set_io(ctx->convert_node, SPA_DIRECTION_INPUT, i,
				SPA_IO_Buffers, &in_io[i], sizeof(in_io[i]));
		spa_assert_se(res == 0);
	}
	for (i = 0; i < out_data->ports; i++) {
		struct buffer *b = &out_buffers[i];
		struct spa_buffer *buffers[1];
		spa_zero(*b);
		b->buffer.datas = b->datas;
                b->buffer.n_datas = out_data->planes;

		for (j = 0; j < out_data->planes; j++) {
			b->datas[j].type = SPA_DATA_MemPtr;
			b->datas[j].flags = 0;
			b->datas[j].fd = -1;
			b->datas[j].mapoffset = 0;
			b->datas[j].maxsize = out_data->size;
			b->datas[j].data = calloc(1, out_data->size);
			b->datas[j].chunk = &b->chunks[j];
			b->datas[j].chunk->offset = 0;
			b->datas[j].chunk->size = 0;
			b->datas[j].chunk->stride = 0;
		}
		buffers[0] = &b->buffer;
		res = spa_node_port_use_buffers(ctx->convert_node,
				SPA_DIRECTION_OUTPUT, i, 0, buffers, 1);
		spa_assert_se(res == 0);

		out_io[i].status = SPA_STATUS_NEED_DATA;
		out_io[i].buffer_id = -1;

		res = spa_node_port_set_io(ctx->convert_node, SPA_DIRECTION_OUTPUT, i,
				SPA_IO_Buffers, &out_io[i], sizeof(out_io[i]));
		spa_assert_se(res == 0);
	}

	res = spa_node_process(ctx->convert_node);
	spa_assert_se(res == (SPA_STATUS_NEED_DATA | SPA_STATUS_HAVE_DATA));

	for (i = 0, k = 0; i < out_data->ports; i++) {
		struct buffer *b = &out_buffers[i];

		spa_assert_se(out_io[i].status == SPA_STATUS_HAVE_DATA);
		spa_assert_se(out_io[i].buffer_id == 0);

		for (j = 0; j < out_data->planes; j++, k++) {
			spa_assert_se(b->datas[j].chunk->offset == 0);
			spa_assert_se(b->datas[j].chunk->size == out_data->size);

			res = memcmp(b->datas[j].data, out_data->data[k], out_data->size);
			if (res != 0) {
				fprintf(stderr, "error port %d plane %d\n", i, j);
				spa_debug_log_mem(&logger.log, SPA_LOG_LEVEL_WARN,
						0, b->datas[j].data, out_data->size);
				spa_debug_log_mem(&logger.log, SPA_LOG_LEVEL_WARN,
						2, out_data->data[k], out_data->size);
			}
			spa_assert_se(res == 0);

			free(b->datas[j].data);
		}
	}
	cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend);
	res = spa_node_send_command(ctx->convert_node, &cmd);
	spa_assert_se(res == 0);

	return 0;
}

static const float data_f32p_1[] = { 0.1f, 0.1f, 0.1f, 0.1f };
static const float data_f32p_2[] = { 0.2f, 0.2f, 0.2f, 0.2f };
static const float data_f32p_3[] = { 0.3f, 0.3f, 0.3f, 0.3f };
static const float data_f32p_4[] = { 0.4f, 0.4f, 0.4f, 0.4f };
static const float data_f32p_5[] = { 0.5f, 0.5f, 0.5f, 0.5f };
static const float data_f32p_5_6p1[] = { 0.953553438f, 0.953553438f, 0.953553438f, 0.953553438f };
static const float data_f32p_6[] = { 0.6f, 0.6f, 0.6f, 0.6f };
static const float data_f32p_6_6p1[] = { 1.053553343f, 1.053553343f, 1.053553343f, 1.053553343f };
static const float data_f32p_7[] = { 0.7f, 0.7f, 0.7f, 0.7f };
static const float data_f32p_8[] = { 0.8f, 0.8f, 0.8f, 0.8f };

static const float data_f32_5p1[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };
static const float data_f32_6p1[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f };
static const float data_f32_6p1_from_5p1[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.55f, 0.5f, 0.6f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.55f, 0.5f, 0.6f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.55f, 0.5f, 0.6f,
				      0.1f, 0.2f, 0.3f, 0.4f, 0.55f, 0.5f, 0.6f };

static const float data_f32_7p1_remapped[] = { 0.1f, 0.2f, 0.5f, 0.6f, 0.7f, 0.8f, 0.3f, 0.4f,
				      0.1f, 0.2f, 0.5f, 0.6f, 0.7f, 0.8f, 0.3f, 0.4f,
				      0.1f, 0.2f, 0.5f, 0.6f, 0.7f, 0.8f, 0.3f, 0.4f,
				      0.1f, 0.2f, 0.5f, 0.6f, 0.7f, 0.8f, 0.3f, 0.4f };
static const float data_f32_5p1_remapped[] = { 0.1f, 0.2f, 0.5f, 0.6f, 0.3f, 0.4f,
				      0.1f, 0.2f, 0.5f, 0.6f, 0.3f, 0.4f,
				      0.1f, 0.2f, 0.5f, 0.6f, 0.3f, 0.4f,
				      0.1f, 0.2f, 0.5f, 0.6f, 0.3f, 0.4f };

struct data dsp_5p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 6,
	.planes = 1,
	.data = { data_f32p_1, data_f32p_2, data_f32p_3, data_f32p_4, data_f32p_5, data_f32p_6, },
	.size = sizeof(float) * 4
};

struct data dsp_5p1_from_6p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 6,
	.planes = 1,
	.data = { data_f32p_1, data_f32p_2, data_f32p_3, data_f32p_4, data_f32p_5_6p1, data_f32p_6_6p1, },
	.size = sizeof(float) * 4
};


struct data dsp_5p1_remapped = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
		}),
	.ports = 6,
	.planes = 1,
	.data = { data_f32p_1, data_f32p_2, data_f32p_5, data_f32p_6, data_f32p_3, data_f32p_4, },
	.size = sizeof(float) * 4
};

struct data dsp_5p1_remapped_from_6p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
		}),
	.ports = 6,
	.planes = 1,
	.data = { data_f32p_1, data_f32p_2, data_f32p_5_6p1, data_f32p_6_6p1, data_f32p_3, data_f32p_4, },
	.size = sizeof(float) * 4
};

struct data dsp_6p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 7,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RC,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 7,
	.planes = 1,
	.data = { data_f32p_1, data_f32p_2, data_f32p_3, data_f32p_4, data_f32p_5, data_f32p_6, data_f32p_7 },
	.size = sizeof(float) * 4
};

struct data dsp_6p1_side = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 7,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RC,
			SPA_AUDIO_CHANNEL_SL,
			SPA_AUDIO_CHANNEL_SR,
		}),
	.ports = 7,
	.planes = 1,
	.data = { data_f32p_1, data_f32p_2, data_f32p_3, data_f32p_4, data_f32p_5, data_f32p_6, data_f32p_7 },
	.size = sizeof(float) * 4
};

struct data dsp_7p1_remapped = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 8,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
			SPA_AUDIO_CHANNEL_SL,
			SPA_AUDIO_CHANNEL_SR,
		}),
	.ports = 8,
	.planes = 1,
	.data = { data_f32p_1, data_f32p_2, data_f32p_3, data_f32p_4, data_f32p_7, data_f32p_8, data_f32p_5, data_f32p_6 },
	.size = sizeof(data_f32p_1)
};

struct data dsp_5p1_remapped_2 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_dsp,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FL,
		}),
	.ports = 6,
	.planes = 1,
	.data = { data_f32p_3, data_f32p_4, data_f32p_5, data_f32p_6, data_f32p_2, data_f32p_1, },
	.size = sizeof(float) * 4
};

struct data conv_f32_48000_5p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 1,
	.planes = 1,
	.data = { data_f32_5p1 },
	.size = sizeof(data_f32_5p1)
};

struct data conv_f32_48000_5p1_remapped = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
		}),
	.ports = 1,
	.planes = 1,
	.data = { data_f32_5p1_remapped },
	.size = sizeof(data_f32_5p1_remapped)
};

struct data conv_f32p_48000_5p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32P,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 1,
	.planes = 6,
	.data = { data_f32p_1, data_f32p_2, data_f32p_3, data_f32p_4, data_f32p_5, data_f32p_6, },
	.size = sizeof(float) * 4
};

struct data conv_f32_48000_6p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 7,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RC,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 1,
	.planes = 1,
	.data = { data_f32_6p1 },
	.size = sizeof(data_f32_6p1)
};

struct data conv_f32_48000_6p1_from_5p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 7,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RC,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 1,
	.planes = 1,
	.data = { data_f32_6p1_from_5p1 },
	.size = sizeof(data_f32_6p1_from_5p1)
};

struct data conv_f32_48000_6p1_side = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 7,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RC,
			SPA_AUDIO_CHANNEL_SL,
			SPA_AUDIO_CHANNEL_SR,
		}),
	.ports = 1,
	.planes = 1,
	.data = { data_f32_6p1 },
	.size = sizeof(data_f32_6p1)
};

struct data conv_f32p_48000_6p1 = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32P,
		.rate = 48000,
		.channels = 7,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
			SPA_AUDIO_CHANNEL_RC,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
		}),
	.ports = 1,
	.planes = 7,
	.data = { data_f32p_1, data_f32p_2, data_f32p_3, data_f32p_4, data_f32p_5, data_f32p_6, data_f32p_7 },
	.size = sizeof(float) * 4
};

struct data conv_f32p_48000_5p1_remapped = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32P,
		.rate = 48000,
		.channels = 6,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
		}),
	.ports = 1,
	.planes = 6,
	.data = { data_f32p_1, data_f32p_2, data_f32p_5, data_f32p_6, data_f32p_3, data_f32p_4, },
	.size = sizeof(float) * 4
};

struct data conv_f32_48000_7p1_remapped = {
	.mode = SPA_PARAM_PORT_CONFIG_MODE_convert,
	.info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = 48000,
		.channels = 8,
		.position = {
			SPA_AUDIO_CHANNEL_FL,
			SPA_AUDIO_CHANNEL_FR,
			SPA_AUDIO_CHANNEL_SL,
			SPA_AUDIO_CHANNEL_SR,
			SPA_AUDIO_CHANNEL_RL,
			SPA_AUDIO_CHANNEL_RR,
			SPA_AUDIO_CHANNEL_FC,
			SPA_AUDIO_CHANNEL_LFE,
		}),
	.ports = 1,
	.planes = 1,
	.data = { data_f32_7p1_remapped, },
	.size = sizeof(data_f32_7p1_remapped)
};

static int test_convert_remap_dsp(struct context *ctx)
{
	run_convert(ctx, &dsp_5p1, &conv_f32_48000_5p1);
	run_convert(ctx, &dsp_5p1, &conv_f32p_48000_5p1);
	run_convert(ctx, &dsp_5p1, &conv_f32_48000_5p1_remapped);
	run_convert(ctx, &dsp_5p1, &conv_f32p_48000_5p1_remapped);
	run_convert(ctx, &dsp_5p1_remapped, &conv_f32_48000_5p1);
	run_convert(ctx, &dsp_5p1_remapped, &conv_f32p_48000_5p1);
	run_convert(ctx, &dsp_5p1_remapped, &conv_f32_48000_5p1_remapped);
	run_convert(ctx, &dsp_5p1_remapped, &conv_f32p_48000_5p1_remapped);
	run_convert(ctx, &dsp_5p1_remapped_2, &conv_f32_48000_5p1);
	run_convert(ctx, &dsp_5p1_remapped_2, &conv_f32p_48000_5p1);
	run_convert(ctx, &dsp_5p1_remapped_2, &conv_f32_48000_5p1_remapped);
	run_convert(ctx, &dsp_5p1_remapped_2, &conv_f32p_48000_5p1_remapped);
	run_convert(ctx, &dsp_6p1, &conv_f32p_48000_6p1);
	run_convert(ctx, &dsp_6p1, &conv_f32_48000_6p1);
	run_convert(ctx, &dsp_6p1_side, &conv_f32_48000_6p1_side);

	run_convert(ctx, &dsp_5p1, &conv_f32_48000_6p1_from_5p1);
	return 0;
}

static int test_convert_remap_conv(struct context *ctx)
{
	run_convert(ctx, &conv_f32_48000_5p1, &dsp_5p1);
	run_convert(ctx, &conv_f32_48000_5p1, &dsp_5p1_remapped);
	run_convert(ctx, &conv_f32_48000_5p1, &dsp_5p1_remapped_2);
	run_convert(ctx, &conv_f32p_48000_5p1, &dsp_5p1);
	run_convert(ctx, &conv_f32p_48000_5p1, &dsp_5p1_remapped);
	run_convert(ctx, &conv_f32p_48000_5p1, &dsp_5p1_remapped_2);
	run_convert(ctx, &conv_f32_48000_5p1_remapped, &dsp_5p1);
	run_convert(ctx, &conv_f32_48000_5p1_remapped, &dsp_5p1_remapped);
	run_convert(ctx, &conv_f32_48000_5p1_remapped, &dsp_5p1_remapped_2);
	run_convert(ctx, &conv_f32p_48000_5p1_remapped, &dsp_5p1);
	run_convert(ctx, &conv_f32p_48000_6p1, &dsp_6p1);
	run_convert(ctx, &conv_f32_48000_6p1, &dsp_6p1);
	run_convert(ctx, &conv_f32_48000_6p1_side, &dsp_6p1_side);
	run_convert(ctx, &conv_f32p_48000_5p1_remapped, &dsp_5p1_remapped);
	run_convert(ctx, &conv_f32_48000_7p1_remapped, &dsp_7p1_remapped);
	run_convert(ctx, &conv_f32p_48000_5p1_remapped, &dsp_5p1_remapped_2);

	run_convert(ctx, &conv_f32_48000_6p1, &dsp_5p1_from_6p1);
	run_convert(ctx, &conv_f32_48000_6p1_side, &dsp_5p1_from_6p1);
	run_convert(ctx, &conv_f32_48000_6p1, &dsp_5p1_remapped_from_6p1);
	return 0;
}

int main(int argc, char *argv[])
{
	struct context ctx;

	spa_zero(ctx);

	setup_context(&ctx);

	test_init_state(&ctx);
	test_set_in_format(&ctx);
	test_split_setup1(&ctx);
	test_split_setup2(&ctx);
	test_convert_setup1(&ctx);
	test_set_out_format(&ctx);
	test_merge_setup1(&ctx);
	test_set_out_format2(&ctx);
	test_merge_setup2(&ctx);
	test_convert_setup2(&ctx);
	test_set_in_format2(&ctx);
	test_set_out_format(&ctx);

	test_convert_remap_dsp(&ctx);
	test_convert_remap_conv(&ctx);

	clean_context(&ctx);

	return 0;
}
