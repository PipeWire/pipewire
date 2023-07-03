/* Spa SCO Source */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
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
#include <spa/utils/result.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/monitor/device.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include <sbc/sbc.h>

#include "defs.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.source.sco");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#include "decode-buffer.h"

#define DEFAULT_CLOCK_NAME	"clock.system.monotonic"

struct props {
	char clock_name[64];
};

#define MAX_BUFFERS 32

struct buffer {
	uint32_t id;
	unsigned int outstanding:1;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct port {
	struct spa_audio_info current_format;
	int frame_size;
	unsigned int have_format:1;

	uint64_t info_all;
	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_io_rate_match *rate_match;
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

	struct spa_bt_decode_buffer buffer;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	uint32_t quantum_limit;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_PropInfo	0
#define IDX_Props	1
#define IDX_NODE_IO	2
#define N_NODE_PARAMS	3
	struct spa_param_info params[N_NODE_PARAMS];
	struct props props;

	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;

	struct port port;

	unsigned int started:1;
	unsigned int start_ready:1;
	unsigned int transport_started:1;
	unsigned int following:1;
	unsigned int matching:1;
	unsigned int resampling:1;
	unsigned int io_error:1;

	struct spa_source timer_source;
	int timerfd;

	struct spa_io_clock *clock;
	struct spa_io_position *position;

	uint64_t current_time;
	uint64_t next_time;

	/* mSBC */
	sbc_t msbc;
	bool msbc_seq_initialized;
	uint8_t msbc_seq;

	/* mSBC frame parsing */
	uint8_t msbc_buffer[MSBC_ENCODED_SIZE];
	uint8_t msbc_buffer_pos;

	struct timespec now;
};

#define CHECK_PORT(this,d,p)	((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

static void reset_props(struct props *props)
{
	strncpy(props->clock_name, DEFAULT_CLOCK_NAME, sizeof(props->clock_name));
}

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
		switch (result.index) {
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		switch (result.index) {
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

static int do_reassign_follower(struct spa_loop *loop,
			bool async,
			uint32_t seq,
			const void *data,
			size_t size,
			void *user_data)
{
	struct impl *this = user_data;
	struct port *port = &this->port;

	set_timers(this);
	if (this->transport_started)
		spa_bt_decode_buffer_recover(&port->buffer);
	return 0;
}

static inline bool is_following(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
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

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct props new_props = this->props;
	int changed = 0;

	if (param == NULL) {
		reset_props(&new_props);
	} else {
		/* noop */
	}

	changed = (memcmp(&new_props, &this->props, sizeof(struct props)) != 0);
	this->props = new_props;
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

static void reset_buffers(struct port *port)
{
	uint32_t i;

	spa_list_init(&port->free);
	spa_list_init(&port->ready);

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		spa_list_append(&port->free, &b->link);
		b->outstanding = false;
	}
}

static void recycle_buffer(struct impl *this, struct port *port, uint32_t buffer_id)
{
	struct buffer *b = &port->buffers[buffer_id];

	if (b->outstanding) {
		spa_log_trace(this->log, "%p: recycle buffer %u", this, buffer_id);
		spa_list_append(&port->free, &b->link);
		b->outstanding = false;
	}
}

/* Append data to msbc buffer, syncing buffer start to frame headers */
static void msbc_buffer_append_byte(struct impl *this, uint8_t byte)
{
        /* Parse mSBC frame header */
        if (this->msbc_buffer_pos == 0) {
                if (byte != 0x01) {
                        this->msbc_buffer_pos = 0;
                        return;
                }
        }
        else if (this->msbc_buffer_pos == 1) {
                if (!((byte & 0x0F) == 0x08 &&
                      ((byte >> 4) & 1) == ((byte >> 5) & 1) &&
                      ((byte >> 6) & 1) == ((byte >> 7) & 1))) {
                        this->msbc_buffer_pos = 0;
                        return;
                }
        }
        else if (this->msbc_buffer_pos == 2) {
                /* .. and beginning of MSBC frame: SYNCWORD + 2 nul bytes */
                if (byte != 0xAD) {
                        this->msbc_buffer_pos = 0;
                        return;
                }
        }
        else if (this->msbc_buffer_pos == 3) {
                if (byte != 0x00) {
                        this->msbc_buffer_pos = 0;
                        return;
                }
        }
        else if (this->msbc_buffer_pos == 4) {
                if (byte != 0x00) {
                        this->msbc_buffer_pos = 0;
                        return;
                }
        }
        else if (this->msbc_buffer_pos >= MSBC_ENCODED_SIZE) {
                /* Packet completed. Reset. */
                this->msbc_buffer_pos = 0;
                msbc_buffer_append_byte(this, byte);
                return;
        }
        this->msbc_buffer[this->msbc_buffer_pos] = byte;
        ++this->msbc_buffer_pos;
}

/* Helper function for debugging */
static SPA_UNUSED void hexdump_to_log(struct impl *this, uint8_t *data, size_t size)
{
	char buf[2048];
	size_t i, col = 0, pos = 0;
	buf[0] = '\0';
	for (i = 0; i < size; ++i) {
		int res;
		res = spa_scnprintf(buf + pos, sizeof(buf) - pos, "%s%02x",
				(col == 0) ? "\n\t" : " ", data[i]);
		if (res < 0)
			break;
		pos += res;
		col = (col + 1) % 16;
	}
	spa_log_trace(this->log, "hexdump (%d bytes):%s", (int)size, buf);
}

/* helper function to detect if a packet consists only of zeros */
static bool is_zero_packet(uint8_t *data, int size)
{
	for (int i = 0; i < size; ++i) {
		if (data[i] != 0) {
			return false;
		}
	}
	return true;
}

static uint32_t preprocess_and_decode_msbc_data(void *userdata, uint8_t *read_data, int size_read)
{
	struct impl *this = userdata;
	struct port *port = &this->port;
	uint32_t decoded = 0;
	int i;

	spa_log_trace(this->log, "handling mSBC data");

	/*
	 * Check if the packet contains only zeros - if so ignore the packet.
	 * This is necessary, because some kernels insert bogus "all-zero" packets
	 * into the datastream.
	 * See https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/549
	 */
	if (is_zero_packet(read_data, size_read))
		return 0;

	for (i = 0; i < size_read; ++i) {
		void *buf;
		uint32_t avail;
		int seq, processed;
		size_t written;

		msbc_buffer_append_byte(this, read_data[i]);

		if (this->msbc_buffer_pos != MSBC_ENCODED_SIZE)
			continue;

		/*
		 * Handle found mSBC packet
		 */

		buf = spa_bt_decode_buffer_get_write(&port->buffer, &avail);

		/* Check sequence number */
		seq = ((this->msbc_buffer[1] >> 4) & 1) |
			((this->msbc_buffer[1] >> 6) & 2);

		spa_log_trace(this->log, "mSBC packet seq=%u", seq);
		if (!this->msbc_seq_initialized) {
			this->msbc_seq_initialized = true;
			this->msbc_seq = seq;
		} else if (seq != this->msbc_seq) {
			/* TODO: PLC (too late to insert data now) */
			spa_log_info(this->log,
					"missing mSBC packet: %u != %u", seq, this->msbc_seq);
			this->msbc_seq = seq;
		}

		this->msbc_seq = (this->msbc_seq + 1) % 4;

		if (avail < MSBC_DECODED_SIZE)
			spa_log_warn(this->log, "Output buffer full, dropping msbc data");

		/* decode frame */
		processed = sbc_decode(
			&this->msbc, this->msbc_buffer + 2, MSBC_ENCODED_SIZE - 3,
					buf, avail, &written);

		if (processed < 0) {
			spa_log_warn(this->log, "sbc_decode failed: %d", processed);
			/* TODO: manage errors */
			continue;
		}

		spa_bt_decode_buffer_write_packet(&port->buffer, written);
		decoded += written;
	}

	return decoded;
}

static int sco_source_cb(void *userdata, uint8_t *read_data, int size_read)
{
	struct impl *this = userdata;
	struct port *port = &this->port;
	uint32_t decoded;
	uint64_t dt;

	/* Drop data when not started */
	if (!this->started)
		return 0;

	if (this->transport == NULL) {
		spa_log_debug(this->log, "no transport, stop reading");
		goto stop;
	}

	/* update the current pts */
	dt = SPA_TIMESPEC_TO_NSEC(&this->now);
	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &this->now);
	dt = SPA_TIMESPEC_TO_NSEC(&this->now) - dt;

	/* handle data read from socket */
#if 0
	hexdump_to_log(this, read_data, size_read);
#endif

	if (this->transport->codec == HFP_AUDIO_CODEC_MSBC) {
		decoded = preprocess_and_decode_msbc_data(userdata, read_data, size_read);
	} else {
		uint32_t avail;
		uint8_t *packet;

		if (size_read != 48 && is_zero_packet(read_data, size_read)) {
			/* Adapter is returning non-standard CVSD stream. For example
			 * Intel 8087:0029 at Firmware revision 0.0 build 191 week 21 2021
			 * on kernel 5.13.19 produces such data.
			 */
			return 0;
		}

		if (size_read % port->frame_size != 0) {
			/* Unaligned data: reception or adapter problem.
			 * Consider the whole packet lost and report.
			 */
			spa_log_debug(this->log,
					"received bad Bluetooth SCO CVSD packet");
			return 0;
		}

		packet = spa_bt_decode_buffer_get_write(&port->buffer, &avail);
		avail = SPA_MIN(avail, (uint32_t)size_read);
		spa_memmove(packet, read_data, avail);
		spa_bt_decode_buffer_write_packet(&port->buffer, avail);

		decoded = avail;
	}

	spa_log_trace(this->log, "read socket data size:%d decoded frames:%d dt:%d dms",
			size_read, decoded / port->frame_size,
			(int)(dt / 100000));

	return 0;

stop:
	this->io_error = true;
	return 1;
}

static int setup_matching(struct impl *this)
{
	struct port *port = &this->port;

	if (!this->transport_started)
		port->buffer.corr = 1.0;

	if (this->position && port->rate_match) {
		port->rate_match->rate = 1 / port->buffer.corr;

		this->matching = this->following;
		this->resampling = this->matching ||
			(port->current_format.info.raw.rate != this->position->clock.target_rate.denom);
	} else {
		this->matching = false;
		this->resampling = false;
	}

	if (port->rate_match)
		SPA_FLAG_UPDATE(port->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE, this->matching);

	return 0;
}

static int produce_buffer(struct impl *this);

static void sco_on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	struct port *port = &this->port;
	uint64_t exp, duration;
	uint32_t rate;
	uint64_t prev_time, now_time;
	int res;

	if (this->started) {
		if ((res = spa_system_timerfd_read(this->data_system, this->timerfd, &exp)) < 0) {
			if (res != -EAGAIN)
				spa_log_warn(this->log, "error reading timerfd: %s",
						spa_strerror(res));
			return;
		}
	}

	prev_time = this->current_time;
	now_time = this->current_time = this->next_time;

	spa_log_trace(this->log, "%p: timer %"PRIu64" %"PRIu64"", this,
			now_time, now_time - prev_time);

	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.target_duration;
		rate = this->position->clock.target_rate.denom;
	} else {
		duration = 1024;
		rate = 48000;
	}

