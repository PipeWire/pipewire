/* Spa MIDI node */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/utils/dll.h>
#include <spa/utils/ringbuffer.h>
#include <spa/monitor/device.h>
#include <spa/control/control.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include <spa/debug/mem.h>
#include <spa/debug/log.h>

#include "midi.h"

#include "bluez5-interface-gen.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.midi.node");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define DEFAULT_CLOCK_NAME	"clock.system.monotonic"

#define DLL_BW			0.05

#define DEFAULT_LATENCY_OFFSET	(0 * SPA_NSEC_PER_MSEC)

#define MAX_BUFFERS		32

#define MIDI_RINGBUF_SIZE	(8192*4)

enum node_role {
	NODE_SERVER,
	NODE_CLIENT,
};

struct props {
	char clock_name[64];
	char device_name[512];
	int64_t latency_offset;
};

struct midi_event_ringbuffer_entry {
	uint64_t time;
	unsigned int size;
};

struct midi_event_ringbuffer {
	struct spa_ringbuffer rbuf;
	uint8_t buf[MIDI_RINGBUF_SIZE];
};

struct buffer {
	uint32_t id;
	unsigned int outgoing:1;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct time_sync {
	uint64_t prev_recv_time;
	uint64_t recv_time;

	uint16_t prev_device_timestamp;
	uint16_t device_timestamp;

	uint64_t device_time;

	struct spa_dll dll;
};

struct port {
	uint32_t id;
	enum spa_direction direction;

	struct spa_audio_info current_format;
	unsigned int have_format:1;

	uint64_t info_all;
	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_latency_info latency;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffers	4
#define IDX_Latency	5
#define N_PORT_PARAMS	6
	struct spa_param_info params[N_PORT_PARAMS];

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list free;
	struct spa_list ready;

	int fd;
	uint16_t mtu;

	struct buffer *buffer;
	struct spa_pod_builder builder;
	struct spa_pod_frame frame;

	struct time_sync sync;

	unsigned int acquired:1;
	GCancellable *acquire_call;

	struct spa_source source;

	struct impl *impl;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	GDBusConnection *conn;
	Bluez5GattCharacteristic1 *proxy;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_PropInfo	0
#define IDX_Props	1
#define IDX_NODE_IO	2
#define N_NODE_PARAMS	3
	struct spa_param_info params[N_NODE_PARAMS];
	struct props props;

#define PORT_IN		0
#define PORT_OUT	1
#define N_PORTS		2
	struct port ports[N_PORTS];

	char *chr_path;

	unsigned int started:1;
	unsigned int following:1;

	struct spa_source timer_source;

	int timerfd;

	struct spa_io_clock *clock;
	struct spa_io_position *position;

	uint32_t duration;
	uint32_t rate;

	uint64_t current_time;
	uint64_t next_time;

	struct midi_event_ringbuffer event_rbuf;

	struct spa_bt_midi_parser parser;
	struct spa_bt_midi_parser tmp_parser;
	uint8_t read_buffer[MIDI_MAX_MTU];

	struct spa_bt_midi_writer writer;

	enum node_role role;

	struct spa_bt_midi_server *server;
};

#define CHECK_PORT(this,d,p)	((p) == 0 && ((d) == SPA_DIRECTION_INPUT || (d) == SPA_DIRECTION_OUTPUT))
#define GET_PORT(this,d,p)	(&(this)->ports[(d) == SPA_DIRECTION_OUTPUT ? PORT_OUT : PORT_IN])

static void midi_event_ringbuffer_init(struct midi_event_ringbuffer *mbuf)
{
	spa_ringbuffer_init(&mbuf->rbuf);
}

static int midi_event_ringbuffer_push(struct midi_event_ringbuffer *mbuf,
		uint64_t time, uint8_t *event, unsigned int size)
{
	const unsigned int bufsize = sizeof(mbuf->buf);
	int32_t avail;
	uint32_t index;
	struct midi_event_ringbuffer_entry evt = {
		.time = time,
		.size = size
	};

	avail = spa_ringbuffer_get_write_index(&mbuf->rbuf, &index);
	if (avail < 0 || avail + sizeof(evt) + size > bufsize)
		return -ENOSPC;

	spa_ringbuffer_write_data(&mbuf->rbuf, mbuf->buf, bufsize, index % bufsize,
			&evt, sizeof(evt));
	index += sizeof(evt);
	spa_ringbuffer_write_update(&mbuf->rbuf, index);
	spa_ringbuffer_write_data(&mbuf->rbuf, mbuf->buf, bufsize, index % bufsize,
			event, size);
	index += size;
	spa_ringbuffer_write_update(&mbuf->rbuf, index);

	return 0;
}

static int midi_event_ringbuffer_peek(struct midi_event_ringbuffer *mbuf, uint64_t *time, unsigned int *size)
{
	const unsigned bufsize = sizeof(mbuf->buf);
	int32_t avail;
	uint32_t index;
	struct midi_event_ringbuffer_entry evt;

	avail = spa_ringbuffer_get_read_index(&mbuf->rbuf, &index);
	if (avail < (int)sizeof(evt))
		return -ENOENT;

	spa_ringbuffer_read_data(&mbuf->rbuf, mbuf->buf, bufsize, index % bufsize,
					&evt, sizeof(evt));

	*time = evt.time;
	*size = evt.size;
	return 0;
}

static int midi_event_ringbuffer_pop(struct midi_event_ringbuffer *mbuf, uint8_t *data, size_t max_size)
{
	const unsigned bufsize = sizeof(mbuf->buf);
	int32_t avail;
	uint32_t index;
	struct midi_event_ringbuffer_entry evt;

	avail = spa_ringbuffer_get_read_index(&mbuf->rbuf, &index);
	if (avail < (int)sizeof(evt))
		return -ENOENT;

	spa_ringbuffer_read_data(&mbuf->rbuf, mbuf->buf, bufsize, index % bufsize,
					&evt, sizeof(evt));
	index += sizeof(evt);
	avail -= sizeof(evt);
	spa_ringbuffer_read_update(&mbuf->rbuf, index);

	if ((uint32_t)avail < evt.size) {
		/* corrupted ringbuffer: should never happen */
		spa_assert_not_reached();
		return -EINVAL;
	}

	if (evt.size <= max_size)
		spa_ringbuffer_read_data(&mbuf->rbuf, mbuf->buf, bufsize, index % bufsize,
				data, SPA_MIN(max_size, evt.size));
	index += evt.size;
	spa_ringbuffer_read_update(&mbuf->rbuf, index);

	if (evt.size > max_size)
		return -ENOSPC;

	return 0;
}

static void reset_props(struct props *props)
{
	props->latency_offset = DEFAULT_LATENCY_OFFSET;
	strncpy(props->clock_name, DEFAULT_CLOCK_NAME, sizeof(props->clock_name));
	props->device_name[0] = '\0';
}

static bool is_following(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
}

static int set_timeout(struct impl *this, uint64_t time)
{
	struct itimerspec ts;
	ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	return spa_system_timerfd_settime(this->data_system,
			this->timerfd, SPA_FD_TIMER_ABSTIME, &ts, NULL);
}

static int set_timers(struct impl *this)
{
	struct timespec now;

	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);
	this->next_time = SPA_TIMESPEC_TO_NSEC(&now);

