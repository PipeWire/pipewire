/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Running audioadapter nodes.
 [title]
 [doc]
 Runs an output audioadapter using audiotestsrc as follower
 with an input audioadapter using alsa-pcm-sink as follower
 for easy testing.
 [doc]
 */

#include "config.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <getopt.h>

#include <spa/control/control.h>
#include <spa/graph/graph.h>
#include <spa/support/plugin.h>
#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

static SPA_LOG_IMPL(default_log);

#define MIN_LATENCY	    1024
#define CONTROL_BUFFER_SIZE 32768

#define DEFAULT_RAMP_SAMPLES (64*1*1024)
#define DEFAULT_RAMP_STEP_SAMPLES 200

#define DEFAULT_RAMP_TIME 2000 // 2 seconds
#define DEFAULT_RAMP_STEP_TIME 5000 // 5 milli seconds

#define DEFAULT_DEVICE "hw:0,0"

#define LINEAR "linear"
#define CUBIC "cubic"
#define DEFAULT_SCALE SPA_AUDIO_VOLUME_RAMP_LINEAR

#define NON_NATIVE "non-native"
#define NATIVE "native"
#define DEFAULT_MODE NON_NATIVE


struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	struct spa_data datas[1];
	struct spa_chunk chunks[1];
};

struct data {
	const char *plugin_dir;
	struct spa_log *log;
	struct spa_system *system;
	struct spa_loop *loop;
	struct spa_loop_control *control;
	struct spa_support support[5];
	uint32_t n_support;

	struct spa_graph graph;
	struct spa_graph_state graph_state;
	struct spa_graph_node graph_source_node;
	struct spa_graph_node graph_sink_node;
	struct spa_graph_state graph_source_state;
	struct spa_graph_state graph_sink_state;
	struct spa_graph_port graph_source_port_0;
	struct spa_graph_port graph_sink_port_0;

	struct spa_node *source_follower_node;  // audiotestsrc
	struct spa_node *source_node;  // adapter for audiotestsrc
	struct spa_node *sink_follower_node;  // alsa-pcm-sink
	struct spa_node *sink_node;  // adapter for alsa-pcm-sink

	struct spa_io_position position;
	struct spa_io_buffers source_sink_io[1];
	struct spa_buffer *source_buffers[1];
	struct buffer source_buffer[1];

	struct spa_io_buffers control_io;
	struct spa_buffer *control_buffers[1];
	struct buffer control_buffer[1];

	int buffer_count;
	bool start_fade_in;
	double volume_accum;
	uint32_t volume_offs;

	const char *alsa_device;

	const char *mode;
	enum spa_audio_volume_ramp_scale scale;

	uint32_t volume_ramp_samples;
	uint32_t volume_ramp_step_samples;
	uint32_t volume_ramp_time;
	uint32_t volume_ramp_step_time;

	bool running;
	pthread_t thread;
};

static int load_handle (struct data *data, struct spa_handle **handle, const
	char *lib, const char *name, struct spa_dict *info)
{
	int res;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t i;
	char *path;

	if ((path = spa_aprintf("%s/%s", data->plugin_dir, lib)) == NULL)
		return -ENOMEM;

	hnd = dlopen(path, RTLD_NOW);
	free(path);

	if (hnd == NULL) {
		printf("can't load %s: %s\n", lib, dlerror());
		return -ENOENT;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find enum function\n");
		res = -ENOENT;
		goto exit_cleanup;
	}

	for (i = 0;;) {
		const struct spa_handle_factory *factory;

		if ((res = enum_func(&factory, &i)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %s\n", spa_strerror(res));
			break;
		}
		if (factory->version < 1)
			continue;
		if (!spa_streq(factory->name, name))
			continue;

		*handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
		if ((res = spa_handle_factory_init(factory, *handle,
						info, data->support,
						data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			goto exit_cleanup;
		}
		return 0;
	}
	return -EBADF;

exit_cleanup:
	dlclose(hnd);
	return res;
}

static int init_data(struct data *data)
{
	int res;
	const char *str;
	struct spa_handle *handle = NULL;
	struct spa_dict_item items [2];
	struct spa_dict info;
	void *iface;

	if ((str = getenv("SPA_PLUGIN_DIR")) == NULL)
		str = PLUGINDIR;
	data->plugin_dir = str;

	/* start not doing fade-in */
	data->start_fade_in = true;
	data->volume_accum = 0.0;
	data->volume_offs = 0;

	/* init the graph */
	spa_graph_init(&data->graph, &data->graph_state);

	/* enable the debug messages in SPA */
	items [0] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_TIMESTAMP, "true");
	info = SPA_DICT_INIT(items, 1);
	if ((res = load_handle (data, &handle, "support/libspa-support.so",
			SPA_NAME_SUPPORT_LOG, &info)) < 0)
		return res;
	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Log, &iface)) < 0) {
		printf("can't get System interface %d\n", res);
		return res;
	}
	data->log = iface;
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, data->log);

	/* load and set support system */
	if ((res = load_handle(data, &handle,
			"support/libspa-support.so",
			SPA_NAME_SUPPORT_SYSTEM, NULL)) < 0)
		return res;
	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_System, &iface)) < 0) {
		printf("can't get System interface %d\n", res);
		return res;
	}
	data->system = iface;
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_System, data->system);
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataSystem, data->system);

	/* load and set support loop and loop control */
	if ((res = load_handle(data, &handle,
			"support/libspa-support.so",
			SPA_NAME_SUPPORT_LOOP, NULL)) < 0)
		return res;

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Loop, &iface)) < 0) {
		printf("can't get interface %d\n", res);
		return res;
	}
	data->loop = iface;
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Loop, data->loop);
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, data->loop);
	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_LoopControl, &iface)) < 0) {
		printf("can't get interface %d\n", res);
		return res;
	}
	data->control = iface;

	if ((str = getenv("SPA_DEBUG")))
		data->log->level = atoi(str);

	return 0;
}