	setup_matching(this);

	this->next_time = now_time + duration * SPA_NSEC_PER_SEC / port->buffer.corr / rate;

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = now_time;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = duration;
		this->clock->rate_diff = port->buffer.corr;
		this->clock->next_nsec = this->next_time;
	}

	if (port->io) {
		int io_status = port->io->status;
		int status = produce_buffer(this);
		spa_log_trace(this->log, "%p: io:%d->%d status:%d", this, io_status, port->io->status, status);
	}

	spa_node_call_ready(&this->callbacks, SPA_STATUS_HAVE_DATA);

	set_timeout(this, this->next_time);
}

static int do_add_source(struct spa_loop *loop,
			 bool async,
			 uint32_t seq,
			 const void *data,
			 size_t size,
			 void *user_data)
{
	struct impl *this = user_data;

	spa_bt_sco_io_set_source_cb(this->transport->sco_io, sco_source_cb, this);

	return 0;
}

static int transport_start(struct impl *this)
{
	struct port *port = &this->port;
	int res;

	/* Don't do anything if the node has already started */
	if (this->transport_started)
		return 0;
	if (!this->start_ready)
		return -EIO;

	spa_log_debug(this->log, "%p: start transport", this);

	/* Make sure the transport is valid */
	spa_return_val_if_fail (this->transport != NULL, -EIO);

	/* Reset the buffers and sample count */
	reset_buffers(port);

	spa_bt_decode_buffer_clear(&port->buffer);
	if ((res = spa_bt_decode_buffer_init(&port->buffer, this->log,
			port->frame_size, port->current_format.info.raw.rate,
			this->quantum_limit, this->quantum_limit)) < 0)
		return res;

	/* 40 ms max buffer */
	spa_bt_decode_buffer_set_max_latency(&port->buffer,
			port->current_format.info.raw.rate * 40 / 1000);

	/* Init mSBC if needed */
	if (this->transport->codec == HFP_AUDIO_CODEC_MSBC) {
		sbc_init_msbc(&this->msbc, 0);
		/* Libsbc expects audio samples by default in host endianness, mSBC requires little endian */
		this->msbc.endian = SBC_LE;
		this->msbc_seq_initialized = false;

		this->msbc_buffer_pos = 0;
	}

	this->io_error = false;

	/* Start socket i/o */
	if ((res = spa_bt_transport_ensure_sco_io(this->transport, this->data_loop)) < 0)
		goto fail;
	spa_loop_invoke(this->data_loop, do_add_source, 0, NULL, 0, true, this);

	/* Set the started flag */
	this->transport_started = true;

	return 0;

fail:
	return res;
}

