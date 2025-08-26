/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <math.h>
#include <limits.h>
#include <string.h>

#include <spa/utils/result.h>
#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/support/log.h>

#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

#include "audio-plugin.h"

#define MAX_PORTS	256
#define MAX_CTX		64

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_log *log;
};

struct descriptor {
	struct spa_fga_descriptor desc;
	struct plugin *p;

	AVFilterGraph *filter_graph;
	const AVFilter *format;
	const AVFilter *buffersrc;
	const AVFilter *buffersink;

	AVChannelLayout layout[MAX_CTX];
	uint32_t latency_idx;
};

struct instance {
	struct descriptor *desc;

	AVFilterGraph *filter_graph;

	uint32_t rate;

	AVFrame *frame;
	AVFilterContext *ctx[MAX_CTX];
	uint32_t n_ctx;
	uint32_t n_src;
	uint32_t n_sink;

	uint64_t frame_num;
	float *data[MAX_PORTS];
};

static void layout_from_name(AVChannelLayout *layout, const char *name)
{
	const char *chan;

	if ((chan = strrchr(name, '_')) != NULL)
		chan++;
	else
		chan = "FC";

	if (av_channel_layout_from_string(layout, chan) < 0)
		av_channel_layout_from_string(layout, "FC");
}

static void *ffmpeg_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor *desc,
                        unsigned long SampleRate, int index, const char *config)
{
	struct descriptor *d = (struct descriptor *)desc;
	struct plugin *p = d->p;
	struct instance *i;
	AVFilterInOut *fp, *in, *out;
	AVFilterContext *cnv, *ctx;
	int res;
	char channel[512];
	char options_str[1024];
	uint32_t n_fp;

	i = calloc(1, sizeof(*i));
	if (i == NULL)
		return NULL;

	i->desc = d;
	i->rate = SampleRate;

	i->filter_graph = avfilter_graph_alloc();
	if (i->filter_graph == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	res = avfilter_graph_parse2(i->filter_graph, d->desc.name, &in, &out);
	if (res < 0) {
		spa_log_error(p->log, "can parse filter graph %s", d->desc.name);
		errno = EINVAL;
		return NULL;
	}

	for (n_fp = 0, fp = in; fp != NULL; fp = fp->next, n_fp++) {
		ctx = avfilter_graph_alloc_filter(i->filter_graph, d->buffersrc, "src");
		if (ctx == NULL) {
			spa_log_error(p->log, "can't alloc buffersrc");
			return NULL;
		}
		av_channel_layout_describe(&d->layout[n_fp], channel, sizeof(channel));

		snprintf(options_str, sizeof(options_str),
	             "sample_fmt=%s:sample_rate=%ld:channel_layout=%s",
		             av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP), SampleRate, channel);

		spa_log_info(p->log, "%d buffersrc %s", n_fp, options_str);
		avfilter_init_str(ctx, options_str);
		avfilter_link(ctx, 0, fp->filter_ctx, fp->pad_idx);

		i->ctx[n_fp] = ctx;
	}
	i->n_src = n_fp;
	for (fp = out; fp != NULL; fp = fp->next, n_fp++) {
		cnv = avfilter_graph_alloc_filter(i->filter_graph, d->format, "format");
		if (cnv == NULL) {
			spa_log_error(p->log, "can't alloc format");
			return NULL;
		}

		av_channel_layout_describe(&d->layout[n_fp], channel, sizeof(channel));

		snprintf(options_str, sizeof(options_str),
	             "sample_fmts=%s:sample_rates=%ld:channel_layouts=%s",
		             av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP), SampleRate, channel);

		spa_log_info(p->log, "%d format %s", n_fp, options_str);
		avfilter_init_str(cnv, options_str);
		avfilter_link(fp->filter_ctx, fp->pad_idx, cnv, 0);

		ctx = avfilter_graph_alloc_filter(i->filter_graph, d->buffersink, "sink");
		if (ctx == NULL) {
			spa_log_error(p->log, "can't alloc buffersink");
			return NULL;
		}
		avfilter_init_str(ctx, NULL);
		avfilter_link(cnv, 0, ctx, 0);
		i->ctx[n_fp] = ctx;
	}
	i->n_sink = n_fp;

	avfilter_graph_config(i->filter_graph, NULL);

	i->frame = av_frame_alloc();