	return set_timeout(this, this->following ? 0 : this->next_time);
}

static void recycle_buffer(struct impl *this, struct port *port, uint32_t buffer_id)
{
	struct buffer *b = &port->buffers[buffer_id];

	if (b->outgoing) {
		spa_log_trace(this->log, "%p: recycle buffer %u", this, buffer_id);
		spa_list_append(&port->free, &b->link);
		b->outgoing = false;
	}
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_list_init(&port->free);
		spa_list_init(&port->ready);
		port->n_buffers = 0;
	}
	return 0;
}

static void reset_buffers(struct port *port)
{
	uint32_t i;

	spa_list_init(&port->free);
	spa_list_init(&port->ready);

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];

		if (port->direction == SPA_DIRECTION_OUTPUT) {
			spa_list_append(&port->free, &b->link);
			b->outgoing = false;
		} else {
			b->outgoing = true;
		}
	}
}

static struct buffer *peek_buffer(struct impl *this, struct port *port)
{
	if (spa_list_is_empty(&port->free))
		return NULL;
	return spa_list_first(&port->free, struct buffer, link);
}

static int prepare_buffer(struct impl *this, struct port *port)
{
	if (port->buffer != NULL)
		return 0;
	if ((port->buffer = peek_buffer(this, port)) == NULL)
		return -EPIPE;

	spa_pod_builder_init(&port->builder,
			port->buffer->buf->datas[0].data,
			port->buffer->buf->datas[0].maxsize);
        spa_pod_builder_push_sequence(&port->builder, &port->frame, 0);

	return 0;
}

static int finish_buffer(struct impl *this, struct port *port)
{
	if (port->buffer == NULL)
		return 0;

	spa_pod_builder_pop(&port->builder, &port->frame);

	port->buffer->buf->datas[0].chunk->offset = 0;
	port->buffer->buf->datas[0].chunk->size = port->builder.state.offset;

	/* move buffer to ready queue */
	spa_list_remove(&port->buffer->link);
	spa_list_append(&port->ready, &port->buffer->link);
	port->buffer = NULL;

	return 0;
}

/* Replace value -> value + n*period, to minimize |value - target| */
static int64_t unwrap_to_closest(int64_t value, int64_t target, int64_t period)
{
	if (value > target)
		value -= SPA_ROUND_DOWN(value - target + period/2, period);
	if (value < target)
		value += SPA_ROUND_DOWN(target - value + period/2, period);
	return value;
}

static int64_t time_diff(uint64_t a, uint64_t b)
{
	if (a >= b)
		return a - b;
	else
		return -(int64_t)(b - a);
}

static void midi_event_get_last_timestamp(void *user_data, uint16_t timestamp, uint8_t *data, size_t size)
{
	int *last_timestamp = user_data;
	*last_timestamp = timestamp;
}

static uint64_t midi_convert_time(struct time_sync *sync, uint16_t timestamp)
{
	int offset;

	/*
	 * sync->device_timestamp is a device timestamp that corresponds to system
	 * clock time sync->device_time.
	 *
	 * It is the timestamp of the last MIDI event in the current packet, so we can
	 * assume here no event here has timestamp after it.
	 */
	if (timestamp > sync->device_timestamp)
		offset = sync->device_timestamp + MIDI_CLOCK_PERIOD_MSEC - timestamp;
	else
		offset = sync->device_timestamp - timestamp;

	return sync->device_time - offset * SPA_NSEC_PER_MSEC;
}

static void midi_event_recv(void *user_data, uint16_t timestamp, uint8_t *data, size_t size)
{
	struct impl *this = user_data;
	struct port *port = &this->ports[PORT_OUT];
	struct time_sync *sync = &port->sync;
	uint64_t time;
	int res;

	spa_assert(size > 0);

	time = midi_convert_time(sync, timestamp);

	spa_log_trace(this->log, "%p: event:0x%x size:%d timestamp:%d time:%"PRIu64"",
			this, (int)data[0], (int)size, (int)timestamp, (uint64_t)time);

	res = midi_event_ringbuffer_push(&this->event_rbuf, time, data, size);
	if (res < 0) {
		midi_event_ringbuffer_init(&this->event_rbuf);
		spa_log_warn(this->log, "%p: MIDI receive buffer overflow: %s",
				this, spa_strerror(res));
	}
}

