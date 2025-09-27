/* Spa SCO I/O */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/cleanup.h>
#include <spa/monitor/device.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/param/param.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include <sbc/sbc.h>

#include "defs.h"
#include "media-codecs.h"
#include "hfp-codec-caps.h"

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.bluez5.sco-io");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#include "decode-buffer.h"


/* We'll use the read rx data size to find the correct packet size for writing,
 * since kernel might not report it as the socket MTU, see
 * https://lore.kernel.org/linux-bluetooth/20201210003528.3pmaxvubiwegxmhl@pali/T/
 *
 * We continue reading also when there's no source connected, to keep socket
 * flushed.
 *
 * XXX: when the kernel/backends start giving the right values, the heuristic
 * XXX: can be removed
 */
#define MAX_MTU 1024

#define KEEPALIVE_NSEC	(500 * SPA_NSEC_PER_MSEC)


struct spa_bt_sco_io {
	uint8_t read_buffer[MAX_MTU];
	size_t read_size;

	uint8_t write_buffer[MAX_MTU];
	size_t write_size;

	int fd;
	uint16_t read_mtu;
	uint16_t write_mtu;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;
	struct spa_source source;

	struct spa_bt_recvmsg_data recv;

	int (*source_cb)(void *userdata, uint8_t *data, int size, uint64_t rx_time);
	void *source_userdata;

	const struct media_codec *codec;
	void *codec_data;

	uint64_t last_tx_time;
	uint64_t last_rx_time;
	uint16_t keepalive_seqnum;
	bool keepalive;
};

static void keepalive_send(struct spa_bt_sco_io *io)
{
	static const uint8_t zeros[2048];
	uint8_t buf[MAX_MTU];
	int res, need_flush = 0;
	size_t size;

	if (io->codec->id == SPA_BLUETOOTH_AUDIO_CODEC_CVSD) {
		/* Doesn't have fixed block size; TX same amount as RX instead */
		size = SPA_MIN(sizeof(buf), io->read_size);
		memset(buf, 0, size);
		goto send;
	}

	size = res = io->codec->start_encode(io->codec_data, buf, sizeof(buf), ++io->keepalive_seqnum,
			io->last_rx_time / SPA_NSEC_PER_USEC);
	if (res < 0)
		return;

	do {
		size_t encoded;

		res = io->codec->encode(io->codec_data, zeros, sizeof(zeros),
				SPA_PTROFF(buf, size, void), sizeof(buf) - size,
				&encoded, &need_flush);
		if (res < 0)
			return;

		size += encoded;
		if (size >= sizeof(buf))
			return;
	} while (!need_flush);

send:
	if (!io->keepalive)
		spa_bt_sco_io_write_start(io);

	io->keepalive = false;
	spa_bt_sco_io_write(io, buf, size);
	io->keepalive = true;
}

static void sco_io_on_ready(struct spa_source *source)
{
	struct spa_bt_sco_io *io = source->data;

	if (SPA_FLAG_IS_SET(source->rmask, SPA_IO_IN)) {
		int res;
		int dummy;
		uint64_t rx_time = 0;

	read_again:
		res = spa_bt_recvmsg(&io->recv, io->read_buffer, SPA_MIN(io->read_mtu, MAX_MTU), &rx_time, &dummy);
		if (res <= 0) {
			if (errno == EINTR) {
				/* retry if interrupted */
				goto read_again;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* no data: try it next time */
				goto read_done;
			}

			/* error */
			goto stop;
		}

		if (res != (int)io->read_size) {
			spa_log_trace(io->log, "%p: packet size:%d", io, res);

			/* drop buffer when packet size changes */
			io->write_size = 0;
		}

		io->read_size = res;
		io->last_rx_time = rx_time;
		if (!io->last_tx_time)
			io->last_tx_time = rx_time;

		if (io->source_cb) {
			int res;
			res = io->source_cb(io->source_userdata, io->read_buffer, io->read_size, rx_time);
			if (res) {
				io->source_cb = NULL;
			}
		}

		/* If sink has not supplied packets for some time, for each RX packet send
		 * same amount of silence to keep the connection alive. Some devices (with
		 * LC3-24kHZ) require this and it doesn't hurt for others.
		 */
		if (io->last_tx_time + KEEPALIVE_NSEC < io->last_rx_time || io->keepalive)
			keepalive_send(io);
	}

read_done:
	if (SPA_FLAG_IS_SET(source->rmask, SPA_IO_ERR) || SPA_FLAG_IS_SET(source->rmask, SPA_IO_HUP)) {
		goto stop;
	}
	return;

stop:
	if (io->source.loop)
		spa_loop_remove_source(io->data_loop, &io->source);
}

static int write_packets(struct spa_bt_sco_io *io, const uint8_t **buf, size_t *size, size_t packet_size)
{
	while (*size >= packet_size) {
		ssize_t written;

		written = send(io->fd, *buf, packet_size, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}

		*buf += written;
		*size -= written;
	}
	return 0;
}