#if 0
	char *dump = avfilter_graph_dump(i->filter_graph, NULL);
	spa_log_debug(p->log, "%s", dump);
	free(dump);
#endif
	return i;
}

static void ffmpeg_cleanup(void *instance)
{
	struct instance *i = instance;
	avfilter_graph_free(&i->filter_graph);
	av_frame_free(&i->frame);
	free(i);
}

static void ffmpeg_free(const struct spa_fga_descriptor *desc)
{
	struct descriptor *d = (struct descriptor*)desc;
	uint32_t i;
	avfilter_graph_free(&d->filter_graph);
	for (i = 0; i <  d->desc.n_ports; i++)
		free((void*)d->desc.ports[i].name);
	free((char*)d->desc.name);
	free(d->desc.ports);
	free(d);
}

static void ffmpeg_connect_port(void *instance, unsigned long port, void *data)
{
	struct instance *i = instance;
	i->data[port] = data;
}

static void ffmpeg_run(void *instance, unsigned long SampleCount)
{
	struct instance *i = instance;
	struct descriptor *desc = i->desc;
	char buf[1024];
	int err, j;
	uint32_t c, d = 0;
	float delay;

	spa_log_trace(i->desc->p->log, "run %ld", SampleCount);

	for (c = 0; c < i->n_src; c++) {
		i->frame->nb_samples = SampleCount;
		i->frame->sample_rate = i->rate;
		i->frame->format = AV_SAMPLE_FMT_FLTP;
		i->frame->pts = i->frame_num;

		av_channel_layout_copy(&i->frame->ch_layout, &desc->layout[c]);

		for (j = 0; j < desc->layout[c].nb_channels; j++)
			i->frame->data[j] = (uint8_t*)i->data[d++];

		if ((err = av_buffersrc_add_frame_flags(i->ctx[c], i->frame,
				AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT)) < 0) {
			av_strerror(err, buf, sizeof(buf));
			spa_log_warn(i->desc->p->log, "can't add frame: %s", buf);
			av_frame_unref(i->frame);
			continue;
		}
	}
	delay = 0.0f;
	for (; c < i->n_sink; c++) {
		if ((err = av_buffersink_get_samples(i->ctx[c], i->frame, SampleCount)) < 0) {
			av_strerror(err, buf, sizeof(buf));
			spa_log_debug(i->desc->p->log, "can't get frame: %s", buf);
			for (j = 0; j < desc->layout[c].nb_channels; j++)
				memset(i->data[d++], 0, SampleCount * sizeof(float));
			continue;
		}
		delay = fmaxf(delay, i->frame_num - i->frame->pts);

		spa_log_trace(i->desc->p->log, "got frame %d %d %d %s %f",
				i->frame->nb_samples,
				i->frame->ch_layout.nb_channels,
				i->frame->sample_rate,
				av_get_sample_fmt_name(i->frame->format),
				delay);

		for (j = 0; j < desc->layout[c].nb_channels; j++)
			memcpy(i->data[d++], i->frame->data[j], SampleCount * sizeof(float));

		av_frame_unref(i->frame);
	}
	i->frame_num += SampleCount;
	if (i->data[desc->latency_idx] != NULL)
		 i->data[desc->latency_idx][0] = delay;
}