static int do_start(struct impl *this)
{
	bool do_accept;
	int res;

	if (this->started)
		return 0;

	spa_return_val_if_fail(this->transport, -EIO);

	this->following = is_following(this);

	this->start_ready = true;

	spa_log_debug(this->log, "%p: start following:%d", this, this->following);

	/* Do accept if Gateway; otherwise do connect for Head Unit */
	do_accept = this->transport->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY;

	/* acquire the socket fd (false -> connect | true -> accept) */
	if ((res = spa_bt_transport_acquire(this->transport, do_accept)) < 0) {
		this->start_ready = false;
		return res;
	}

	/* Start timer */
	this->timer_source.data = this;
	this->timer_source.fd = this->timerfd;
	this->timer_source.func = sco_on_timeout;
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->timer_source);

	setup_matching(this);

	set_timers(this);

	this->started = true;

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

static int do_remove_transport_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;

	this->transport_started = false;

	if (this->transport && this->transport->sco_io)
		spa_bt_sco_io_set_source_cb(this->transport->sco_io, NULL, NULL);

	return 0;
}

static void transport_stop(struct impl *this)
{
	struct port *port = &this->port;

	if (!this->transport_started)
		return;

	spa_log_debug(this->log, "sco-source %p: transport stop", this);

	spa_loop_invoke(this->data_loop, do_remove_transport_source, 0, NULL, 0, true, this);

	spa_bt_decode_buffer_clear(&port->buffer);
}

