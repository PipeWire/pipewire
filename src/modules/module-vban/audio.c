/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

static void vban_audio_process_playback(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t wanted, timestamp, target_buffer, stride, maxsize;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	stride = impl->stride;

	maxsize = d[0].maxsize / stride;
	wanted = buf->requested ? SPA_MIN(buf->requested, maxsize) : maxsize;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);

	target_buffer = impl->target_buffer;

	if (avail < (int32_t)wanted) {
		enum spa_log_level level;
		memset(d[0].data, 0, wanted * stride);
		if (impl->have_sync) {
			impl->have_sync = false;
			level = SPA_LOG_LEVEL_WARN;
		} else {
			level = SPA_LOG_LEVEL_DEBUG;
		}
		pw_log(level, "underrun %d/%u < %u",
					avail, target_buffer, wanted);
	} else {
		float error, corr;
		if (impl->first) {
			if ((uint32_t)avail > target_buffer) {
				uint32_t skip = avail - target_buffer;
				pw_log_debug("first: avail:%d skip:%u target:%u",
							avail, skip, target_buffer);
				timestamp += skip;
				avail = target_buffer;
			}
			impl->first = false;
		} else if (avail > (int32_t)SPA_MIN(target_buffer * 8, BUFFER_SIZE / stride)) {
			pw_log_warn("overrun %u > %u", avail, target_buffer * 8);
			timestamp += avail - target_buffer;
			avail = target_buffer;
		}
		/* try to adjust our playback rate to keep the
		 * requested target_buffer bytes in the ringbuffer */
		error = (float)target_buffer - (float)avail;
		error = SPA_CLAMP(error, -impl->max_error, impl->max_error);

		corr = spa_dll_update(&impl->dll, error);

		pw_log_debug("avail:%u target:%u error:%f corr:%f", avail,
				target_buffer, error, corr);

		if (impl->io_rate_match) {
			SPA_FLAG_SET(impl->io_rate_match->flags,
					SPA_IO_RATE_MATCH_FLAG_ACTIVE);
			impl->io_rate_match->rate = 1.0f / corr;
		}
		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(timestamp * stride) & BUFFER_MASK,
				d[0].data, wanted * stride);

		timestamp += wanted;
		spa_ringbuffer_read_update(&impl->ring, timestamp);
	}
	d[0].chunk->size = wanted * stride;
	d[0].chunk->stride = stride;
	d[0].chunk->offset = 0;
	buf->size = wanted;

	pw_stream_queue_buffer(impl->stream, buf);
}

static int vban_audio_receive(struct impl *impl, uint8_t *buffer, ssize_t len)
{
	struct vban_header *hdr;
	ssize_t hlen, plen;
	uint32_t n_frames, timestamp, samples, write, expected_write;
	uint32_t stride = impl->stride;
	int32_t filled;

	if (len < VBAN_HEADER_SIZE)
		goto short_packet;

	hdr = (struct vban_header*)buffer;
	if (strncmp(hdr->vban, "VBAN", 3))
		goto invalid_version;

	impl->receiving = true;

	hlen = VBAN_HEADER_SIZE;
	plen = len - hlen;
	samples = SPA_MIN(hdr->format_nbs+1, plen / stride);

	n_frames = hdr->n_frames;
	if (impl->have_sync && impl->n_frames != n_frames) {
		pw_log_info("unexpected frame (%d != %d)",
				n_frames, impl->n_frames);
		impl->have_sync = false;
	}
	impl->n_frames = n_frames + 1;

	timestamp = impl->timestamp;
	impl->timestamp += samples;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_write);

	/* we always write to timestamp + delay */
	write = timestamp + impl->target_buffer;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u target:%u",
				timestamp, impl->target_buffer);

		/* we read from timestamp, keeping target_buffer of data
		 * in the ringbuffer. */
		impl->ring.readindex = timestamp;
		impl->ring.writeindex = write;
		filled = impl->target_buffer;

		spa_dll_init(&impl->dll);
		spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MAX, 128, impl->rate);
		memset(impl->buffer, 0, BUFFER_SIZE);
		impl->have_sync = true;
	} else if (expected_write != write) {
		pw_log_debug("unexpected write (%u != %u)",
				write, expected_write);
	}

	if (filled + samples > BUFFER_SIZE / stride) {
		pw_log_debug("capture overrun %u + %u > %u", filled, samples,
				BUFFER_SIZE / stride);
		impl->have_sync = false;
	} else {
		pw_log_trace("got samples:%u", samples);
		spa_ringbuffer_write_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(write * stride) & BUFFER_MASK,
				&buffer[hlen], (samples * stride));
		write += samples;
		spa_ringbuffer_write_update(&impl->ring, write);
	}
	return 0;