static int make_node(struct data *data, struct spa_node **node, const char *lib,
    const char *name, const struct spa_dict *props)
{
	struct spa_handle *handle;
	int res = 0;
	void *hnd = NULL;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t i;
	char *path;

	if ((path = spa_aprintf("%s/%s", data->plugin_dir, lib)) == NULL)
		return -ENOMEM;

	hnd = dlopen(path, RTLD_NOW);
	free(path);

	if (hnd == NULL) {
		printf("can't load %s: %s\n", lib, dlerror());
		return -ENOENT;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find enum function\n");
		res = -ENOENT;
		goto exit_cleanup;
	}

	for (i = 0;;) {
		const struct spa_handle_factory *factory;
		void *iface;

		if ((res = enum_func(&factory, &i)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %s\n", spa_strerror(res));
			break;
		}
		if (factory->version < 1)
			continue;
		if (!spa_streq(factory->name, name))
			continue;

		handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
		if ((res =
		     spa_handle_factory_init(factory, handle, props, data->support,
					     data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			goto exit_cleanup;
		}
		if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node, &iface)) < 0) {
			printf("can't get interface %d\n", res);
			goto exit_cleanup;
		}
		*node = iface;
		return 0;
	}
	return -EBADF;

exit_cleanup:
	dlclose(hnd);
	return res;
}

static int get_ramp_samples(struct data *data)
{
	int samples = -1;
	if (data->volume_ramp_samples)
		samples = data->volume_ramp_samples;
	else if (data->volume_ramp_time) {
		samples = (data->volume_ramp_time * 48000) / 1000;
	}
	if (!samples)
		samples = -1;

	return samples;
}

static int get_ramp_step_samples(struct data *data)
{
	int samples = -1;
	if (data->volume_ramp_step_samples)
		samples = data->volume_ramp_step_samples;
	else if (data->volume_ramp_step_time) {
		/* convert the step time which is in nano seconds to seconds */
		samples = (data->volume_ramp_step_time / 1000) * (48000 / 1000);
	}
	if (!samples)
		samples = -1;

	return samples;
}

static double get_volume_at_scale(struct data *data)
{
	if (data->scale == SPA_AUDIO_VOLUME_RAMP_LINEAR)
		return data->volume_accum;
	else if (data->scale == SPA_AUDIO_VOLUME_RAMP_CUBIC)
		return (data->volume_accum * data->volume_accum * data->volume_accum);

	return 0.0;
}