static int unacquire_port(struct port *port)
{
	struct impl *this = port->impl;

	if (!port->acquired)
		return 0;

	spa_log_debug(this->log, "%p: unacquire port:%d", this, port->direction);

	shutdown(port->fd, SHUT_RDWR);
	close(port->fd);
	port->fd = -1;
	port->acquired = false;

	if (this->server)
		spa_bt_midi_server_released(this->server,
				(port->direction == SPA_DIRECTION_OUTPUT));

	return 0;
}

static int do_unacquire_port(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct port *port = user_data;

	/* in main thread */
	unacquire_port(port);
	return 0;
}

static void on_ready_read(struct spa_source *source)
{
	struct port *port = source->data;
	struct impl *this = port->impl;
	struct timespec now;
	int res, size, last_timestamp;

	if (SPA_FLAG_IS_SET(source->rmask, SPA_IO_ERR) ||
			SPA_FLAG_IS_SET(source->rmask, SPA_IO_HUP)) {
		spa_log_debug(this->log, "%p: port:%d ERR/HUP", this, port->direction);
		goto stop;
	}

	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);

	/* read data from socket */
again:
	size = recv(port->fd, this->read_buffer, sizeof(this->read_buffer), MSG_DONTWAIT | MSG_NOSIGNAL);
	if (size == 0) {
		return;
	} else if (size < 0) {
		if (errno == EINTR)
			goto again;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		goto stop;
	}

	spa_log_trace(this->log, "%p: port:%d recv data size:%d", this, port->direction, size);
	spa_debug_log_mem(this->log, SPA_LOG_LEVEL_TRACE, 4, this->read_buffer, size);

	if (port->direction != SPA_DIRECTION_OUTPUT) {
		/* Just monitor errors for the input port */
		spa_log_debug(this->log, "%p: port:%d is not RX port; ignoring data",
				this, port->direction);
		return;
	}

	/* prepare for producing events */
	if (port->io == NULL || port->n_buffers == 0 || !this->started)
		return;

	/*
	 * Remote clock synchronization:
	 *
	 * Assume: Last timestamp in packet on average corresponds to packet send time.
	 * There is some unknown latency in between, but on average it is constant.
	 *
	 * The `device_time` computed below is the estimated wall-clock time
	 * corresponding to the timestamp `device_timestamp` of the last event
	 * in the packet. This timestamp is late by the average transmission latency,
	 * which is unknown.
	 *
	 * Packet reception jitter and any clock drift is smoothed over with DLL.
	 * The estimated timestamps are stable and preserve event intervals.
	 *
	 * To allow latency_offset to work better, we don't write the events
	 * to the output buffer here, but instead put them to a ringbuffer.
	 * This is because if the offset shifts events to later buffers,
	 * this is simpler to handle with the rbuf.
	 */
	last_timestamp = -1;
	spa_bt_midi_parser_dup(&this->parser, &this->tmp_parser, true);
	res = spa_bt_midi_parser_parse(&this->tmp_parser, this->read_buffer, size, true,
			midi_event_get_last_timestamp, &last_timestamp);
	if (res >= 0 && last_timestamp >= 0) {
		struct time_sync *sync = &port->sync;
		int64_t clock_elapsed;
		int64_t device_elapsed;
		int64_t err_nsec;
		double corr, tcorr;

		sync->prev_recv_time = sync->recv_time;
		sync->recv_time = SPA_TIMESPEC_TO_NSEC(&now);

		sync->prev_device_timestamp = sync->device_timestamp;
		sync->device_timestamp = last_timestamp;

		if (port->sync.prev_recv_time == 0) {
			sync->prev_recv_time = sync->recv_time;
			sync->prev_device_timestamp = sync->device_timestamp;
			spa_dll_init(&sync->dll);
		}
		if (SPA_UNLIKELY(sync->dll.bw == 0))
			spa_dll_set_bw(&sync->dll, DLL_BW, 1024, 48000);

		/* move device clock forward */
		clock_elapsed = sync->recv_time - sync->prev_recv_time;

		device_elapsed = (int)sync->device_timestamp - (int)sync->prev_device_timestamp;
		device_elapsed *= SPA_NSEC_PER_MSEC;
		device_elapsed = unwrap_to_closest(device_elapsed, clock_elapsed, MIDI_CLOCK_PERIOD_NSEC);
		sync->device_time += device_elapsed;

		/* smooth clock sync */
		err_nsec = time_diff(sync->recv_time, sync->device_time);
		corr = spa_dll_update(&sync->dll,
				-SPA_CLAMP(err_nsec, -20*SPA_NSEC_PER_MSEC, 20*SPA_NSEC_PER_MSEC)
				* this->rate / SPA_NSEC_PER_SEC);
		tcorr = SPA_MIN(device_elapsed, SPA_NSEC_PER_SEC) * (corr - 1);
		sync->device_time += tcorr;

		/* reset if too much off */
		if (err_nsec < -50 * SPA_NSEC_PER_MSEC ||
				err_nsec > 200 * SPA_NSEC_PER_MSEC ||
				SPA_ABS(tcorr) > 20*SPA_NSEC_PER_MSEC ||
				device_elapsed < 0) {
			spa_log_debug(this->log, "%p: device clock sync off too much: resync", this);
			spa_dll_init(&sync->dll);
			sync->device_time = sync->recv_time;
		}

		spa_log_debug(this->log,
				"timestamp:%d dt:%d dt2:%d err:%.1f tcorr:%.2f (ms) corr:%f",
				(int)sync->device_timestamp,
				(int)(clock_elapsed/SPA_NSEC_PER_MSEC),
				(int)(device_elapsed/SPA_NSEC_PER_MSEC),
				(double)err_nsec / SPA_NSEC_PER_MSEC,
				tcorr/SPA_NSEC_PER_MSEC,
				corr);
	}

	/* put midi event data to the buffer */
	res = spa_bt_midi_parser_parse(&this->parser, this->read_buffer, size, false,
			midi_event_recv, this);
	if (res < 0) {
		/* bad data */
		spa_bt_midi_parser_init(&this->parser);

		spa_log_info(this->log, "BLE MIDI data packet parsing failed: %d", res);
		spa_debug_log_mem(this->log, SPA_LOG_LEVEL_DEBUG, 4, this->read_buffer, size);
	}

	return;

