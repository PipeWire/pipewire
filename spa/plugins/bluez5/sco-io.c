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


struct spa_bt_sco_io {
	bool started;

	uint8_t read_buffer[MAX_MTU];
	uint32_t read_size;

	int fd;
	uint16_t read_mtu;
	uint16_t write_mtu;

	struct spa_loop *data_loop;
	struct spa_source source;

	int (*source_cb)(void *userdata, uint8_t *data, int size);
	void *source_userdata;

	int (*sink_cb)(void *userdata);
	void *sink_userdata;
};


static void update_source(struct spa_bt_sco_io *io)
{
	int enabled;
	int changed = 0;

	enabled = io->sink_cb != NULL;
	if (SPA_FLAG_IS_SET(io->source.mask, SPA_IO_OUT) != enabled) {
		SPA_FLAG_UPDATE(io->source.mask, SPA_IO_OUT, enabled);
		changed = 1;
	}

	if (changed) {
		spa_loop_update_source(io->data_loop, &io->source);
	}
}

static void sco_io_on_ready(struct spa_source *source)
{
	struct spa_bt_sco_io *io = source->data;

	if (SPA_FLAG_IS_SET(source->rmask, SPA_IO_IN)) {
		int res;

	read_again:
		res = recv(io->fd, io->read_buffer, SPA_MIN(io->read_mtu, MAX_MTU), MSG_DONTWAIT);
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

		io->read_size = res;

		if (io->source_cb) {
			int res;
			res = io->source_cb(io->source_userdata, io->read_buffer, io->read_size);
			if (res) {
				io->source_cb = NULL;
			}
		}
	}

read_done:
	if (SPA_FLAG_IS_SET(source->rmask, SPA_IO_OUT)) {
		if (io->sink_cb) {
			int res;
			res = io->sink_cb(io->sink_userdata);
			if (res) {
				io->sink_cb = NULL;
			}
		}
	}

	if (SPA_FLAG_IS_SET(source->rmask, SPA_IO_ERR) || SPA_FLAG_IS_SET(source->rmask, SPA_IO_HUP)) {
		goto stop;
	}

	/* Poll socket in/out only if necessary */
	update_source(io);

	return;

stop:
	if (io->source.loop) {
		spa_loop_remove_source(io->data_loop, &io->source);
		io->started = false;
	}
}

/*
 * Write data to socket in correctly sized blocks.
 * Returns the number of bytes written, 0 when data cannot be written now or
 * there is too little of it to write, and <0 on write error.
 */
int spa_bt_sco_io_write(struct spa_bt_sco_io *io, uint8_t *buf, int size)
{
	uint16_t packet_size;
	uint8_t *buf_start = buf;

	if (io->read_size == 0) {
		/* The proper write packet size is not known yet */
		return 0;
	}

	packet_size = SPA_MIN(io->write_mtu, io->read_size);

	if (size < packet_size) {
		return 0;
	}

	do {
		int written;

		written = send(io->fd, buf, packet_size, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (written < 0) {
			if (errno == EINTR) {
				/* retry if interrupted */
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* Don't continue writing */
				break;
			}
			return -errno;
		}

		buf += written;
		size -= written;
	} while (size >= packet_size);

	return buf - buf_start;
}


struct spa_bt_sco_io *spa_bt_sco_io_create(struct spa_loop *data_loop,
                                           int fd,
                                           uint16_t read_mtu,
                                           uint16_t write_mtu)
{
	struct spa_bt_sco_io *io;

	io = calloc(1, sizeof(struct spa_bt_sco_io));
	if (io == NULL)
		return io;

	io->fd = fd;
	io->read_mtu = read_mtu;
	io->write_mtu = write_mtu;
	io->data_loop = data_loop;

	io->read_size = 0;

	/* Add the ready callback */
	io->source.data = io;
	io->source.fd = io->fd;
	io->source.func = sco_io_on_ready;
	io->source.mask = SPA_IO_IN | SPA_IO_OUT | SPA_IO_ERR | SPA_IO_HUP;
	io->source.rmask = 0;
	spa_loop_add_source(io->data_loop, &io->source);

	io->started = true;

	return io;
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
	if (io->started)
		spa_loop_invoke(io->data_loop, do_remove_source, 0, NULL, 0, true, io);

	io->started = false;
	free(io);
}

/* Set source callback.
 * This function should only be called from the data thread.
 * Callback is called (in data loop) with data just read from the socket.
 */
void spa_bt_sco_io_set_source_cb(struct spa_bt_sco_io *io, int (*source_cb)(void *, uint8_t *, int), void *userdata)
{
	io->source_cb = source_cb;
	io->source_userdata = userdata;

	if (io->started) {
		update_source(io);
	}
}

/* Set sink callback.
 * This function should only be called from the data thread.
 * Callback is called (in data loop) when socket can be written to.
 */
void spa_bt_sco_io_set_sink_cb(struct spa_bt_sco_io *io, int (*sink_cb)(void *), void *userdata)
{
	io->sink_cb = sink_cb;
	io->sink_userdata = userdata;

	if (io->started) {
		update_source(io);
	}
}