static int fade_in(struct data *data)
{
	printf("fading in\n");
	if (spa_streq (data->mode, NON_NATIVE)) {
		struct spa_pod_builder b;
		struct spa_pod_frame f[1];
		void *buffer = data->control_buffer->datas[0].data;
		int ramp_samples = get_ramp_samples(data);
		int ramp_step_samples = get_ramp_step_samples(data);
		double step_size = ((double) ramp_step_samples / (double) ramp_samples);
		uint32_t buffer_size = data->control_buffer->datas[0].maxsize;
		data->control_buffer->datas[0].chunk[0].size = buffer_size;

		spa_pod_builder_init(&b, buffer, buffer_size);
		spa_pod_builder_push_sequence(&b, &f[0], 0);
		data->volume_offs = 0;
		do {
			// printf("volume level %f offset %d\n", get_volume_at_scale(data), data->volume_offs);
			spa_pod_builder_control(&b, data->volume_offs, SPA_CONTROL_Properties);
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, 0,
				SPA_PROP_volume, SPA_POD_Float(get_volume_at_scale(data)));
			data->volume_accum += step_size;
			data->volume_offs += ramp_step_samples;
		} while (data->volume_accum < 1.0);
		spa_pod_builder_pop(&b, &f[0]);
	}
	else {
		struct spa_pod_builder b;
		struct spa_pod *props;
		int res = 0;
		uint8_t buffer[1024];
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		props = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, 0,
			SPA_PROP_volume, SPA_POD_Float(1.0),
			SPA_PROP_volumeRampSamples, SPA_POD_Int(data->volume_ramp_samples),
			SPA_PROP_volumeRampStepSamples, SPA_POD_Int(data->volume_ramp_step_samples),
			SPA_PROP_volumeRampTime, SPA_POD_Int(data->volume_ramp_time),
			SPA_PROP_volumeRampStepTime, SPA_POD_Int(data->volume_ramp_step_time),
			SPA_PROP_volumeRampScale, SPA_POD_Id(data->scale));
		if ((res = spa_node_set_param(data->sink_node, SPA_PARAM_Props, 0, props)) < 0) {
			printf("can't call volramp set params %d\n", res);
			return res;
		}
	}

	return 0;
}

static int fade_out(struct data *data)
{
	printf("fading out\n");
	if (spa_streq (data->mode, NON_NATIVE)) {
		struct spa_pod_builder b;
		struct spa_pod_frame f[1];
		int ramp_samples = get_ramp_samples(data);
		int ramp_step_samples = get_ramp_step_samples(data);
		double step_size = ((double) ramp_step_samples / (double) ramp_samples);


		void *buffer = data->control_buffer->datas[0].data;
		uint32_t buffer_size = data->control_buffer->datas[0].maxsize;
		data->control_buffer->datas[0].chunk[0].size = buffer_size;

		spa_pod_builder_init(&b, buffer, buffer_size);
		spa_pod_builder_push_sequence(&b, &f[0], 0);
		data->volume_offs = ramp_step_samples;
		do {
			// printf("volume level %f offset %d\n", get_volume_at_scale(data), data->volume_offs);
			spa_pod_builder_control(&b, data->volume_offs, SPA_CONTROL_Properties);
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, 0,
				SPA_PROP_volume, SPA_POD_Float(get_volume_at_scale(data)));
			data->volume_accum -= step_size;
			data->volume_offs += ramp_step_samples;
		} while (data->volume_accum > 0.0);
		spa_pod_builder_pop(&b, &f[0]);
	} else {
		struct spa_pod_builder b;
		uint8_t buffer[1024];
		struct spa_pod *props;
		int res = 0;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		props = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, 0,
			SPA_PROP_volume, SPA_POD_Float(0.0),
			SPA_PROP_volumeRampSamples, SPA_POD_Int(data->volume_ramp_samples),
			SPA_PROP_volumeRampStepSamples, SPA_POD_Int(data->volume_ramp_step_samples),
			SPA_PROP_volumeRampTime, SPA_POD_Int(data->volume_ramp_time),
			SPA_PROP_volumeRampStepTime, SPA_POD_Int(data->volume_ramp_step_time),
			SPA_PROP_volumeRampScale, SPA_POD_Id(data->scale));
		if ((res = spa_node_set_param(data->sink_node, SPA_PARAM_Props, 0, props)) < 0) {
			printf("can't call volramp set params %d\n", res);
			return res;
		}
	}

	return 0;
}

static void do_fade(struct data *data)
{
	if (spa_streq (data->mode, NON_NATIVE)) {
		switch (data->control_io.status) {
		case SPA_STATUS_OK:
		case SPA_STATUS_NEED_DATA:
			break;
		case SPA_STATUS_HAVE_DATA:
		case SPA_STATUS_STOPPED:
		default:
			return;
		}
	}

	/* fade */
	if (data->start_fade_in)
		fade_in(data);
	else
		fade_out(data);

	if (spa_streq (data->mode, NON_NATIVE)) {
		data->control_io.status = SPA_STATUS_HAVE_DATA;
		data->control_io.buffer_id = 0;
	}

	/* alternate */
	data->start_fade_in = !data->start_fade_in;
}