stop:
	spa_log_debug(this->log, "%p: port:%d stopping port", this, port->direction);

	if (port->source.loop)
		spa_loop_remove_source(this->data_loop, &port->source);

	/* port->acquired is updated only from the main thread */
	spa_loop_invoke(this->main_loop, do_unacquire_port, 0, NULL, 0, false, port);
}

static int process_output(struct impl *this)
{
	struct port *port = &this->ports[PORT_OUT];
	struct buffer *buffer;
	struct spa_io_buffers *io = port->io;

	/* Check if we are able to process */
	if (io == NULL || !port->acquired)
		return SPA_STATUS_OK;

	/* Return if we already have a buffer */
	if (io->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;

	/* Recycle */
	if (io->buffer_id < port->n_buffers) {
		recycle_buffer(this, port, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	/* Produce buffer */
	if (prepare_buffer(this, port) >= 0) {
		/*
		 * this->current_time is at the end time of the buffer, and offsets
		 * are recorded vs. the start of the buffer.
		 */
		const uint64_t start_time = this->current_time
			- this->duration * SPA_NSEC_PER_SEC / this->rate;
		const uint64_t end_time = this->current_time;
		uint64_t time;
		uint32_t offset;
		void *buf;
		unsigned int size;
		int res;

		while (true) {
			res = midi_event_ringbuffer_peek(&this->event_rbuf, &time, &size);
			if (res < 0)
				break;

			time -= this->props.latency_offset;

			if (time > end_time) {
				break;
			} else if (time + SPA_NSEC_PER_MSEC < start_time) {
				/* Log events in the past by more than 1 ms, but don't
				 * do anything about them. The user can change the latency
				 * offset to choose whether to tradeoff latency for more
				 * accurate timestamps.
				 *
				 * TODO: maybe this information should be available in
				 * a more visible place, some latency property?
				 */
				spa_log_debug(this->log, "%p: event in the past by %d ms",
						this, (int)((start_time - time) / SPA_NSEC_PER_MSEC));
			}

			time = SPA_MAX(time, start_time) - start_time;
			offset = time * this->rate / SPA_NSEC_PER_SEC;
			offset = SPA_CLAMP(offset, 0u, this->duration - 1);

			spa_pod_builder_control(&port->builder, offset, SPA_CONTROL_Midi);
			buf = spa_pod_builder_reserve_bytes(&port->builder, size);
			if (buf) {
				midi_event_ringbuffer_pop(&this->event_rbuf, buf, size);

				spa_log_trace(this->log, "%p: produce event:0x%x offset:%d time:%"PRIu64"",
						this, (int)*(uint8_t*)buf, (int)offset,
						(uint64_t)(start_time + offset * SPA_NSEC_PER_SEC / this->rate));
			}
		}

		finish_buffer(this, port);
	}

	/* Return if there are no buffers ready to be processed */
	if (spa_list_is_empty(&port->ready))
		return SPA_STATUS_OK;

	/* Get the new buffer from the ready list */
	buffer = spa_list_first(&port->ready, struct buffer, link);
	spa_list_remove(&buffer->link);
	buffer->outgoing = true;

	/* Set the new buffer in IO */
	io->buffer_id = buffer->id;
	io->status = SPA_STATUS_HAVE_DATA;

	/* Notify we have a buffer ready to be processed */
	return SPA_STATUS_HAVE_DATA;
}

static int flush_packet(struct impl *this)
{
	struct port *port = &this->ports[PORT_IN];
	int res;

	if (this->writer.size == 0)
		return 0;

	res = send(port->fd, this->writer.buf, this->writer.size,
			MSG_DONTWAIT | MSG_NOSIGNAL);
	if (res < 0)
		return -errno;

	spa_log_trace(this->log, "%p: send packet size:%d", this, this->writer.size);
	spa_debug_log_mem(this->log, SPA_LOG_LEVEL_TRACE, 4, this->writer.buf, this->writer.size);

	return 0;
}

static int write_data(struct impl *this, struct spa_data *d)
{
	struct port *port = &this->ports[PORT_IN];
	struct spa_pod_sequence *pod;
	struct spa_pod_control *c;
	uint64_t time;
	int res;

	pod = spa_pod_from_data(d->data, d->maxsize, d->chunk->offset, d->chunk->size);
	if (pod == NULL) {
		spa_log_warn(this->log, "%p: invalid sequence in buffer max:%u offset:%u size:%u",
				this, d->maxsize, d->chunk->offset, d->chunk->size);
		return -EINVAL;
	}

	spa_bt_midi_writer_init(&this->writer, port->mtu);
	time = 0;

	SPA_POD_SEQUENCE_FOREACH(pod, c) {
		uint8_t *event;
		size_t size;

		if (c->type != SPA_CONTROL_Midi)
			continue;

		time = SPA_MAX(time, this->current_time + c->offset * SPA_NSEC_PER_SEC / this->rate);
		event = SPA_POD_BODY(&c->value);
		size = SPA_POD_BODY_SIZE(&c->value);

		spa_log_trace(this->log, "%p: output event:0x%x time:%"PRIu64, this,
				(size > 0) ? event[0] : 0, time);

		do {
			res = spa_bt_midi_writer_write(&this->writer,
					time, event, size);
			if (res < 0) {
				return res;
			} else if (res) {
				int res2;
				if ((res2 = flush_packet(this)) < 0)
					return res2;
			}
		} while (res);
	}

	if ((res = flush_packet(this)) < 0)
		return res;

	return 0;
}

static int process_input(struct impl *this)
{
	struct port *port = &this->ports[PORT_IN];
	struct buffer *b;
	struct spa_io_buffers *io = port->io;
	int res;

	/* Check if we are able to process */
	if (io == NULL || !port->acquired)
		return SPA_STATUS_OK;

	if (io->status != SPA_STATUS_HAVE_DATA || io->buffer_id >= port->n_buffers)
		return SPA_STATUS_OK;

	b = &port->buffers[io->buffer_id];
	if (!b->outgoing) {
		spa_log_warn(this->log, "%p: buffer %u not outgoing", this, io->buffer_id);
		io->status = -EINVAL;
		return -EINVAL;
	}

	if ((res = write_data(this, &b->buf->datas[0])) < 0) {
		spa_log_info(this->log, "%p: writing data failed: %s",
				this, spa_strerror(res));
	}

	port->io->buffer_id = b->id;
	io->status = SPA_STATUS_NEED_DATA;
	spa_node_call_reuse_buffer(&this->callbacks, 0, io->buffer_id);

	return SPA_STATUS_HAVE_DATA;
}

static void update_position(struct impl *this)
{
	if (SPA_LIKELY(this->position)) {
		this->duration = this->position->clock.duration;
		this->rate = this->position->clock.rate.denom;
	} else {
		this->duration = 1024;
		this->rate = 48000;
	}
}

static void on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t exp;
	uint64_t prev_time, now_time;
	int status;

	if (!this->started)
		return;

	if (spa_system_timerfd_read(this->data_system, this->timerfd, &exp) < 0)
		spa_log_warn(this->log, "%p: error reading timerfd: %s", this, strerror(errno));

	prev_time = this->current_time;
	now_time = this->current_time = this->next_time;

	spa_log_trace(this->log, "%p: timer %"PRIu64" %"PRIu64"", this,
			now_time, now_time - prev_time);

	if (SPA_LIKELY(this->position)) {
		this->duration = this->position->clock.target_duration;
		this->rate = this->position->clock.target_rate.denom;
	} else {
		this->duration = 1024;
		this->rate = 48000;
	}

	this->next_time = now_time + this->duration * SPA_NSEC_PER_SEC / this->rate;

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = now_time;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = this->duration;
		this->clock->rate_diff = 1.0f;
		this->clock->next_nsec = this->next_time;
	}

	status = process_output(this);
	spa_log_trace(this->log, "%p: status:%d", this, status);

	spa_node_call_ready(&this->callbacks, status | SPA_STATUS_NEED_DATA);

	set_timeout(this, this->next_time);
}

static int do_start(struct impl *this);

static int do_release(struct impl *this);

static int do_stop(struct impl *this);

static void acquire_reply(GObject *source_object, GAsyncResult *res, gpointer user_data, bool notify)
{
	struct port *port;
	struct impl *this;
	const char *method;
	GError *err = NULL;
	GUnixFDList *fd_list = NULL;
	GVariant *fd_handle = NULL;
	int fd;
	guint16 mtu;

	if (notify) {
		bluez5_gatt_characteristic1_call_acquire_notify_finish(
			BLUEZ5_GATT_CHARACTERISTIC1(source_object), &fd_handle, &mtu, &fd_list, res, &err);
	} else {
		bluez5_gatt_characteristic1_call_acquire_write_finish(
			BLUEZ5_GATT_CHARACTERISTIC1(source_object), &fd_handle, &mtu, &fd_list, res, &err);
	}

	if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		/* Operation canceled: user_data may be invalid by now. */
		g_error_free(err);
		return;
	}

	port = user_data;
	this = port->impl;
	method = notify ? "AcquireNotify" : "AcquireWrite";
	if (err) {
		spa_log_error(this->log, "%s.%s() for %s failed: %s",
				BLUEZ_GATT_CHR_INTERFACE, method,
				this->chr_path, err->message);
		goto fail;
	}

	fd = g_unix_fd_list_get(fd_list, g_variant_get_handle(fd_handle), &err);
	if (fd < 0) {
		spa_log_error(this->log, "%s.%s() for %s failed to get fd: %s",
				BLUEZ_GATT_CHR_INTERFACE, method,
				this->chr_path, err->message);
		goto fail;
	}

	spa_log_info(this->log, "%p: BLE MIDI %s %s success mtu:%d",
			this, this->chr_path, method, mtu);
	port->fd = fd;
	port->mtu = mtu;
	port->acquired = true;

	if (port->direction == SPA_DIRECTION_OUTPUT) {
		spa_bt_midi_parser_init(&this->parser);

		/* Start source */
		port->source.data = port;
		port->source.fd = port->fd;
		port->source.func = on_ready_read;
		port->source.mask = SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR;
		port->source.rmask = 0;
		spa_loop_add_source(this->data_loop, &port->source);
	}
	return;

fail:
	g_error_free(err);
	g_clear_object(&fd_list);
	g_clear_object(&fd_handle);
	do_stop(this);
	do_release(this);
}

