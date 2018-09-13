/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/mman.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <pipewire/pipewire.h>

#define M_PI_M2 ( M_PI + M_PI )

#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2
#define DEFAULT_VOLUME		0.7

struct data {
	struct pw_main_loop *loop;

	struct pw_core *core;
	struct pw_remote *remote;

	struct pw_stream *stream;

	double accumulator;
};

static void fill_f32(struct data *d, void *dest, int avail)
{
	float *dst = dest, val;
	int n_samples = avail / (sizeof(float) * DEFAULT_CHANNELS);
	int i, c;

        for (i = 0; i < n_samples; i++) {
                d->accumulator += M_PI_M2 * 440 / DEFAULT_RATE;
                if (d->accumulator >= M_PI_M2)
                        d->accumulator -= M_PI_M2;

                val = sin(d->accumulator) * DEFAULT_VOLUME;
                for (c = 0; c < DEFAULT_CHANNELS; c++)
                        *dst++ = val;
        }
}

static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *p;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL)
		return;

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL)
		return;

	fill_f32(data, p, buf->datas[0].maxsize);

	buf->datas[0].chunk->size = buf->datas[0].maxsize;

	pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);

	data.stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data.loop),
			"audio-src",
			pw_properties_new(
				PW_NODE_PROP_MEDIA, "Audio",
				PW_NODE_PROP_CATEGORY, "Playback",
				PW_NODE_PROP_ROLE, "Music",
				NULL),
			&stream_events,
			&data);

	data.remote = pw_stream_get_remote(data.stream);

	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&SPA_AUDIO_INFO_RAW_INIT(
				.format = SPA_AUDIO_FORMAT_F32,
				.channels = DEFAULT_CHANNELS,
				.rate = DEFAULT_RATE ));

	pw_stream_connect(data.stream,
			  PW_DIRECTION_OUTPUT,
			  argc > 1 ? argv[1] : NULL,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS |
			  PW_STREAM_FLAG_RT_PROCESS,
			  params, 1);

	pw_main_loop_run(data.loop);

	pw_stream_destroy(data.stream);
	pw_main_loop_destroy(data.loop);

	return 0;
}