static const struct spa_fga_descriptor *ffmpeg_plugin_make_desc(void *plugin, const char *name)
{
	struct plugin *p = (struct plugin *)plugin;
	struct descriptor *desc;
	uint32_t n_fp, n_p;
	AVFilterInOut *in = NULL, *out = NULL, *fp;
	int res, j;

	spa_log_info(p->log, "%s", name);

	desc = calloc(1, sizeof(*desc));
	if (desc == NULL)
		return NULL;

	desc->p = p;
	desc->filter_graph = avfilter_graph_alloc();
	if (desc->filter_graph == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	res = avfilter_graph_parse2(desc->filter_graph, name, &in, &out);
	if (res < 0) {
		spa_log_error(p->log, "can parse filter graph %s", name);
		errno = EINVAL;
		return NULL;
	}

	desc->desc.n_ports = 0;
	for (n_fp = 0, fp = in; fp != NULL && n_fp < MAX_CTX; fp = fp->next, n_fp++) {
		layout_from_name(&desc->layout[n_fp], fp->name);
		spa_log_info(p->log, "%p: in %s %p:%d channels:%d", fp, fp->name,
				fp->filter_ctx, fp->pad_idx, desc->layout[n_fp].nb_channels);
		desc->desc.n_ports += desc->layout[n_fp].nb_channels;
	}
	for (fp = out; fp != NULL && n_fp < MAX_CTX; fp = fp->next, n_fp++) {
		layout_from_name(&desc->layout[n_fp], fp->name);
		spa_log_info(p->log, "%p: out %s %p:%d channels:%d", fp, fp->name,
				fp->filter_ctx, fp->pad_idx, desc->layout[n_fp].nb_channels);
		desc->desc.n_ports += desc->layout[n_fp].nb_channels;
	}
	/* one for the latency */
	desc->desc.n_ports++;

	if (n_fp >= MAX_CTX) {
		spa_log_error(p->log, "%p: too many in/out ports %d > %d", desc,
				n_fp, MAX_CTX);
		errno = ENOSPC;
		return NULL;
	}
	if (desc->desc.n_ports >= MAX_PORTS) {
		spa_log_error(p->log, "%p: too many ports %d > %d", desc,
				desc->desc.n_ports, MAX_PORTS);
		errno = ENOSPC;
		return NULL;
	}

	desc->desc.instantiate = ffmpeg_instantiate;
	desc->desc.cleanup = ffmpeg_cleanup;
	desc->desc.free = ffmpeg_free;
	desc->desc.connect_port = ffmpeg_connect_port;
	desc->desc.run = ffmpeg_run;

	desc->desc.name = strdup(name);
	desc->desc.flags = 0;

	desc->desc.ports = calloc(desc->desc.n_ports, sizeof(struct spa_fga_port));

	for (n_fp = 0, n_p = 0, fp = in; fp != NULL; fp = fp->next, n_fp++) {
		for (j = 0; j < desc->layout[n_fp].nb_channels; j++, n_p++) {
			desc->desc.ports[n_p].index = n_p;
			if (desc->layout[n_fp].nb_channels == 1)
				desc->desc.ports[n_p].name = spa_aprintf("%s", fp->name);
			else
				desc->desc.ports[n_p].name = spa_aprintf("%s_%d", fp->name, j);
			desc->desc.ports[n_p].flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO;
		}
	}
	for (fp = out; fp != NULL; fp = fp->next, n_fp++) {
		for (j = 0; j < desc->layout[n_fp].nb_channels; j++, n_p++) {
			desc->desc.ports[n_p].index = n_p;
			if (desc->layout[n_fp].nb_channels == 1)
				desc->desc.ports[n_p].name = spa_aprintf("%s", fp->name);
			else
				desc->desc.ports[n_p].name = spa_aprintf("%s_%d", fp->name, j);
			desc->desc.ports[n_p].flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO;
		}
	}
	desc->desc.ports[n_p].index = n_p;
	desc->desc.ports[n_p].name = strdup("latency");
	desc->desc.ports[n_p].flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL;
	desc->desc.ports[n_p].hint = SPA_FGA_HINT_LATENCY;
	desc->latency_idx = n_p++;

	desc->buffersrc = avfilter_get_by_name("abuffer");
	desc->buffersink = avfilter_get_by_name("abuffersink");
	desc->format = avfilter_get_by_name("aformat");

	return &desc->desc;
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = ffmpeg_plugin_make_desc,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct plugin *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct plugin *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin))
		*interface = &impl->plugin;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct plugin);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct plugin *impl;
	uint32_t i;
	const char *path = NULL;

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct plugin *) handle;

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "filter.graph.path"))
			path = s;
	}
	if (!spa_streq(path, "filtergraph"))
		return -EINVAL;

	impl->plugin.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,
			SPA_VERSION_FGA_PLUGIN,
			&impl_plugin, impl);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin },
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

static struct spa_handle_factory spa_fga_plugin_ffmpeg_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.ffmpeg",
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
		*factory = &spa_fga_plugin_ffmpeg_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