static void acquire_notify_reply(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	acquire_reply(source_object, res, user_data, true);
}

static void acquire_write_reply(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	acquire_reply(source_object, res, user_data, false);
}

static int do_acquire(struct port *port)
{
	struct impl *this = port->impl;
	const char *method = (port->direction == SPA_DIRECTION_OUTPUT) ?
		"AcquireNotify" : "AcquireWrite";
	GVariant *options;
	GVariantBuilder builder;

	if (port->acquired)
		return 0;
	if (port->acquire_call)
		return 0;

	spa_log_info(this->log,
			"%p: port %d: client %s for BLE MIDI device characteristic %s",
			this, port->direction, method, this->chr_path);

	port->acquire_call = g_cancellable_new();

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	options = g_variant_builder_end(&builder);

	if (port->direction == SPA_DIRECTION_OUTPUT) {
		bluez5_gatt_characteristic1_call_acquire_notify(
				BLUEZ5_GATT_CHARACTERISTIC1(this->proxy),
				options,
				NULL,
				port->acquire_call,
				acquire_notify_reply,
				port);
	} else {
		bluez5_gatt_characteristic1_call_acquire_write(
				BLUEZ5_GATT_CHARACTERISTIC1(this->proxy),
				options,
				NULL,
				port->acquire_call,
				acquire_write_reply,
				port);
	}

	return 0;
}