static int do_stop(struct impl *this)
{
	int res;

	if (!this->started)
		return 0;

	spa_log_debug(this->log, "%p: stop", this);

	this->start_ready = false;

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, this);

	transport_stop(this);

	if (this->transport)
		res = spa_bt_transport_release(this->transport);
	else
		res = 0;

	this->started = false;

	return res;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	struct port *port;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	port = &this->port;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!port->have_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;

		if ((res = do_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		if ((res = do_stop(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full)
{
	static const struct spa_dict_item hu_node_info_items[] = {
		{ SPA_KEY_DEVICE_API, "bluez5" },
		{ SPA_KEY_MEDIA_CLASS, "Audio/Source" },
		{ SPA_KEY_NODE_DRIVER, "true" },
	};
	const struct spa_dict_item ag_node_info_items[] = {
		{ SPA_KEY_DEVICE_API, "bluez5" },
		{ SPA_KEY_MEDIA_CLASS, "Stream/Output/Audio" },
		{ "media.name", ((this->transport && this->transport->device->name) ?
					this->transport->device->name : "HSP/HFP") },
		{ SPA_KEY_MEDIA_ROLE, "Communication" },
	};
	bool is_ag = this->transport && (this->transport->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY);
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = is_ag ?
			&SPA_DICT_INIT_ARRAY(ag_node_info_items) :
			&SPA_DICT_INIT_ARRAY(hu_node_info_items);
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
		spa_node_emit_port_info(&this->hooks,
				SPA_DIRECTION_OUTPUT, 0, &port->info);
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

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, &this->port, true);

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
	port = &this->port;

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (result.index > 0)
			return 0;
		if (this->transport == NULL)
			return -EIO;

		/* set the info structure */
		struct spa_audio_info_raw info = { 0, };
		info.format = SPA_AUDIO_FORMAT_S16_LE;
		info.channels = 1;
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;

		 /* CVSD format has a rate of 8kHz
		  * MSBC format has a rate of 16kHz */
		if (this->transport->codec == HFP_AUDIO_CODEC_MSBC)
			info.rate = 16000;
		else
			info.rate = 8000;

		/* build the param */
		param = spa_format_audio_raw_build(&b, id, &info);
		break;

	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &port->current_format.info.raw);
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
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							this->quantum_limit * port->frame_size,
							16 * port->frame_size,
							INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->frame_size));
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
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_RateMatch),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_rate_match)));
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