static int on_sink_node_ready(void *_data, int status)
{
	struct data *data = _data;
	int runway = (get_ramp_samples(data) / 1024);

	/* only do fade in/out when buffer count is 0 */
	if (data->buffer_count == 0)
		do_fade(data);

	/* update buffer count */
	data->buffer_count++;
	if (data->buffer_count > (runway * 2))
		  data->buffer_count = 0;

	spa_graph_node_process(&data->graph_source_node);
	spa_graph_node_process(&data->graph_sink_node);
	return 0;
}

static const struct spa_node_callbacks sink_node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = on_sink_node_ready,
};

static int make_nodes(struct data *data)
{
	int res = 0;
	struct spa_pod *props;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	char value[32];
	struct spa_dict_item items[2];
	struct spa_audio_info_raw info;
	struct spa_pod *param;
	float initial_volume = 0.0;

	items[0] = SPA_DICT_ITEM_INIT("clock.quantum-limit", "8192");

	/* make the source node (audiotestsrc) */
	if ((res = make_node(data, &data->source_follower_node,
					"audiotestsrc/libspa-audiotestsrc.so",
					"audiotestsrc",
					&SPA_DICT_INIT(items, 1))) < 0) {
		printf("can't create source follower node (audiotestsrc): %d\n", res);
		return res;
	}
	printf("created source follower node %p\n", data->source_follower_node);

	/* set the format on the source */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_format_audio_raw_build(&b, 0,
			&SPA_AUDIO_INFO_RAW_INIT(
				.format = SPA_AUDIO_FORMAT_S16,
				.rate = 48000,
				.channels = 2 ));
	if ((res = spa_node_port_set_param(data->source_follower_node,
					   SPA_DIRECTION_OUTPUT, 0,
					   SPA_PARAM_Format, 0, param)) < 0) {
		printf("can't set format on follower node (audiotestsrc): %d\n", res);
		return res;
	}

	/* make the source adapter node */
	snprintf(value, sizeof(value), "pointer:%p", data->source_follower_node);
	items[1] = SPA_DICT_ITEM_INIT("audio.adapt.follower", value);
	if ((res = make_node(data, &data->source_node,
					"audioconvert/libspa-audioconvert.so",
					SPA_NAME_AUDIO_ADAPT,
					&SPA_DICT_INIT(items, 2))) < 0) {
		printf("can't create source adapter node: %d\n", res);
		return res;
	}
	printf("created source adapter node %p\n", data->source_node);

	/* setup the source node props */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	props = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Props, 0,
		SPA_PROP_frequency, SPA_POD_Float(600.0),
		SPA_PROP_volume,    SPA_POD_Float(0.5),
		SPA_PROP_live,	    SPA_POD_Bool(false));
	if ((res = spa_node_set_param(data->source_node, SPA_PARAM_Props, 0, props)) < 0) {
		printf("can't setup source follower node %d\n", res);
		return res;
	}

	/* setup the source node port config */
	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_F32P;
	info.channels = 1;
	info.rate = 48000;
	info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig,	SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(SPA_DIRECTION_OUTPUT),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
		SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(param));
	if ((res = spa_node_set_param(data->source_node, SPA_PARAM_PortConfig, 0, param) < 0)) {
		printf("can't setup source node %d\n", res);
		return res;
	}

	/* make the sink follower node (alsa-pcm-sink) */
	if ((res = make_node(data, &data->sink_follower_node,
					"alsa/libspa-alsa.so",
					SPA_NAME_API_ALSA_PCM_SINK,
					&SPA_DICT_INIT(items, 1))) < 0) {
		printf("can't create sink follower node (alsa-pcm-sink): %d\n", res);
		return res;
	}
	printf("created sink follower node %p\n", data->sink_follower_node);

	/* make the sink adapter node */
	snprintf(value, sizeof(value), "pointer:%p", data->sink_follower_node);
	items[1] = SPA_DICT_ITEM_INIT("audio.adapt.follower", value);
	if ((res = make_node(data, &data->sink_node,
					"audioconvert/libspa-audioconvert.so",
					SPA_NAME_AUDIO_ADAPT,
					&SPA_DICT_INIT(items, 2))) < 0) {
		printf("can't create sink adapter node: %d\n", res);
		return res;
	}
	printf("created sink adapter node %p\n", data->sink_node);

	/* add sink follower node callbacks */
	spa_node_set_callbacks(data->sink_node, &sink_node_callbacks, data);

	/* setup the sink node props */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	props = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Props, 0,
		SPA_PROP_device, SPA_POD_String(data->alsa_device),
		SPA_PROP_minLatency, SPA_POD_Int(MIN_LATENCY));
	if ((res = spa_node_set_param(data->sink_follower_node, SPA_PARAM_Props, 0, props)) < 0) {
		printf("can't setup sink follower node %d\n", res);
		return res;
	}
	printf("Selected (%s) alsa device\n", data->alsa_device);

	if (!data->start_fade_in)
		initial_volume = 1.0;

	/* setup the sink node port config */
	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_F32P;
	info.channels = 1;
	info.rate = 48000;
	info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);

	if (spa_streq (data->mode, NON_NATIVE))
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
			SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
			SPA_PARAM_PORT_CONFIG_control, SPA_POD_Bool(true),
			SPA_PARAM_PORT_CONFIG_format, SPA_POD_Pod(param));
	else
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
			SPA_PARAM_PORT_CONFIG_mode, SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
			SPA_PARAM_PORT_CONFIG_format, SPA_POD_Pod(param));


	if ((res = spa_node_set_param(data->sink_node, SPA_PARAM_PortConfig, 0, param) < 0)) {
		printf("can't setup sink node %d\n", res);
		return res;
	}

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	props = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Props, 0,
		SPA_PROP_volume, SPA_POD_Float(initial_volume));
	if ((res = spa_node_set_param(data->sink_node, SPA_PARAM_Props, 0, props)) < 0) {
		printf("can't configure initial volume %d\n", res);
		return res;
	}

	/* set io buffers on source and sink nodes */
	data->source_sink_io[0] = SPA_IO_BUFFERS_INIT;
	if ((res = spa_node_port_set_io(data->source_node,
			SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers,
			&data->source_sink_io[0], sizeof(data->source_sink_io[0]))) < 0) {
		printf("can't set io buffers on port 0 of source node: %d\n", res);
		return res;
	}
	printf("set io buffers on port 0 of source node %p\n", data->source_node);


	if ((res = spa_node_port_set_io(data->sink_node,
			  SPA_DIRECTION_INPUT, 0,
			  SPA_IO_Buffers,
			  &data->source_sink_io[0], sizeof(data->source_sink_io[0]))) < 0) {
		printf("can't set io buffers on port 0 of sink node: %d\n", res);
		return res;
	}
	printf("set io buffers on port 0 of sink node %p\n", data->sink_node);

	/* set io position and clock on source and sink nodes */
	data->position.clock.target_rate = SPA_FRACTION(1, 48000);
	data->position.clock.target_duration = 1024;
	data->position.clock.rate = data->position.clock.target_rate;
	data->position.clock.duration = data->position.clock.target_duration;
	if ((res = spa_node_set_io(data->source_node,
			SPA_IO_Position,
			&data->position, sizeof(data->position))) < 0) {
		printf("can't set io position on source node: %d\n", res);
		return res;
	}
	if ((res = spa_node_set_io(data->sink_node,
			  SPA_IO_Position,
			  &data->position, sizeof(data->position))) < 0) {
		printf("can't set io position on sink node: %d\n", res);
		return res;
	}
	if ((res = spa_node_set_io(data->source_node,
			SPA_IO_Clock,
			&data->position.clock, sizeof(data->position.clock))) < 0) {
		printf("can't set io clock on source node: %d\n", res);
		return res;
	}
	if ((res = spa_node_set_io(data->sink_node,
			  SPA_IO_Clock,
			  &data->position.clock, sizeof(data->position.clock))) < 0) {
		printf("can't set io clock on sink node: %d\n", res);
		return res;
	}

	if (spa_streq (data->mode, NON_NATIVE)) {
		/* set io buffers on control port of sink node */
		if ((res = spa_node_port_set_io(data->sink_node,
			SPA_DIRECTION_INPUT, 1,
			SPA_IO_Buffers,
			&data->control_io, sizeof(data->control_io))) < 0) {
			printf("can't set io buffers on control port 1 of sink node\n");
			return res;
		}
	}
	/* add source node to the graph */
	spa_graph_node_init(&data->graph_source_node, &data->graph_source_state);
	spa_graph_node_set_callbacks(&data->graph_source_node, &spa_graph_node_impl_default, data->source_node);
	spa_graph_node_add(&data->graph, &data->graph_source_node);
	spa_graph_port_init(&data->graph_source_port_0, SPA_DIRECTION_OUTPUT, 0, 0);
	spa_graph_port_add(&data->graph_source_node, &data->graph_source_port_0);

	/* add sink node to the graph */
	spa_graph_node_init(&data->graph_sink_node, &data->graph_sink_state);
	spa_graph_node_set_callbacks(&data->graph_sink_node, &spa_graph_node_impl_default, data->sink_node);
	spa_graph_node_add(&data->graph, &data->graph_sink_node);
	spa_graph_port_init(&data->graph_sink_port_0, SPA_DIRECTION_INPUT, 0, 0);
	spa_graph_port_add(&data->graph_sink_node, &data->graph_sink_port_0);

	/* link source and sink nodes */
	spa_graph_port_link(&data->graph_source_port_0, &data->graph_sink_port_0);

	return res;
}

