/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 MIDI source using \ref pw_filter "pw_filter".
 [title]
 */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <getopt.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <spa/pod/builder.h>
#include <spa/control/control.h>


#define PERIOD_NSEC	(SPA_NSEC_PER_SEC/8)

struct port {
};

struct data {
	struct pw_main_loop *loop;
	struct pw_filter *filter;
	struct port *port;
	uint32_t clock_id;
	int64_t offset;
	uint64_t position;
};

static void on_process(void *userdata, struct spa_io_position *position)
{
	struct data *data = userdata;
	struct port *port = data->port;
	struct pw_buffer *buf;
	struct spa_data *d;
	struct spa_pod_builder builder;
	struct spa_pod_frame frame;
	uint64_t sample_offset, sample_period, sample_position, cycle;

	/*
	 * Use the clock sample position.
	 *
	 * If the playback switches to using a different clock, we reset
	 * playback as the sample position can then be discontinuous.
	 */
	if (data->clock_id != position->clock.id) {
		pw_log_info("switch to clock %u", position->clock.id);
		data->offset = position->clock.position - data->position;
		data->clock_id = position->clock.id;
	}

	sample_position = position->clock.position - data->offset;
	data->position = sample_position + position->clock.duration;

	/*
	 * Produce note on/off every `PERIOD_NSEC` nanoseconds (rounded down to
	 * samples, for simplicity).
	 *
	 * We want to place the notes on the playback timeline, so we use sample
	 * positions (not real time!).
	 */

	sample_period = PERIOD_NSEC * position->clock.rate.denom
		/ position->clock.rate.num / SPA_NSEC_PER_SEC;

	cycle = sample_position / sample_period;
	if (sample_position % sample_period != 0)
		++cycle;

	sample_offset = cycle*sample_period - sample_position;

	if (sample_offset >= position->clock.duration)
		return;  /* don't need to produce anything yet */

	/* Get output buffer */
	if ((buf = pw_filter_dequeue_buffer(port)) == NULL)
		return;

	/* Midi buffers always have exactly one data block */
	spa_assert(buf->buffer->n_datas == 1);

	d = &buf->buffer->datas[0];
	d->chunk->offset = 0;
	d->chunk->size = 0;
	d->chunk->stride = 1;
	d->chunk->flags = 0;

	/*
	 * MIDI buffers contain a SPA POD with a sequence of
	 * control messages and their raw MIDI data.
	 */
	spa_pod_builder_init(&builder, d->data, d->maxsize);
	spa_pod_builder_push_sequence(&builder, &frame, 0);

	while (sample_offset < position->clock.duration) {
		if (cycle % 2 == 0) {
			/* MIDI note on, channel 0, middle C, max velocity */
			uint32_t event = 0x20903c7f;

			/* The time position of the message in the graph cycle
			 * is given as offset from the cycle start, in
			 * samples. The cycle has duration of `clock.duration`
			 * samples, and the sample offset should satisfy
			 * 0 <= sample_offset < position->clock.duration.
			 */
			spa_pod_builder_control(&builder, sample_offset, SPA_CONTROL_UMP);

			/* Raw MIDI data for the message */
			spa_pod_builder_bytes(&builder, &event, sizeof(event));

			pw_log_info("note on at %"PRIu64, sample_position + sample_offset);
		} else {
			/* MIDI note off, channel 0, middle C, max velocity */
			uint32_t event = 0x20803c7f;

			spa_pod_builder_control(&builder, sample_offset, SPA_CONTROL_UMP);
			spa_pod_builder_bytes(&builder, &event, sizeof(event));

			pw_log_info("note off at %"PRIu64, sample_position + sample_offset);
		}

		sample_offset += sample_period;
		++cycle;
	}

	/*
	 * Finish the sequence and queue buffer to output.
	 */
        spa_pod_builder_pop(&builder, &frame);
        d->chunk->size = builder.state.offset;

	pw_log_trace("produced %u/%u bytes", d->chunk->size, d->maxsize);

	pw_filter_queue_buffer(port, buf);
}

static void state_changed(void *userdata, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct data *data = userdata;

	switch (state) {
	case PW_FILTER_STATE_STREAMING:
		/* reset playback position */
		pw_log_info("start playback");
		data->clock_id = SPA_ID_INVALID;
		data->offset = 0;
		data->position = 0;
		break;
	default:
		break;
	}
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
	.state_changed = state_changed,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
	struct data data = {};
	uint8_t buffer[1024];
	struct spa_pod_builder builder;
	struct spa_pod *params[1];
	uint32_t n_params = 0;

	pw_init(&argc, &argv);

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
			"midi-src",
			pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Midi",
				PW_KEY_MEDIA_CATEGORY, "Playback",
				PW_KEY_MEDIA_CLASS, "Midi/Source",
				NULL),
			&filter_events,
			&data);

	/* Make a midi output port */
	data.port = pw_filter_add_port(data.filter,
			PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit raw UMP",
				PW_KEY_PORT_NAME, "output",
				NULL),
			NULL, 0);

	/* Update SPA_PARAM_Buffers to request a specific sizes and counts.
	 * This is not mandatory: if you skip this, you'll get default sized
	 * buffers, usually 4k or 32k bytes or so.
	 *
	 * We'll here ask for 4096 bytes as that's enough.
	 */
	spa_pod_builder_init(&builder, buffer, sizeof(buffer));

	params[n_params++] = spa_pod_builder_add_object(&builder,
			/* POD Object for the buffer parameter */
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			/* Default 1 buffer, minimum of 1, max of 32 buffers.
			 * We can do with 1 buffer as we dequeue and queue in the same
			 * cycle.
			 */
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, 32),
			/* MIDI buffers always have 1 data block */
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			/* Buffer size: request default 4096 bytes, min 4096, no maximum */
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(4096, 4096, INT32_MAX),
			/* MIDI buffers have stride 1 */
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(1));

	pw_filter_update_params(data.filter, data.port,
			(const struct spa_pod **)params, n_params);

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