static int server_do_acquire(struct port *port, int fd, uint16_t mtu)
{
	struct impl *this = port->impl;
	const char *method = (port->direction == SPA_DIRECTION_OUTPUT) ?
		"AcquireWrite" : "AcquireNotify";

	spa_log_info(this->log,
			"%p: port %d: server %s for BLE MIDI device characteristic %s",
			this, port->direction, method, this->server->chr_path);

	if (port->acquired) {
		spa_log_info(this->log,
				"%p: port %d: %s failed: already acquired",
				this, port->direction, method);
		return -EBUSY;
	}

	port->fd = fd;
	port->mtu = mtu;

	if (port->direction == SPA_DIRECTION_OUTPUT)
		spa_bt_midi_parser_init(&this->parser);

	/* Start source */
	port->source.data = port;
	port->source.fd = port->fd;
	port->source.func = on_ready_read;
	port->source.mask = SPA_IO_HUP | SPA_IO_ERR;
	if (port->direction == SPA_DIRECTION_OUTPUT)
		port->source.mask |= SPA_IO_IN;
	port->source.rmask = 0;
	spa_loop_add_source(this->data_loop, &port->source);

	port->acquired = true;
	return 0;
}

static int server_acquire_write(void *user_data, int fd, uint16_t mtu)
{
	struct impl *this = user_data;
	return server_do_acquire(&this->ports[PORT_OUT], fd, mtu);
}

static int server_acquire_notify(void *user_data, int fd, uint16_t mtu)
{
	struct impl *this = user_data;
	return server_do_acquire(&this->ports[PORT_IN], fd, mtu);
}

static int server_release(void *user_data)
{
	struct impl *this = user_data;
	do_release(this);
	return 0;
}

static const char *server_description(void *user_data)
{
	struct impl *this = user_data;
	return this->props.device_name;
}

static int do_remove_port_source(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *this = user_data;
	int i;

	for (i = 0; i < N_PORTS; ++i) {
		struct port *port = &this->ports[i];

		if (port->source.loop)
			spa_loop_remove_source(this->data_loop, &port->source);
	}

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
		bool async,
		uint32_t seq,
		const void *data,
		size_t size,
		void *user_data)
{
	struct impl *this = user_data;

	if (this->timer_source.loop)
		spa_loop_remove_source(this->data_loop, &this->timer_source);
	set_timeout(this, 0);
	return 0;
}

static int do_stop(struct impl *this)
{
	int res = 0;

	spa_log_debug(this->log, "%p: stop", this);

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, this);

	this->started = false;

	return res;
}

static int do_release(struct impl *this)
{
	int res = 0;
	size_t i;

	spa_log_debug(this->log, "%p: release", this);

	spa_loop_invoke(this->data_loop, do_remove_port_source, 0, NULL, 0, true, this);

	for (i = 0; i < N_PORTS; ++i) {
		struct port *port = &this->ports[i];

		g_cancellable_cancel(port->acquire_call);
		g_clear_object(&port->acquire_call);

		unacquire_port(port);
	}

	return res;
}

static int do_start(struct impl *this)
{
	int res;
	size_t i;

	if (this->started)
		return 0;

	this->following = is_following(this);

	update_position(this);

	spa_log_debug(this->log, "%p: start following:%d",
			this, this->following);

	for (i = 0; i < N_PORTS; ++i) {
		struct port *port = &this->ports[i];

		switch (this->role) {
		case NODE_CLIENT:
			/* Acquire Bluetooth I/O */
			if ((res = do_acquire(port)) < 0) {
				do_stop(this);
				do_release(this);
				return res;
			}
			break;
		case NODE_SERVER:
			/*
			 * In MIDI server role, the device/BlueZ invokes
			 * the acquire asynchronously as available/needed.
			 */
			break;
		default:
			spa_assert_not_reached();
		}

		reset_buffers(port);
	}

	midi_event_ringbuffer_init(&this->event_rbuf);

	this->started = true;

	/* Start timer */
	this->timer_source.data = this;
	this->timer_source.fd = this->timerfd;
	this->timer_source.func = on_timeout;
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->timer_source);

	set_timers(this);

	return 0;
}