static void
init_buffer(struct data *data, struct spa_buffer **bufs, struct buffer *ba, int n_buffers,
	    size_t size)
{
	int i;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &ba[i];
		bufs[i] = &b->buffer;

		b->buffer.metas = b->metas;
		b->buffer.n_metas = 1;
		b->buffer.datas = b->datas;
		b->buffer.n_datas = 1;

		b->header.flags = 0;
		b->header.seq = 0;
		b->header.pts = 0;
		b->header.dts_offset = 0;
		b->metas[0].type = SPA_META_Header;
		b->metas[0].data = &b->header;
		b->metas[0].size = sizeof(b->header);

		b->datas[0].type = SPA_DATA_MemPtr;
		b->datas[0].flags = 0;
		b->datas[0].fd = -1;
		b->datas[0].mapoffset = 0;
		b->datas[0].maxsize = size;
		b->datas[0].data = malloc(size);
		b->datas[0].chunk = &b->chunks[0];
		b->datas[0].chunk->offset = 0;
		b->datas[0].chunk->size = 0;
		b->datas[0].chunk->stride = 0;
	}
}

static int negotiate_formats(struct data *data)
{
	int res;
	struct spa_pod *filter = NULL, *param = NULL;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	uint32_t state = 0;
	size_t buffer_size = 1024;

	/* set the sink and source formats */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_format_audio_dsp_build(&b, 0,
		&SPA_AUDIO_INFO_DSP_INIT(
			.format = SPA_AUDIO_FORMAT_F32P));
	if ((res = spa_node_port_set_param(data->source_node,
			SPA_DIRECTION_OUTPUT, 0, SPA_PARAM_Format, 0, param)) < 0) {
		printf("can't set format on source node: %d\n", res);
		return res;
	}
	if ((res = spa_node_port_set_param(data->sink_node,
			SPA_DIRECTION_INPUT, 0, SPA_PARAM_Format, 0, param)) < 0) {
		printf("can't set format on source node: %d\n", res);
		return res;
	}

	if (spa_streq (data->mode, NON_NATIVE)) {
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		if ((res = spa_node_port_set_param(data->sink_node,
			SPA_DIRECTION_INPUT, 1, SPA_PARAM_Format, 0, param)) < 0) {
			printf("can't set format on control port of source node: %d\n", res);
			return res;
		}
	}

	/* get the source node buffer size */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if ((res = spa_node_port_enum_params_sync(data->source_node,
			SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Buffers, &state, filter, &param, &b)) != 1)
		return res ? res : -ENOTSUP;
	spa_pod_fixate(param);
	if ((res = spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamBuffers, NULL,
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(&buffer_size))) < 0)
		return res;

	/* use buffers on the source and sink */
	init_buffer(data, data->source_buffers, data->source_buffer, 1, buffer_size);
	if ((res = spa_node_port_use_buffers(data->source_node,
		SPA_DIRECTION_OUTPUT, 0, 0, data->source_buffers, 1)) < 0)
		return res;
	printf("allocated and assigned buffer(%ld) to source node %p\n", buffer_size, data->source_node);
	if ((res = spa_node_port_use_buffers(data->sink_node,
		SPA_DIRECTION_INPUT, 0, 0, data->source_buffers, 1)) < 0)
		return res;
	printf("allocated and assigned buffers to sink node %p\n", data->sink_node);

	if (spa_streq (data->mode, NON_NATIVE)) {
		/* Set the control buffers */
		init_buffer(data, data->control_buffers, data->control_buffer, 1, CONTROL_BUFFER_SIZE);
		if ((res = spa_node_port_use_buffers(data->sink_node,
			SPA_DIRECTION_INPUT, 1, 0, data->control_buffers, 1)) < 0)
			return res;
		printf("allocated and assigned control buffers(%d) to sink node %p\n", CONTROL_BUFFER_SIZE, data->sink_node);
	}

	return 0;
}

