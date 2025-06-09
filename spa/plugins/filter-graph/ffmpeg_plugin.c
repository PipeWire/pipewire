/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <math.h>
#include <limits.h>

#include <spa/utils/result.h>
#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/support/log.h>

#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

#include "audio-plugin.h"

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
};

struct instance {
	struct descriptor *desc;

	AVFilterGraph *filter_graph;
	AVFilterInOut *in;
	AVFilterInOut *out;

	uint32_t rate;

	AVFilterContext *ctx[128];
	uint32_t n_ctx;

	float *data[128];
	AVFrame *frame;
};

static void *ffmpeg_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor *desc,
                        unsigned long SampleRate, int index, const char *config)
{
	struct descriptor *d = (struct descriptor *)desc;
	struct plugin *p = d->p;
	struct instance *i;
	AVFilterInOut *fp;
	AVFilterContext *cnv, *ctx;
	int res;
	char options_str[1024];

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

	res = avfilter_graph_parse2(i->filter_graph, d->desc.name, &i->in, &i->out);
	if (res < 0) {
		spa_log_error(p->log, "can parse filter graph %s", d->desc.name);
		errno = EINVAL;
		return NULL;
	}

	for (fp = i->in; fp != NULL; fp = fp->next) {
		ctx = avfilter_graph_alloc_filter(i->filter_graph, d->buffersrc, "src");
		if (ctx == NULL) {
			spa_log_error(p->log, "can't alloc buffersrc");
			return NULL;
		}
		snprintf(options_str, sizeof(options_str),
	             "sample_fmt=%s:sample_rate=%ld:channel_layout=mono",
		             av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP), SampleRate);
		avfilter_init_str(ctx, options_str);
		avfilter_link(ctx, 0, fp->filter_ctx, fp->pad_idx);
		i->ctx[i->n_ctx++] = ctx;
	}
	for (fp = i->out; fp != NULL; fp = fp->next) {

		cnv = avfilter_graph_alloc_filter(i->filter_graph, d->format, "format");
		if (cnv == NULL) {
			spa_log_error(p->log, "can't alloc format");
			return NULL;
		}

		snprintf(options_str, sizeof(options_str),
	             "sample_fmts=%s:sample_rates=%ld:channel_layouts=mono",
		             av_get_sample_fmt_name(AV_SAMPLE_FMT_FLTP), SampleRate);

		avfilter_init_str(cnv, options_str);
		avfilter_link(fp->filter_ctx, fp->pad_idx, cnv, 0);

		ctx = avfilter_graph_alloc_filter(i->filter_graph, d->buffersink, "sink");
		if (ctx == NULL) {
			spa_log_error(p->log, "can't alloc buffersink");
			return NULL;
		}
		avfilter_init_str(ctx, NULL);
		avfilter_link(cnv, 0, ctx, 0);
		i->ctx[i->n_ctx++] = ctx;
	}
	avfilter_graph_config(i->filter_graph, NULL);

	i->frame = av_frame_alloc();

#if 1
	char *dump = avfilter_graph_dump(i->filter_graph, NULL);
	fprintf(stderr, "%s\n", dump);
	free(dump);
#endif
	return i;
}

static void ffmpeg_cleanup(void *instance)
{
	struct instance *i = instance;
	free(i);
}

static void ffmpeg_free(const struct spa_fga_descriptor *desc)
{
	struct descriptor *d = (struct descriptor*)desc;
	avfilter_graph_free(&d->filter_graph);
	free(d);
}

static void ffmpeg_connect_port(void *instance, unsigned long port, float *data)
{
	struct instance *i = instance;
	i->data[port] = data;
}

static void ffmpeg_run(void *instance, unsigned long SampleCount)
{
	struct instance *i = instance;
	char buf[1024];
	int err;

	spa_log_debug(i->desc->p->log, "run %ld", SampleCount);
	i->frame->nb_samples = SampleCount;
	i->frame->sample_rate = i->rate;
	i->frame->format = AV_SAMPLE_FMT_FLTP;
	av_channel_layout_copy(&i->frame->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);

	i->frame->data[0] = (uint8_t*)i->data[0];

	if ((err = av_buffersrc_add_frame_flags(i->ctx[0], i->frame,
			AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT)) < 0) {
		av_strerror(err, buf, sizeof(buf));
		spa_log_error(i->desc->p->log, "can't add frame %s", buf);
		av_frame_unref(i->frame);
		return;
	}
	if ((err = av_buffersink_get_samples(i->ctx[1], i->frame, SampleCount)) >= 0) {
		spa_log_trace(i->desc->p->log, "got frame %d %d %d %s",
				i->frame->nb_samples,
				i->frame->ch_layout.nb_channels,
				i->frame->sample_rate,
				av_get_sample_fmt_name(i->frame->format));

		memcpy(i->data[1], i->frame->data[0], SampleCount * sizeof(float));

		av_frame_unref(i->frame);
	}
}

static const struct spa_fga_descriptor *ffmpeg_plugin_make_desc(void *plugin, const char *name)
{
	struct plugin *p = (struct plugin *)plugin;
	struct descriptor *desc;
	uint32_t i;
	AVFilterInOut *in = NULL, *out = NULL, *fp;
	int res;

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
	for (fp = in; fp != NULL; fp = fp->next) {
		spa_log_info(p->log, "%p: in %s %p:%d", fp, fp->name, fp->filter_ctx, fp->pad_idx);
		desc->desc.n_ports++;
	}
	for (fp = out; fp != NULL; fp = fp->next) {
		spa_log_info(p->log, "%p: out %s %p:%d", fp, fp->name, fp->filter_ctx, fp->pad_idx);
		desc->desc.n_ports++;
	}


	desc->desc.instantiate = ffmpeg_instantiate;
	desc->desc.cleanup = ffmpeg_cleanup;
	desc->desc.free = ffmpeg_free;
	desc->desc.connect_port = ffmpeg_connect_port;
	desc->desc.run = ffmpeg_run;

	desc->desc.name = strdup(name);
	desc->desc.flags = 0;

	desc->desc.ports = calloc(desc->desc.n_ports, sizeof(struct spa_fga_port));

	for (i = 0, fp = in; fp != NULL; i++, fp = fp->next) {
		desc->desc.ports[i].index = i;
		desc->desc.ports[i].name = fp->name;
		desc->desc.ports[i].flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO;
	}
	for (fp = out; fp != NULL; i++, fp = fp->next) {
		desc->desc.ports[i].index = i;
		desc->desc.ports[i].name = fp->name;
		desc->desc.ports[i].flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO;
	}
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
	struct plugin *impl = (struct plugin *)handle;
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
	int res;
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
	if (path == NULL)
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