/*
 * Write data to socket in correctly sized blocks.
 * Returns the number of bytes written or buffered, and <0 on write error.
 */
int spa_bt_sco_io_write(struct spa_bt_sco_io *io, const uint8_t *buf, size_t size)
{
	const size_t orig_size = size;
	const uint8_t *pos;
	size_t packet_size;
	int res;

	io->last_tx_time = io->last_rx_time;

	if (io->read_size == 0) {
		/* The proper write packet size is not known yet */
		return 0;
	}

	if (io->keepalive) {
		/* Transition from keepalive to sink-fed data */
		io->write_size = 0;
		io->keepalive = false;
	}

	packet_size = SPA_MIN(SPA_MIN(io->write_mtu, io->read_size), sizeof(io->write_buffer));

	if (io->write_size >= packet_size) {
		/* packet size changed, drop data */
		io->write_size = 0;
	} else if (io->write_size) {
		/* write fragment */
		size_t need = SPA_MIN(packet_size - io->write_size, size);

		memcpy(io->write_buffer + io->write_size, buf, need);
		buf += need;
		size -= need;
		io->write_size += need;

		if (io->write_size < packet_size)
			return orig_size;

		pos = io->write_buffer;
		if ((res = write_packets(io, &pos, &io->write_size, packet_size)) < 0)
			goto fail;
		if (io->write_size)
			goto fail;
	}

	/* write */
	if ((res = write_packets(io, &buf, &size, packet_size)) < 0)
		goto fail;

	spa_assert(size < packet_size);

	/* store fragment */
	io->write_size = size;
	if (size)
		memcpy(io->write_buffer, buf, size);

	return orig_size;

fail:
	io->write_size = 0;
	return res;
}

void spa_bt_sco_io_write_start(struct spa_bt_sco_io *io)
{
	/* drop fragment */
	io->write_size = 0;
}

struct spa_bt_sco_io *spa_bt_sco_io_create(struct spa_bt_transport *transport, struct spa_loop *data_loop,
		struct spa_system *data_system, struct spa_log *log)
{
	spa_autofree struct spa_bt_sco_io *io = NULL;
	struct spa_audio_info format = { 0 };

	spa_log_topic_init(log, &log_topic);

	io = calloc(1, sizeof(struct spa_bt_sco_io));
	if (io == NULL)
		return io;

	io->fd = transport->fd;
	io->read_mtu = transport->read_mtu;
	io->write_mtu = transport->write_mtu;
	io->data_loop = data_loop;
	io->data_system = data_system;
	io->log = log;

	if (transport->device->adapter->bus_type == BUS_TYPE_USB) {
		/* For USB we need to wait for RX to know it. Using wrong size doesn't
		 * work anyway, and may result to errors printed to dmesg if too big.
		 */
		io->read_size = 0;
	} else {
		/* Set some sensible initial packet size */
		switch (transport->media_codec->id) {
		case SPA_BLUETOOTH_AUDIO_CODEC_CVSD:
			io->read_size = 48;  /* 3ms S16_LE 8000 Hz */
			break;
		default:
			io->read_size = HFP_H2_PACKET_SIZE;
			break;
		}
	}

	io->codec = transport->media_codec;

	if (io->codec->validate_config(io->codec, 0, NULL, 0, &format) < 0)
		return NULL;

	io->codec_data = io->codec->init(io->codec, 0, NULL, 0, &format, NULL, transport->write_mtu);
	if (!io->codec_data)
		return NULL;

	spa_log_debug(io->log, "%p: initial packet size:%d", io, (int)io->read_size);

	spa_bt_recvmsg_init(&io->recv, io->fd, io->data_system, io->log);

	/* Add the ready callback */
	io->source.data = io;
	io->source.fd = io->fd;
	io->source.func = sco_io_on_ready;
	io->source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	io->source.rmask = 0;
	spa_loop_add_source(io->data_loop, &io->source);

	return spa_steal_ptr(io);
}

static int do_remove_source(struct spa_loop *loop,
                            bool async,
                            uint32_t seq,
                            const void *data,
                            size_t size,
                            void *user_data)
{
	struct spa_bt_sco_io *io = user_data;

	if (io->source.loop)
		spa_loop_remove_source(io->data_loop, &io->source);

	return 0;
}

void spa_bt_sco_io_destroy(struct spa_bt_sco_io *io)
{
	spa_log_debug(io->log, "%p: destroy", io);
	spa_loop_locked(io->data_loop, do_remove_source, 0, NULL, 0, io);
	io->codec->deinit(io->codec_data);
	free(io);
}

/* Set source callback.
 * This function should only be called from the data thread.
 * Callback is called (in data loop) with data just read from the socket.
 */
void spa_bt_sco_io_set_source_cb(struct spa_bt_sco_io *io, int (*source_cb)(void *, uint8_t *, int, uint64_t), void *userdata)
{
	io->source_cb = source_cb;
	io->source_userdata = userdata;
	io->last_rx_time = 0;
	io->last_tx_time = 0;
}