static void *loop(void *user_data)
{
	struct data *data = user_data;

	printf("enter thread\n");
	spa_loop_control_enter(data->control);

	while (data->running) {
		spa_loop_control_iterate(data->control, -1);
	}

	printf("leave thread\n");
	spa_loop_control_leave(data->control);
	return NULL;

	return NULL;
}

static void run_async_sink(struct data *data)
{
	int res, err;
	struct spa_command cmd;

	cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	if ((res = spa_node_send_command(data->source_node, &cmd)) < 0)
		printf("got error %d\n", res);
	printf("Source node started\n");
	if ((res = spa_node_send_command(data->sink_node, &cmd)) < 0)
		printf("got error %d\n", res);
	printf("sink node started\n");

	spa_loop_control_leave(data->control);

	data->running = true;
	if ((err = pthread_create(&data->thread, NULL, loop, data)) != 0) {
		printf("can't create thread: %d %s", err, strerror(err));
		data->running = false;
	}

	printf("sleeping for 1000 seconds\n");
	sleep(1000);

	if (data->running) {
		data->running = false;
		pthread_join(data->thread, NULL);
	}

	spa_loop_control_enter(data->control);

	cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	if ((res = spa_node_send_command(data->source_node, &cmd)) < 0)
		printf("got error %d\n", res);
	if ((res = spa_node_send_command(data->sink_node, &cmd)) < 0)
		printf("got error %d\n", res);
}

