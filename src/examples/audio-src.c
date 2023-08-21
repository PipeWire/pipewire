/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Audio source using \ref pw_stream "pw_stream".
 [title]
 */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>

#include <spa/param/audio/format-utils.h>

#include <pipewire/pipewire.h>

#define M_PI_M2 ( M_PI + M_PI )

#define DEFAULT_QUANTUM		1764
#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2
#define DEFAULT_VOLUME		0.7

struct data {
	struct pw_main_loop *loop;
	struct pw_stream *stream;

	double accumulator;
	uint64_t outputSample;
};

static void fill_f32(struct data *d, void *dest, int n_frames)
{
	float *dst = dest, val;
	int i, c;

        for (i = 0; i < n_frames; i++) {
                d->accumulator += M_PI_M2 * 440 / DEFAULT_RATE;
                if (d->accumulator >= M_PI_M2)
                        d->accumulator -= M_PI_M2;

                val = sin(d->accumulator) * DEFAULT_VOLUME;
                for (c = 0; c < DEFAULT_CHANNELS; c++)
                        *dst++ = val;
        }
}

/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  b = pw_stream_dequeue_buffer(stream);
 *
 *  .. generate stuff in the buffer ...
 *
 *  pw_stream_queue_buffer(stream, b);
 */
static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int n_frames, stride;
	uint8_t *p;

	struct pw_time streamTime;
	uint64_t driverSample;
	int64_t queued;

	pw_stream_get_time_n(data->stream, &streamTime, sizeof(streamTime));

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	/* calculate the current sample in the driver's timeline */
	driverSample = streamTime.ticks * DEFAULT_RATE * streamTime.rate.num / streamTime.rate.denom;

	/* find how many samples we have queued but have not yet appeared in the driver's timeline;
	   these are queued in the audioadapter but pw_stream doesn't report them accurately */
	queued = data->outputSample - driverSample;
	if (queued < 0) {
		/* XRun, resync our timeline */
		pw_log_info("XRun! resync, data->outputSample += %"PRIi64, -queued);
		data->outputSample += -queued;
		queued = 0;
	}

	pw_log_info("process, b.req %"PRIu64", s.ticks %"PRIu64 ", sample(our) %"PRIu64", sample(dr) %"PRIu64",\n"
		    "         s.buf %"PRIu64 ", s.queued %"PRIu64 ", queued (real) %"PRIi64 ", s.delay %"PRIu64,
		    b->requested, streamTime.ticks, data->outputSample, driverSample,
		    streamTime.buffered, streamTime.queued, queued, streamTime.delay);

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL)
		return;

	stride = sizeof(float) * DEFAULT_CHANNELS;
	n_frames = SPA_MIN((uint64_t) DEFAULT_QUANTUM, buf->datas[0].maxsize / stride);

	fill_f32(data, p, n_frames);

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n_frames * stride;

	b->size = n_frames;
	data->outputSample += n_frames;

	pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct pw_properties *props;
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	pw_init(&argc, &argv);

	/* make a main loop. If you already have another main loop, you can add
	 * the fd of this pipewire mainloop to it. */
	data.loop = pw_main_loop_new(NULL);

	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	/* Create a simple stream, the simple stream manages the core and remote
	 * objects for you if you don't need to deal with them.
	 *
	 * If you plan to autoconnect your stream, you need to provide at least
	 * media, category and role properties.
	 *
	 * Pass your events and a user_data pointer as the last arguments. This
	 * will inform you about the stream state. The most important event
	 * you need to listen to is the process event where you need to produce
	 * the data.
	 */
	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Music",
			PW_KEY_NODE_LATENCY, SPA_STRINGIFY (DEFAULT_QUANTUM) "/" SPA_STRINGIFY (DEFAULT_RATE),
			NULL);
	if (argc > 1)
		/* Set stream target if given on command line */
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, argv[1]);
	data.stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data.loop),
			"audio-src",
			props,
			&stream_events,
			&data);

	/* Make one parameter with the supported formats. The SPA_PARAM_EnumFormat
	 * id means that this is a format enumeration (of 1 value). */
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&SPA_AUDIO_INFO_RAW_INIT(
				.format = SPA_AUDIO_FORMAT_F32,
				.channels = DEFAULT_CHANNELS,
				.rate = DEFAULT_RATE ));
	params[1] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(4),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int(DEFAULT_QUANTUM * sizeof(float) * DEFAULT_CHANNELS),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(sizeof(float) * DEFAULT_CHANNELS),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemPtr)));

	/* Now connect this stream. We ask that our process function is
	 * called in a realtime thread. */
	pw_stream_connect(data.stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, 2);

	/* and wait while we let things run */
	pw_main_loop_run(data.loop);

	pw_stream_destroy(data.stream);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return 0;
}