static int clear_buffers(struct impl *this, struct port *port)
{
	do_stop(this);
	if (port->n_buffers > 0) {
		spa_list_init(&port->free);
		spa_list_init(&port->ready);
		port->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(struct impl *this, struct port *port,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	int err;

	if (format == NULL) {
		spa_log_debug(this->log, "clear format");
		clear_buffers(this, port);
		port->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if (info.info.raw.format != SPA_AUDIO_FORMAT_S16_LE ||
		    info.info.raw.rate == 0 ||
		    info.info.raw.channels != 1)
			return -EINVAL;

		port->frame_size = info.info.raw.channels * 2;
		port->current_format = info;
		port->have_format = true;
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
		port->info.flags = SPA_PORT_FLAG_LIVE;
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
		port->info.rate = SPA_FRACTION(1, port->current_format.info.raw.rate);
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
		port->params[IDX_Latency].flags ^= SPA_PARAM_INFO_SERIAL;
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

	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);
	port = &this->port;

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
	port = &this->port;

	spa_log_debug(this->log, "use buffers %d", n_buffers);

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

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
		spa_list_append(&port->free, &b->link);
		b->outstanding = false;
	}
	port->n_buffers = n_buffers;

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
	port = &this->port;

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	case SPA_IO_RateMatch:
		port->rate_match = data;
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

	spa_return_val_if_fail(port_id == 0, -EINVAL);
	port = &this->port;

	if (port->n_buffers == 0)
		return -EIO;

	if (buffer_id >= port->n_buffers)
		return -EINVAL;

	recycle_buffer(this, port, buffer_id);

	return 0;
}

static uint32_t get_samples(struct impl *this, uint32_t *result_duration)
{
	struct port *port = &this->port;
	uint32_t samples, rate_denom;
	uint64_t duration;

	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.duration;
		rate_denom = this->position->clock.rate.denom;
	} else {
		duration = 1024;
		rate_denom = port->current_format.info.raw.rate;
	}

	*result_duration = duration * port->current_format.info.raw.rate / rate_denom;

	if (SPA_LIKELY(port->rate_match) && this->resampling)
		samples = port->rate_match->size;
	else
		samples = *result_duration;

	return samples;
}

