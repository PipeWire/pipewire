/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/layout.h>

#include <pipewire/pipewire.h>

#include "dsffile.h"

struct data {
	struct pw_main_loop *loop;
	struct pw_stream *stream;

	const char *opt_filename;
	const char *opt_remote;

	struct dsf_file *f;
	struct dsf_file_info info;
	struct dsf_layout layout;
};

static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	ssize_t stride;
	uint32_t samples;
	uint8_t *d;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((d = buf->datas[0].data) == NULL)
		return;

	stride = data->info.channels * data->layout.interleave;

	samples = dsf_file_read(data->f, d, buf->datas[0].maxsize / stride,
			&data->layout);

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = samples * stride;

	pw_stream_queue_buffer(data->stream, b);
}

static void
on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
	struct data *data = userdata;
	struct spa_audio_info info = { 0 };
	int err;

	if (id != SPA_PARAM_Format || param == NULL)
		return;

	if ((err = spa_format_parse(param, &info.media_type, &info.media_subtype)) < 0)
		return;

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_dsd)
		return;

	if (spa_format_audio_dsd_parse(param, &info.info.dsd) < 0)
		return;

	data->layout.interleave = info.info.dsd.interleave,
	data->layout.channels = info.info.dsd.channels;
	data->layout.lsb = info.info.dsd.bitorder == SPA_PARAM_BITORDER_lsb;

	fprintf(stderr, "output:\n");
	fprintf(stderr, " bitorder: %s\n", data->layout.lsb ? "lsb" : "msb");
	fprintf(stderr, " channels: %u\n", data->layout.channels);
	fprintf(stderr, " interleave: %d\n", data->layout.interleave);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.param_changed = on_param_changed,
	.process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

struct layout_info {
	 uint32_t type;
	 struct spa_audio_layout_info info;
};

static const struct layout_info layouts[] = {
	{ 1, { SPA_AUDIO_LAYOUT_Mono, }, },
	{ 2, { SPA_AUDIO_LAYOUT_Stereo, }, },
	{ 3, { SPA_AUDIO_LAYOUT_2FC }, },
	{ 4, { SPA_AUDIO_LAYOUT_Quad }, },
	{ 5, { SPA_AUDIO_LAYOUT_3_1 }, },
	{ 6, { SPA_AUDIO_LAYOUT_5_0R }, },
	{ 7, { SPA_AUDIO_LAYOUT_5_1R }, },
};

int handle_dsd_playback(struct data *data)
{
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info_dsd info;
	size_t i;

	data->loop = pw_main_loop_new(NULL);

	pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGINT, do_quit, data);
	pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGTERM, do_quit, data);

	data->stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data->loop),
			"audio-src",
			pw_properties_new(
				PW_KEY_REMOTE_NAME, data->opt_remote,
				PW_KEY_MEDIA_TYPE, "Audio",
				PW_KEY_MEDIA_CATEGORY, "Playback",
				PW_KEY_MEDIA_ROLE, "Music",
				NULL),
			&stream_events,
			data);

	spa_zero(info);
	info.channels = data->info.channels;
	info.rate = data->info.rate / 8;

	for (i = 0; i < SPA_N_ELEMENTS(layouts); i++) {
		if (layouts[i].type != data->info.channel_type)
			continue;
		info.channels = layouts[i].info.n_channels;
		memcpy(info.position, layouts[i].info.position,
				info.channels * sizeof(uint32_t));
	}

	params[0] = spa_format_audio_dsd_build(&b, SPA_PARAM_EnumFormat, &info);

	/* Now connect this stream. We ask that our process function is
	 * called in a realtime thread. */
	pw_stream_connect(data->stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, 1);

	/* and wait while we let things run */
	pw_main_loop_run(data->loop);

	pw_stream_destroy(data->stream);
	pw_main_loop_destroy(data->loop);
	return 0;
}

static void show_help(const char *name)
{
        fprintf(stdout, "%s [options] FILE\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n",
		name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	int c;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ NULL,	0, NULL, 0}
	};

	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hVr:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0]);
			return 0;
		case 'V':
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			data.opt_remote = optarg;
			break;
		default:
			show_help(argv[0]);
			return -1;
		}
	}
	if (optind < argc)
		data.opt_filename = argv[optind];


	data.f = dsf_file_open(data.opt_filename, "r", &data.info);
	if (data.f == NULL) {
		fprintf(stderr, "can't open file %s: %m", data.opt_filename);
		return -1;
	}
	fprintf(stderr, "file details:\n");
	fprintf(stderr, " channel_type: %u\n", data.info.channel_type);
	fprintf(stderr, " channels: %u\n", data.info.channels);
	fprintf(stderr, " rate: %u\n", data.info.rate);
	fprintf(stderr, " lsb: %u\n", data.info.lsb);
	fprintf(stderr, " samples: %"PRIu64"\n", data.info.samples);
	fprintf(stderr, " length: %"PRIu64"\n", data.info.length);
	fprintf(stderr, " blocksize: %u\n", data.info.blocksize);

	handle_dsd_playback(&data);

	dsf_file_close(data.f);

	pw_deinit();

	return 0;
}
