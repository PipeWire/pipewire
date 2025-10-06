/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Audio sink using \ref pw_filter "pw_filter"
 [title]
 */

#include "config.h"

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

/* define to make this filter allocate buffer memory */
#define ALLOC_BUFFERS

struct data;

struct port {
	struct data *data;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_filter *filter;
	struct port *in_port;
	bool move;
	uint32_t quantum_limit;
};

/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  out = pw_filter_dequeue_buffer(filter, in_port);
 *
 *  .. consume data in the buffer ...
 *
 *  pw_filter_queue_buffer(filter, in_port, out);
 *
 *  For DSP ports, there is a shortcut to directly dequeue, get
 *  the data and requeue the buffer with pw_filter_get_dsp_buffer().
 */
static void on_process(void *userdata, struct spa_io_position *position)
{
	struct data *data = userdata;
	float *in, max;
	struct port *in_port = data->in_port;
	uint32_t i, n_samples = position->clock.duration, peak;

	pw_log_trace("do process %d", n_samples);

	in = pw_filter_get_dsp_buffer(in_port, n_samples);
	if (in == NULL)
		return;

	/* move cursor up */
	if (data->move)
		fprintf(stdout, "%c[%dA", 0x1b, 2);
	fprintf(stdout, "captured %d samples\n", n_samples);
	max = 0.0f;
	for (i = 0; i < n_samples; i++)
		max = fmaxf(max, fabsf(in[i]));

	peak = (uint32_t)SPA_CLAMPF(max * 30, 0.f, 39.f);

	fprintf(stdout, "input: |%*s%*s| peak:%f\n", peak+1, "*", 40 - peak, "", max);
	data->move = true;
	fflush(stdout);
}

#ifdef ALLOC_BUFFERS
/* close the memfd we set on the buffers here */
static void on_remove_buffer(void *_data, void *_port_data, struct pw_buffer *buffer)
{
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;

	d = buf->datas;
	pw_log_info("remove buffer %p", buffer);

	close(d[0].fd);
}

/* we set the PW_STREAM_FLAG_ALLOC_BUFFERS flag when connecting so we need
 * to provide buffer memory.  */
static void on_add_buffer(void *_data, void *_port_data, struct pw_buffer *buffer)
{
	struct data *data = _data;
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;

	pw_log_info("add buffer %p", buffer);
	d = buf->datas;

	if ((d[0].type & (1<<SPA_DATA_MemFd)) == 0) {
		pw_log_error("unsupported data type %08x", d[0].type);
		return;
	}

	/* create the memfd on the buffer, set the type and flags */
	d[0].type = SPA_DATA_MemFd;
	d[0].flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
#ifdef HAVE_MEMFD_CREATE
	d[0].fd = memfd_create("audio-dsp-sink-memfd", MFD_CLOEXEC);
#else
	d[0].fd = -1;
#endif
	if (d[0].fd == -1) {
		pw_log_error("can't create memfd: %m");
		return;
	}
	d[0].mapoffset = 0;
	d[0].maxsize = data->quantum_limit * sizeof(float);

	/* truncate to the right size */
	if (ftruncate(d[0].fd, d[0].maxsize) < 0) {
		pw_log_error("can't truncate to %d: %m", d[0].maxsize);
		return;
	}
}
#endif

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
#ifdef ALLOC_BUFFERS
	.add_buffer = on_add_buffer,
	.remove_buffer = on_remove_buffer,
#endif
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	uint32_t flags;

	pw_init(&argc, &argv);

	data.quantum_limit= 8192;

	/* make a main loop. If you already have another main loop, you can add
	 * the fd of this pipewire mainloop to it. */
	data.loop = pw_main_loop_new(NULL);

	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	/* Create a simple filter, the simple filter manages the core and remote
	 * objects for you if you don't need to deal with them.
	 *
	 * Pass your events and a user_data pointer as the last arguments. This
	 * will inform you about the filter state. The most important event
	 * you need to listen to is the process event where you need to process
	 * the data.
	 */
	data.filter = pw_filter_new_simple(
			pw_main_loop_get_loop(data.loop),
			"audio-dsp-sink",
			pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Audio",
				PW_KEY_MEDIA_CATEGORY, "Sink",
				PW_KEY_MEDIA_ROLE, "DSP",
				PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
				PW_KEY_NODE_AUTOCONNECT, "true",
				NULL),
			&filter_events,
			&data);

	flags = PW_FILTER_PORT_FLAG_MAP_BUFFERS;
#ifdef ALLOC_BUFFERS
	flags |= PW_FILTER_PORT_FLAG_ALLOC_BUFFERS;
#endif

	/* make an audio DSP output port */
	data.in_port = pw_filter_add_port(data.filter,
			PW_DIRECTION_INPUT,
			flags,
			sizeof(struct port),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, "input",
				NULL),
			NULL, 0);

	/* Now connect this filter. We ask that our process function is
	 * called in a realtime thread. */
	if (pw_filter_connect(data.filter,
				PW_FILTER_FLAG_RT_PROCESS,
				NULL, 0) < 0) {
		fprintf(stderr, "can't connect\n");
		return -1;
	}

	/* and wait while we let things run */
	pw_main_loop_run(data.loop);

	pw_filter_destroy(data.filter);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return 0;
}