static int do_reassign_follower(struct spa_loop *loop,
		bool async,
		uint32_t seq,
		const void *data,
		size_t size,
		void *user_data)
{
	struct impl *this = user_data;
	set_timers(this);
	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	bool following;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		this->clock = data;
		if (this->clock != NULL) {
			spa_scnprintf(this->clock->name,
					sizeof(this->clock->name),
					"%s", this->props.clock_name);
		}
		break;
	case SPA_IO_Position:
		this->position = data;
		break;
	default:
		return -ENOENT;
	}

	following = is_following(this);
	if (this->started && following != this->following) {
		spa_log_debug(this->log, "%p: reassign follower %d->%d", this, this->following, following);
		this->following = following;
		spa_loop_invoke(this->data_loop, do_reassign_follower, 0, NULL, 0, true, this);
	}

	return 0;
}

static void emit_node_info(struct impl *this, bool full);

static int impl_node_enum_params(void *object, int seq,
		uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_latencyOffsetNsec),
				SPA_PROP_INFO_description, SPA_POD_String("Latency offset (ns)"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Long(0LL, INT64_MIN, INT64_MAX));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_deviceName),
				SPA_PROP_INFO_description, SPA_POD_String("Device name"),
				SPA_PROP_INFO_type, SPA_POD_String(p->device_name));
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_latencyOffsetNsec, SPA_POD_Long(p->latency_offset),
				SPA_PROP_deviceName, SPA_POD_String(p->device_name));
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static void emit_port_info(struct impl *this, struct port *port, bool full);

static void set_latency(struct impl *this, bool emit_latency)
{
	struct port *port = &this->ports[PORT_OUT];

	port->latency.min_ns = port->latency.max_ns = this->props.latency_offset;

	if (emit_latency) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		port->params[IDX_Latency].flags ^= SPA_PARAM_INFO_SERIAL;
		emit_port_info(this, port, false);
	}
}

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct props new_props = this->props;
	int changed = 0;

	if (param == NULL) {
		reset_props(&new_props);
	} else {
		spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_Props, NULL,
				SPA_PROP_latencyOffsetNsec, SPA_POD_OPT_Long(&new_props.latency_offset),
				SPA_PROP_deviceName, SPA_POD_OPT_Stringn(new_props.device_name,
						sizeof(new_props.device_name)));
	}

	changed = (memcmp(&new_props, &this->props, sizeof(struct props)) != 0);
	this->props = new_props;

	if (changed)
		set_latency(this, true);

	return changed;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		if (apply_props(this, param) > 0) {
			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
			this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
			emit_node_info(this, false);
		}
		break;
	}
	default:
		return -ENOENT;
	}

	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res, res2;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if ((res = do_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Pause:
		if ((res = do_stop(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Suspend:
		res = do_stop(this);
		if (this->role == NODE_CLIENT)
			res2 = do_release(this);
		else
			res2 = 0;
		if (res < 0)
			return res;
		if (res2 < 0)
			return res2;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full)
{
	const struct spa_dict_item node_info_items[] = {
		{ SPA_KEY_DEVICE_API, "bluez5" },
		{ SPA_KEY_MEDIA_CLASS, "Midi/Bridge" },
	};
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props =  &SPA_DICT_INIT_ARRAY(node_info_items);
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		spa_node_emit_port_info(&this->hooks, port->direction, port->id, &port->info);
		port->info.change_mask = old;
	}
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;
	size_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);

	for (i = 0; i < N_PORTS; ++i)
		emit_port_info(this, &this->ports[i], true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks,
		void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_node_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id,
		uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{

	struct impl *this = object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,	   SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;

	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
				SPA_FORMAT_mediaType,	   SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;

	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,	   SPA_POD_CHOICE_RANGE_Int(
					4096, 4096, INT32_MAX),
				SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(1));
		break;

	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
					SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0:
			param = spa_latency_build(&b, id, &port->latency);
			break;
		default:
			return 0;
		}
		break;

	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int port_set_format(struct impl *this, struct port *port,
		uint32_t flags,
		const struct spa_pod *format)
{
	int err;

	if (format == NULL) {
		if (!port->have_format)
			return 0;

		clear_buffers(this, port);
		port->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_application ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_control)
			return -EINVAL;

		port->current_format = info;
		port->have_format = true;
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
	port->info.rate = SPA_FRACTION(1, 1);
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, port, false);

	return 0;
}

static int
impl_node_port_set_param(void *object,
		enum spa_direction direction, uint32_t port_id,
		uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *this = object;
	struct port *port;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_PARAM_Format:
		res = port_set_format(this, port, flags, param);
		break;
	case SPA_PARAM_Latency:
		res = 0;
		break;
	default:
		res = -ENOENT;
		break;
	}
	return res;
}

static int
impl_node_port_use_buffers(void *object,
		enum spa_direction direction, uint32_t port_id,
		uint32_t flags,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, "%p: use buffers %d", this, n_buffers);

	if (!port->have_format)
		return -EIO;

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		struct spa_data *d = buffers[i]->datas;

		b->buf = buffers[i];
		b->id = i;

		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (d[0].data == NULL) {
			spa_log_error(this->log, "%p: need mapped memory", this);
			return -EINVAL;
		}
	}
	port->n_buffers = n_buffers;

	reset_buffers(port);

	return 0;
}