#define WARN_ONCE(cond, ...) \
	if (SPA_UNLIKELY(cond)) { static bool __once; if (!__once) { __once = true; spa_log_warn(__VA_ARGS__); } }

static void process_buffering(struct impl *this)
{
	struct port *port = &this->port;
	uint32_t duration;
	const uint32_t samples = get_samples(this, &duration);
	void *buf;
	uint32_t avail;

	spa_bt_decode_buffer_process(&port->buffer, samples, duration);

	setup_matching(this);

	buf = spa_bt_decode_buffer_get_read(&port->buffer, &avail);

	/* copy data to buffers */
	if (!spa_list_is_empty(&port->free)) {
		struct buffer *buffer;
		struct spa_data *datas;
		uint32_t data_size;

		buffer = spa_list_first(&port->free, struct buffer, link);
		datas = buffer->buf->datas;

		data_size = samples * port->frame_size;

		WARN_ONCE(datas[0].maxsize < data_size && !this->following,
				this->log, "source buffer too small (%u < %u)",
				datas[0].maxsize, data_size);

		data_size = SPA_MIN(data_size, SPA_ROUND_DOWN(datas[0].maxsize, port->frame_size));

		avail = SPA_MIN(avail, data_size);

		spa_bt_decode_buffer_read(&port->buffer, avail);

		spa_list_remove(&buffer->link);

		spa_log_trace(this->log, "dequeue %d", buffer->id);

		datas[0].chunk->offset = 0;
		datas[0].chunk->size = data_size;
		datas[0].chunk->stride = port->frame_size;

		memcpy(datas[0].data, buf, avail);

		/* pad with silence */
		if (avail < data_size)
			memset(SPA_PTROFF(datas[0].data, avail, void), 0, data_size - avail);

		/* ready buffer if full */
		spa_log_trace(this->log, "queue %d frames:%d", buffer->id, (int)samples);
		spa_list_append(&port->ready, &buffer->link);
	}
}