short_packet:
	pw_log_warn("short packet received");
	return -EINVAL;
invalid_version:
	pw_log_warn("invalid VBAN version");
	spa_debug_mem(0, buffer, len);
	return -EPROTO;
}

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
}

static void vban_audio_flush_packets(struct impl *impl)
{
	int32_t avail, tosend;
	uint32_t stride, timestamp;
	struct iovec iov[3];
	struct vban_header header;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);
	tosend = impl->psamples;

	if (avail < tosend)
		return;

	stride = impl->stride;

	header = impl->header;
	header.format_nbs = tosend - 1;
	header.format_nbc = impl->stream_info.info.raw.channels - 1;

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);

	while (avail >= tosend) {
		set_iovec(&impl->ring,
			impl->buffer, BUFFER_SIZE,
			(timestamp * stride) & BUFFER_MASK,
			&iov[1], tosend * stride);

		pw_log_trace("sending %d timestamp:%08x", tosend, timestamp);

		vban_stream_emit_send_packet(impl, iov, 3);

		timestamp += tosend;
		avail -= tosend;
		header.n_frames++;
	}
	impl->header.n_frames = header.n_frames;
	spa_ringbuffer_read_update(&impl->ring, timestamp);
}

static void vban_audio_process_capture(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t offs, size, timestamp, expected_timestamp, stride;
	int32_t filled, wanted;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
	stride = impl->stride;
	wanted = size / stride;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_timestamp);

	if (SPA_LIKELY(impl->io_position)) {
		uint32_t rate = impl->io_position->clock.rate.denom;
		timestamp = impl->io_position->clock.position * impl->rate / rate;
	} else
		timestamp = expected_timestamp;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u", timestamp);
		impl->ring.readindex = impl->ring.writeindex = timestamp;
		memset(impl->buffer, 0, BUFFER_SIZE);
		impl->have_sync = true;
		expected_timestamp = timestamp;
	} else {
		if (SPA_ABS((int32_t)expected_timestamp - (int32_t)timestamp) > 32) {
			pw_log_warn("expected %u != timestamp %u", expected_timestamp, timestamp);
			impl->have_sync = false;
		} else if (filled + wanted > (int32_t)(BUFFER_SIZE / stride)) {
			pw_log_warn("overrun %u + %u > %u", filled, wanted, BUFFER_SIZE / stride);
			impl->have_sync = false;
		}
	}

	spa_ringbuffer_write_data(&impl->ring,
			impl->buffer,
			BUFFER_SIZE,
			(expected_timestamp * stride) & BUFFER_MASK,
			SPA_PTROFF(d[0].data, offs, void), wanted * stride);
	expected_timestamp += wanted;
	spa_ringbuffer_write_update(&impl->ring, expected_timestamp);

	pw_stream_queue_buffer(impl->stream, buf);

	vban_audio_flush_packets(impl);
}

static int vban_audio_init(struct impl *impl, enum spa_direction direction)
{
	if (direction == SPA_DIRECTION_INPUT)
		impl->stream_events.process = vban_audio_process_capture;
	else
		impl->stream_events.process = vban_audio_process_playback;
	impl->receive_vban = vban_audio_receive;
	return 0;
}