static int
impl_node_port_set_io(void *object,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t id,
		void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_PORT(this, SPA_DIRECTION_OUTPUT, port_id);

	if (port->n_buffers == 0)
		return -EIO;

	if (buffer_id >= port->n_buffers)
		return -EINVAL;

	recycle_buffer(this, port, buffer_id);

	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	int status = SPA_STATUS_OK;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (!this->started)
		return SPA_STATUS_OK;

	if (this->following) {
		if (this->position) {
			this->current_time = this->position->clock.nsec;
		} else {
			struct timespec now = { 0 };
			spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);
			this->current_time = SPA_TIMESPEC_TO_NSEC(&now);
		}
	}

	update_position(this);

	if (this->following)
		status |= process_output(this);

	status |= process_input(this);

	return status;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static const struct spa_bt_midi_server_cb impl_server = {
	.acquire_write = server_acquire_write,
	.acquire_notify = server_acquire_notify,
	.release = server_release,
	.get_description = server_description,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *) handle;

	do_stop(this);
	do_release(this);

	free(this->chr_path);
	if (this->timerfd > 0)
		spa_system_close(this->data_system, this->timerfd);
	if (this->server)
		spa_bt_midi_server_destroy(this->server);
	g_clear_object(&this->proxy);
	g_clear_object(&this->conn);

	spa_zero(*this);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
		const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
		struct spa_handle *handle,
		const struct spa_dict *info,
		const struct spa_support *support,
		uint32_t n_support)
{
	struct impl *this;
	const char *device_name = "";
	int res = 0;
	GError *err = NULL;
	size_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);

	if (this->log == NULL)
		return -EINVAL;

	spa_log_topic_init(this->log, &log_topic);

	if (!(info && spa_atob(spa_dict_lookup(info, SPA_KEY_API_GLIB_MAINLOOP)))) {
		spa_log_error(this->log, "Glib mainloop is not usable: %s not set",
				SPA_KEY_API_GLIB_MAINLOOP);
		return -EINVAL;
	}

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data system is needed");
		return -EINVAL;
	}

	this->role = NODE_CLIENT;

	if (info) {
		const char *str;

		if ((str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_PATH)) != NULL)
			this->chr_path = strdup(str);

		if ((str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_ROLE)) != NULL) {
			if (spa_streq(str, "server"))
				this->role = NODE_SERVER;
		}

		if ((str = spa_dict_lookup(info, "node.nick")) != NULL)
			device_name = str;
		else if ((str = spa_dict_lookup(info, "node.description")) != NULL)
			device_name = str;
	}

	if (this->role == NODE_CLIENT && this->chr_path == NULL) {
		spa_log_error(this->log, "missing MIDI service characteristic path");
		res = -EINVAL;
		goto fail;
	}

	this->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (this->conn == NULL) {
		spa_log_error(this->log, "failed to get dbus connection: %s",
				err->message);
		g_error_free(err);
		res = -EIO;
		goto fail;
	}

	this->node.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_Node,
				SPA_VERSION_NODE,
				&impl_node, this);
	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	spa_scnprintf(this->props.device_name, sizeof(this->props.device_name),
			"%s", device_name);

	/* set the node info */
	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
		SPA_NODE_CHANGE_MASK_PROPS |
		SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 1;
	this->info.max_output_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_NODE_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	/* set the port info */
	for (i = 0; i < N_PORTS; ++i) {
		struct port *port = &this->ports[i];
		static const struct spa_dict_item in_port_items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "8 bit raw midi"),
			SPA_DICT_ITEM_INIT(SPA_KEY_PORT_NAME, "in"),
			SPA_DICT_ITEM_INIT(SPA_KEY_PORT_ALIAS, "in"),
		};
		static const struct spa_dict_item out_port_items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "8 bit raw midi"),
			SPA_DICT_ITEM_INIT(SPA_KEY_PORT_NAME, "out"),
			SPA_DICT_ITEM_INIT(SPA_KEY_PORT_ALIAS, "out"),
		};
		static const struct spa_dict in_port_props = SPA_DICT_INIT_ARRAY(in_port_items);
		static const struct spa_dict out_port_props = SPA_DICT_INIT_ARRAY(out_port_items);

		spa_zero(*port);

		port->impl = this;

		port->id = 0;
		port->direction = (i == PORT_OUT) ? SPA_DIRECTION_OUTPUT :
			SPA_DIRECTION_INPUT;

		port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PROPS |
			SPA_PORT_CHANGE_MASK_PARAMS;
		port->info = SPA_PORT_INFO_INIT();
		port->info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS;
		port->info.flags = SPA_PORT_FLAG_LIVE |
			SPA_PORT_FLAG_PHYSICAL |
			SPA_PORT_FLAG_TERMINAL;
		port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
		port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
		port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
		port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
		port->info.params = port->params;
		port->info.n_params = N_PORT_PARAMS;
		port->info.props = (i == PORT_OUT) ? &out_port_props : &in_port_props;

		port->latency = SPA_LATENCY_INFO(port->direction);
		port->latency.min_quantum = 1.0f;
		port->latency.max_quantum = 1.0f;

		/* Init the buffer lists */
		spa_list_init(&port->ready);
		spa_list_init(&port->free);
	}

	this->duration = 1024;
	this->rate = 48000;

	set_latency(this, false);

	if (this->role == NODE_SERVER) {
		this->server = spa_bt_midi_server_new(&impl_server, this->conn, this->log, this);
		if (this->server == NULL)
			goto fail;
	} else {
		this->proxy = bluez5_gatt_characteristic1_proxy_new_sync(this->conn,
				G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
				BLUEZ_SERVICE,
				this->chr_path,
				NULL,
				&err);
		if (this->proxy == NULL) {
			spa_log_error(this->log,
					"Failed to create BLE MIDI GATT proxy %s: %s",
					this->chr_path, err->message);
			g_error_free(err);
			res = -EIO;
			goto fail;
		}
	}

	this->timerfd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	return 0;

fail:
	res = (res < 0) ? res : ((errno > 0) ? -errno : -EIO);
	impl_clear(handle);
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info, uint32_t *index)
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

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Pauli Virtanen <pav@iki.fi>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Bluez5 MIDI connection" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_bluez5_midi_node_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_MIDI_NODE,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