static int produce_buffer(struct impl *this)
{
	struct buffer *buffer;
	struct port *port = &this->port;
	struct spa_io_buffers *io = port->io;

	if (io == NULL)
		return -EIO;

	/* Return if we already have a buffer */
	if (io->status == SPA_STATUS_HAVE_DATA &&
			(this->following || port->rate_match == NULL))
		return SPA_STATUS_HAVE_DATA;

	/* Recycle */
	if (io->buffer_id < port->n_buffers) {
		recycle_buffer(this, port, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	if (this->io_error) {
		io->status = -EIO;
		return SPA_STATUS_STOPPED;
	}

	/* Handle buffering */
	if (this->transport_started)
		process_buffering(this);

	/* Return if there are no buffers ready to be processed */
	if (spa_list_is_empty(&port->ready))
		return SPA_STATUS_OK;

	/* Get the new buffer from the ready list */
	buffer = spa_list_first(&port->ready, struct buffer, link);
	spa_list_remove(&buffer->link);
	buffer->outstanding = true;

	/* Set the new buffer in IO */
	io->buffer_id = buffer->id;
	io->status = SPA_STATUS_HAVE_DATA;

	/* Notify we have a buffer ready to be processed */
	return SPA_STATUS_HAVE_DATA;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *port;
	struct spa_io_buffers *io;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	port = &this->port;
	if ((io = port->io) == NULL)
		return -EIO;

	if (!this->started || !this->transport_started)
		return SPA_STATUS_OK;

	spa_log_trace(this->log, "%p status:%d", this, io->status);

	/* Return if we already have a buffer */
	if (io->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;

	/* Recycle */
	if (io->buffer_id < port->n_buffers) {
		recycle_buffer(this, port, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	/* Follower produces buffers here, driver in timeout */
	if (this->following)
		return produce_buffer(this);
	else
		return SPA_STATUS_OK;
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

static void transport_state_changed(void *data,
	enum spa_bt_transport_state old,
	enum spa_bt_transport_state state)
{
	struct impl *this = data;

	spa_log_debug(this->log, "%p: transport %p state %d->%d", this, this->transport, old, state);

	if (state == SPA_BT_TRANSPORT_STATE_ACTIVE)
		transport_start(this);
	else if (state < SPA_BT_TRANSPORT_STATE_ACTIVE)
		transport_stop(this);

	if (state == SPA_BT_TRANSPORT_STATE_ERROR) {
		uint8_t buffer[1024];
		struct spa_pod_builder b = { 0 };

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		spa_node_emit_event(&this->hooks,
				spa_pod_builder_add_object(&b,
						SPA_TYPE_EVENT_Node, SPA_NODE_EVENT_Error));
	}
}

static int do_transport_destroy(struct spa_loop *loop,
				bool async,
				uint32_t seq,
				const void *data,
				size_t size,
				void *user_data)
{
	struct impl *this = user_data;
	this->transport = NULL;
	return 0;
}

static void transport_destroy(void *data)
{
	struct impl *this = data;
	spa_log_debug(this->log, "transport %p destroy", this->transport);
	spa_loop_invoke(this->data_loop, do_transport_destroy, 0, NULL, 0, true, this);
}

static const struct spa_bt_transport_events transport_events = {
	SPA_VERSION_BT_TRANSPORT_EVENTS,
	.state_changed = transport_state_changed,
	.destroy = transport_destroy,
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
	if (this->transport)
		spa_hook_remove(&this->transport_listener);
	spa_system_close(this->data_system, this->timerfd);
	spa_bt_decode_buffer_clear(&this->port.buffer);
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
	struct port *port;
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);

	spa_log_topic_init(this->log, &log_topic);

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data system is needed");
		return -EINVAL;
	}

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	/* set the node info */
	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_NODE_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	/* set the port info */
	port = &this->port;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS;
	port->info.flags = SPA_PORT_FLAG_LIVE |
			   SPA_PORT_FLAG_TERMINAL;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;

	port->latency = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);
	port->latency.min_quantum = 1.0f;
	port->latency.max_quantum = 1.0f;

	/* Init the buffer lists */
	spa_list_init(&port->ready);
	spa_list_init(&port->free);

	this->quantum_limit = 8192;
	if (info && (str = spa_dict_lookup(info, "clock.quantum-limit")))
		spa_atou32(str, &this->quantum_limit, 0);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_TRANSPORT)))
		sscanf(str, "pointer:%p", &this->transport);

	if (this->transport == NULL) {
		spa_log_error(this->log, "a transport is needed");
		return -EINVAL;
	}
	spa_bt_transport_add_listener(this->transport,
			&this->transport_listener, &transport_events, this);

	this->timerfd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	return 0;
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
	{ SPA_KEY_FACTORY_AUTHOR, "Collabora Ltd. <contact@collabora.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Capture bluetooth audio with hsp/hfp" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_TRANSPORT"=<transport>" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_sco_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_SCO_SOURCE,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