static char *getscale(uint32_t scale)
{
	char *scale_s = NULL;

	if (scale == SPA_AUDIO_VOLUME_RAMP_LINEAR)
		scale_s = LINEAR;
	else if (scale == SPA_AUDIO_VOLUME_RAMP_CUBIC)
		scale_s = CUBIC;

	return scale_s;
}
static void show_help(struct data *data, const char *name, bool error)
{
	fprintf(error ? stderr : stdout, "%s [options] [command]\n"
		"  -h, --help              Show this help\n"
		"  -d, --alsa-device       ALSA device(\"aplay -l\" for more info) to play the samples on(default %s)\n"
		"  -m, --mode              Volume Ramp Mode(\"NonNative\"(via Control Port) \"Native\" (via Volume Ramp Params of AudioAdapter plugin)) (default %s)\n"
		"  -s, --ramp-samples      SPA_PROP_volumeRampSamples(Samples to ramp the volume over)(default %d)\n"
		"  -a, --ramp-step-samples SPA_PROP_volumeRampStepSamples(Step or incremental Samples to ramp the volume over)(default %d)\n"
		"  -t, --ramp-time         SPA_PROP_volumeRampTime(Time to ramp the volume over in  msec)(default %d)\n"
		"  -i, --ramp-step-time    SPA_PROP_volumeRampStepTime(Step or incremental Time to ramp the volume over in nano sec)(default %d)\n"
		"  -c, --scale             SPA_PROP_volumeRampScale(the scale or graph to used to ramp the volume)(\"linear\" or \"cubic\")(default %s)\n"
		"examples:\n"
		"adapter-control\n"
		"-->when invoked with out any params, ramps volume with default values\n"
		"adapter-control --ramp-samples=70000, rest of the parameters are defaults\n"
		"-->ramps volume over 70000 samples(it is 1.45 seconds)\n"
		"adapter-control --alsa-device=hw:0,0 --ramp-samples=70000\n"
		"-->ramps volume on \"hw:0,0\" alsa device over 70000 samples\n"
		"adapter-control --alsa-device=hw:0,0 --ramp-samples=70000 --mode=native\n"
		"-->ramps volume on \"hw:0,0\" alsa device over 70000 samples in native mode\n"
		"adapter-control --alsa-device=hw:0,0 --ramp-time=1000 --mode=native\n"
		"-->ramps volume on \"hw:0,0\" alsa device over 1000 msec in native mode\n"
		"adapter-control --alsa-device=hw:0,0 --ramp-time=1000 --ramp-step-time=5000 --mode=native\n"
		"-->ramps volume on \"hw:0,0\" alsa device over 1000 msec in steps of 5000 nano seconds(5 msec)in native mode\n"
		"adapter-control --alsa-device=hw:0,0 --ramp-samples=70000 --ramp-step-samples=200 --mode=native\n"
		"-->ramps volume on \"hw:0,0\" alsa device over 70000 samples with a step size of 200 samples in native mode\n"
		"adapter-control --alsa-device=hw:1,0 --scale=linear\n"
		"-->ramps volume on \"hw:1,0\" in linear volume scale, one can leave choose to not use the linear scale here as it is the default\n"
		"adapter-control --alsa-device=hw:1,0 --ramp-samples=70000 --scale=cubic\n"
		"-->ramps volume on \"hw:1,0\" alsa device over 70000 samples deploying cubic volume scale\n"
		"adapter-control --alsa-device=hw:1,0 --ramp-samples=70000 --mode=native --scale=cubic\n"
		"-->ramps volume on \"hw:1,0\" alsa device over 70000 samples deploying cubic volume scale in native mode\n"
		"adapter-control --alsa-device=hw:1,0 --ramp-time=3000 --scale=cubic --mode=native\n"
		"-->ramps volume on \"hw:1,0\" alsa device over 3 seconds samples with a step size of 200 samples in native mode\n",
		name,
		DEFAULT_DEVICE,
		DEFAULT_MODE,
		DEFAULT_RAMP_SAMPLES,
		DEFAULT_RAMP_STEP_SAMPLES,
		DEFAULT_RAMP_TIME,
		DEFAULT_RAMP_STEP_TIME,
		getscale(DEFAULT_SCALE));
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	int res = 0, c;

	/* default values*/
	data.volume_ramp_samples = DEFAULT_RAMP_SAMPLES;
	data.volume_ramp_step_samples = DEFAULT_RAMP_STEP_SAMPLES;
	data.alsa_device = DEFAULT_DEVICE;
	data.mode = DEFAULT_MODE;
	data.scale = DEFAULT_SCALE;

	static const struct option long_options[] = {
	{ "help",		no_argument,		NULL, 'h' },
	{ "alsa-device",	required_argument,	NULL, 'd' },
	{ "mode",		required_argument,	NULL, 'm' },
	{ "ramp-samples",	required_argument,	NULL, 's' },
	{ "ramp-time",		required_argument,	NULL, 't' },
	{ "ramp-step-samples",	required_argument,	NULL, 'a' },
	{ "ramp-step-time",	required_argument,	NULL, 'i' },
	{ "scale",		required_argument,	NULL, 'c' },
	{ NULL,	0, NULL, 0}
	};

	setlocale(LC_ALL, "");

	while ((c = getopt_long(argc, argv, "hdmstiac:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0], false);
			return 0;
		case 'm':
			if (!spa_streq (optarg, NATIVE) && !spa_streq (optarg, NON_NATIVE))
				printf("Invalid Mode(\"%s\"), using default(\"%s\")\n", optarg, DEFAULT_MODE);
			else
				data.mode = optarg;
			break;
		case 'c':
			if (!spa_streq (optarg, LINEAR) && !spa_streq (optarg, CUBIC))
				printf("Invalid Scale(\"%s\"), using default(\"%s\")\n", optarg,
						getscale(DEFAULT_SCALE));
			else
				if (spa_streq (optarg, LINEAR))
					data.scale = SPA_AUDIO_VOLUME_RAMP_LINEAR;
				else if (spa_streq (optarg, CUBIC))
					data.scale = SPA_AUDIO_VOLUME_RAMP_CUBIC;
			break;
		case 'd':
			data.alsa_device = optarg;
			break;
		case 's':
			data.volume_ramp_samples = atoi(optarg);
			break;
		case 't':
			data.volume_ramp_time = atoi(optarg);
			if (!data.volume_ramp_step_time)
				data.volume_ramp_step_time = DEFAULT_RAMP_STEP_TIME;
			data.volume_ramp_samples = 0;
			data.volume_ramp_step_samples = 0;
			break;
		case 'a':
			data.volume_ramp_step_samples = atoi(optarg);
			break;
		case 'i':
			data.volume_ramp_step_time = atoi(optarg);
			break;
		default:
			show_help(&data, argv[0], true);
			return -1;
		}
	}


	/* init data */
	if ((res = init_data(&data)) < 0) {
	  printf("can't init data: %d (%s)\n", res, spa_strerror(res));
	  return -1;
	}

	/* make the nodes (audiotestsrc and adapter with alsa-pcm-sink as follower) */
	if ((res = make_nodes(&data)) < 0) {
	  printf("can't make nodes: %d (%s)\n", res, spa_strerror(res));
		return -1;
	}

	/* Negotiate format */
	if ((res = negotiate_formats(&data)) < 0) {
		printf("can't negotiate nodes: %d (%s)\n", res, spa_strerror(res));
		return -1;
	}

	printf("using %s mode\n", data.mode);
	if (data.volume_ramp_samples && data.volume_ramp_step_samples)
		printf("using %d samples with a step size of %d samples to ramp volume at %s scale\n",
			data.volume_ramp_samples, data.volume_ramp_step_samples, getscale(data.scale));
	else if (data.volume_ramp_time && data.volume_ramp_step_time)
		printf("using %d msec with a step size of %d msec to ramp volume at %s scale\n",
			data.volume_ramp_time, (data.volume_ramp_step_time/1000), getscale(data.scale));

	spa_loop_control_enter(data.control);
	run_async_sink(&data);
	spa_loop_control_leave(data.control);
}
