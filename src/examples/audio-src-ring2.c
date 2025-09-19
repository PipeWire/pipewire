/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Audio source using \ref pw_stream "pw_stream" and ringbuffer.

 This one uses a thread-loop and does a blocking push into a
 ringbuffer.
 [title]
 */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>

#include <spa/param/audio/format-utils.h>
#include <spa/utils/ringbuffer.h>

#include <pipewire/pipewire.h>

#define M_PI_M2f (float)(M_PI+M_PI)

#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2
#define DEFAULT_VOLUME		0.7f

#define BUFFER_SIZE		(16*1024)

#define MIN_SIZE	256
#define MAX_SIZE	BUFFER_SIZE

static float samples[BUFFER_SIZE * DEFAULT_CHANNELS];

struct data {
	struct pw_thread_loop *thread_loop;
	struct pw_loop *loop;
	struct pw_stream *stream;
	int eventfd;
	bool running;

	float accumulator;

	struct spa_ringbuffer ring;
	float buffer[BUFFER_SIZE * DEFAULT_CHANNELS];
};

static void fill_f32(struct data *d, float *samples, int n_frames)
{
	float val;
	int i, c;

        for (i = 0; i < n_frames; i++) {
                d->accumulator += M_PI_M2f * 440 / DEFAULT_RATE;
                if (d->accumulator >= M_PI_M2f)
                        d->accumulator -= M_PI_M2f;

                val = sinf(d->accumulator) * DEFAULT_VOLUME;
                for (c = 0; c < DEFAULT_CHANNELS; c++)
			samples[i * DEFAULT_CHANNELS + c] = val;
        }
}

/* this can be called from any thread with a block of samples to write into
 * the ringbuffer. It will block until all data has been written */
static void push_samples(void *userdata, float *samples, uint32_t n_samples)
{
	struct data *data = userdata;
	int32_t filled;
	uint32_t index, avail, stride = sizeof(float) * DEFAULT_CHANNELS;
	uint64_t count;
	float *s = samples;

	while (n_samples > 0) {
		while (true) {
			filled = spa_ringbuffer_get_write_index(&data->ring, &index);
			/* we xrun, this can not happen because we never read more
			 * than what there is in the ringbuffer and we never write more than
			 * what is left */
			spa_assert(filled >= 0);
			spa_assert(filled <= BUFFER_SIZE);

			/* this is how much samples we can write */
			avail = BUFFER_SIZE - filled;
			if (avail > 0)
				break;

			/* no space.. block and wait for free space */
			spa_system_eventfd_read(data->loop->system, data->eventfd, &count);
			if (!data->running)
				return;
		}
		if (avail > n_samples)
			avail = n_samples;

		spa_ringbuffer_write_data(&data->ring,
				data->buffer, BUFFER_SIZE * stride,
				(index % BUFFER_SIZE) * stride,
				s, avail * stride);

		s += avail * DEFAULT_CHANNELS;
		n_samples -= avail;

		/* and advance the ringbuffer */
		spa_ringbuffer_write_update(&data->ring, index + avail);
	}

}

/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  b = pw_stream_dequeue_buffer(stream);
 *
 *  .. generate stuff in the buffer ...
 *  In this case we read samples from a ringbuffer. The ringbuffer is
 *  filled up by another thread.
 *
 *  pw_stream_queue_buffer(stream, b);
 */
static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *p;
	uint32_t index, to_read, to_silence;
	int32_t avail, n_frames, stride;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL)
		return;

	/* the amount of space in the ringbuffer and the read index */
	avail = spa_ringbuffer_get_read_index(&data->ring, &index);

	stride = sizeof(float) * DEFAULT_CHANNELS;
	n_frames = buf->datas[0].maxsize / stride;
	if (b->requested)
		n_frames = SPA_MIN((int32_t)b->requested, n_frames);

	/* we can read if there is something available */
	to_read = avail > 0 ? SPA_MIN(avail, n_frames) : 0;
	/* and fill the remainder with silence */
	to_silence = n_frames - to_read;

	if (to_read > 0) {
		/* read data into the buffer */
		spa_ringbuffer_read_data(&data->ring,
				data->buffer, BUFFER_SIZE * stride,
				(index % BUFFER_SIZE) * stride,
				p, to_read * stride);
		/* update the read pointer */
		spa_ringbuffer_read_update(&data->ring, index + to_read);
	}
	if (to_silence > 0)
		/* set the rest of the buffer to silence */
		memset(SPA_PTROFF(p, to_read * stride, void), 0, to_silence * stride);

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n_frames * stride;

	pw_stream_queue_buffer(data->stream, b);

	/* signal the main thread to fill the ringbuffer, we can only do this, for
	 * example when the available ringbuffer space falls below a certain
	 * level. */
	spa_system_eventfd_write(data->loop->system, data->eventfd, 1);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	data->running = false;
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[1];
	uint32_t n_params = 0;
	uint8_t buffer[1024];
	struct pw_properties *props;
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	pw_init(&argc, &argv);

	data.thread_loop = pw_thread_loop_new("audio-src", NULL);
	data.loop = pw_thread_loop_get_loop(data.thread_loop);
	data.running = true;

	pw_thread_loop_lock(data.thread_loop);
	pw_loop_add_signal(data.loop, SIGINT, do_quit, &data);
	pw_loop_add_signal(data.loop, SIGTERM, do_quit, &data);

	spa_ringbuffer_init(&data.ring);
	if ((data.eventfd = spa_system_eventfd_create(data.loop->system, SPA_FD_CLOEXEC)) < 0)
                return data.eventfd;

	pw_thread_loop_start(data.thread_loop);

	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Music",
			NULL);
	if (argc > 1)
		/* Set stream target if given on command line */
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, argv[1]);

	data.stream = pw_stream_new_simple(
			data.loop,
			"audio-src-ring",
			props,
			&stream_events,
			&data);

	/* Make one parameter with the supported formats. The SPA_PARAM_EnumFormat
	 * id means that this is a format enumeration (of 1 value). */
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&SPA_AUDIO_INFO_RAW_INIT(
				.format = SPA_AUDIO_FORMAT_F32,
				.channels = DEFAULT_CHANNELS,
				.rate = DEFAULT_RATE ));

	/* Now connect this stream. We ask that our process function is
	 * called in a realtime thread. */
	pw_stream_connect(data.stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS |
			  PW_STREAM_FLAG_RT_PROCESS,
			  params, n_params);

	/* prefill the ringbuffer */
	fill_f32(&data, samples, BUFFER_SIZE);
	push_samples(&data, samples, BUFFER_SIZE);

	srand(time(NULL));

	pw_thread_loop_start(data.thread_loop);
	pw_thread_loop_unlock(data.thread_loop);

	while (data.running) {
		uint32_t size = rand() % ((MAX_SIZE - MIN_SIZE + 1) + MIN_SIZE);
		/* make new random sized block of samples and push */
		fill_f32(&data, samples, size);
		push_samples(&data, samples, size);
	}

	pw_thread_loop_lock(data.thread_loop);
	pw_stream_destroy(data.stream);
	pw_thread_loop_unlock(data.thread_loop);
	pw_thread_loop_destroy(data.thread_loop);
	close(data.eventfd);
	pw_deinit();

	return 0;
}
