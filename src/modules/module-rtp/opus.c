/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifdef HAVE_OPUS

#include <opus/opus.h>
#include <opus/opus_multistream.h>

/* TODO: Direct timestamp mode here may require a rework. See audio.c for a reference.
 * Also check out the usage of actual_max_buffer_size in audio.c. */

static void rtp_opus_process_playback(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t wanted, timestamp, target_buffer, stride, maxsize;
	int32_t avail;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_info("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	stride = impl->stride;

	maxsize = d[0].maxsize / stride;
	wanted = buf->requested ? SPA_MIN(buf->requested, maxsize) : maxsize;

	if (impl->io_position && impl->direct_timestamp) {
		/* in direct mode, read directly from the timestamp index,
		 * because sender and receiver are in sync, this would keep
		 * target_buffer of samples available. */
		spa_ringbuffer_read_update(&impl->ring,
				impl->io_position->clock.position);
	}
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
		double error, corr;
		if (impl->first) {
			if ((uint32_t)avail > target_buffer) {
				uint32_t skip = avail - target_buffer;
				pw_log_debug("first: avail:%d skip:%u target:%u",
							avail, skip, target_buffer);
				timestamp += skip;
				avail = target_buffer;
			}
			impl->first = false;
		} else if (avail > (int32_t)SPA_MIN(target_buffer * 8, impl->buffer_size2 / stride)) {
			pw_log_warn("overrun %u > %u", avail, target_buffer * 8);
			timestamp += avail - target_buffer;
			avail = target_buffer;
		}
		if (!impl->direct_timestamp) {
			/* when not using direct timestamp and clocks are not
			 * in sync, try to adjust our playback rate to keep the
			 * requested target_buffer bytes in the ringbuffer */
			error = (double)target_buffer - (double)avail;
			error = SPA_CLAMPD(error, -impl->max_error, impl->max_error);

			corr = spa_dll_update(&impl->dll, error);

			pw_log_trace("avail:%u target:%u error:%f corr:%f", avail,
					target_buffer, error, corr);

			pw_stream_set_rate(impl->stream, 1.0 / corr);
		}
		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer,
				impl->buffer_size2,
				(timestamp * stride) & impl->buffer_mask2,
				d[0].data, wanted * stride);

		timestamp += wanted;
		spa_ringbuffer_read_update(&impl->ring, timestamp);
	}
	d[0].chunk->offset = 0;
	d[0].chunk->size = wanted * stride;
	d[0].chunk->stride = stride;
	d[0].chunk->flags = 0;
	buf->size = wanted;

	pw_stream_queue_buffer(impl->stream, buf);
}

static int rtp_opus_receive(struct impl *impl, uint8_t *buffer, ssize_t len,
			ssize_t hlen, uint64_t current_time)
{
	struct rtp_header *hdr;
	ssize_t plen;
	uint16_t seq;
	uint32_t timestamp, samples, write, expected_write;
	uint32_t stride = impl->stride;
	OpusMSDecoder *dec = impl->stream_data;
	int32_t filled;
	int res;

	hdr = (struct rtp_header*)buffer;

	seq = ntohs(hdr->sequence_number);
	if (impl->have_seq && impl->seq != seq) {
		pw_log_info("unexpected seq (%d != %d) SSRC:%u",
				seq, impl->seq, impl->ssrc);
		impl->have_sync = false;
	}
	impl->seq = seq + 1;
	impl->have_seq = true;

	timestamp = ntohl(hdr->timestamp) - impl->ts_offset;

	impl->receiving = true;

	plen = len - hlen;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_write);

	/* we always write to timestamp + delay */
	write = timestamp + impl->target_buffer;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u target:%u direct:%u",
				timestamp, seq, impl->ts_offset, impl->ssrc,
				impl->target_buffer, impl->direct_timestamp);

		/* we read from timestamp, keeping target_buffer of data
		 * in the ringbuffer. */
		impl->ring.readindex = timestamp;
		impl->ring.writeindex = write;
		filled = impl->target_buffer;

		spa_dll_init(&impl->dll);
		spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);
		memset(impl->buffer, 0, impl->buffer_size);
		impl->have_sync = true;
	} else if (expected_write != write) {
		pw_log_debug("unexpected write (%u != %u)",
				write, expected_write);
	}

	if (filled + 2880 > (int32_t)(impl->buffer_size2 / stride)) {
		pw_log_debug("capture overrun %u + %d > %u", filled, 2880,
				impl->buffer_size2 / stride);
		impl->have_sync = false;
	} else {
		uint32_t index = (write * stride) & impl->buffer_mask2, end;

		res = opus_multistream_decode_float(dec,
				&buffer[hlen], plen,
				(float*)&impl->buffer[index], 2880,
				0);

		end = index + (res * stride);
		/* fold to the lower part of the ringbuffer when overflow */
		if (end > impl->buffer_size2)
			memmove(impl->buffer, &impl->buffer[impl->buffer_size2], end - impl->buffer_size2);

		pw_log_info("receiving %zd len:%d timestamp:%d %u", plen, res, timestamp, index);
		samples = res;

		write += samples;
		spa_ringbuffer_write_update(&impl->ring, write);
	}
	return 0;
}

static void rtp_opus_flush_packets(struct impl *impl)
{
	int32_t avail, tosend;
	uint32_t stride, timestamp, offset;
	uint8_t out[1280];
	struct iovec iov[2];
	struct rtp_header header;
	OpusMSEncoder *enc = impl->stream_data;
	int res = 0;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);
	tosend = impl->psamples;

	if (avail < tosend)
		return;

	stride = impl->stride;

	spa_zero(header);
	header.v = 2;
	header.pt = impl->payload;
	header.ssrc = htonl(impl->ssrc);

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);
	iov[1].iov_base = out;
	iov[1].iov_len = 0;

	offset = 0;
	while (avail >= tosend) {
		header.sequence_number = htons(impl->seq);
		header.timestamp = htonl(impl->ts_offset + timestamp);

		res = opus_multistream_encode_float(enc,
				(const float*)&impl->buffer[offset * stride], tosend,
				out, sizeof(out));

		pw_log_trace("sending %d len:%d timestamp:%d", tosend, res, timestamp);
		iov[1].iov_len = res;

		rtp_stream_call_send_packet(impl, iov, 2);

		impl->seq++;
		timestamp += tosend;
		offset += tosend;
		avail -= tosend;
	}

	pw_log_trace("move %d offset:%d", avail, offset);
	memmove(impl->buffer, &impl->buffer[offset * stride], avail * stride);

	spa_ringbuffer_read_update(&impl->ring, timestamp);
}

static void rtp_opus_process_capture(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t offs, size, timestamp, expected_timestamp, stride;
	int32_t filled, wanted;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_info("Out of stream buffers: %m");
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
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u",
				timestamp, impl->seq, impl->ts_offset, impl->ssrc);
		impl->ring.readindex = impl->ring.writeindex = expected_timestamp = timestamp;
		memset(impl->buffer, 0, impl->buffer_size);
		impl->have_sync = true;
	} else {
		if (SPA_ABS((int32_t)expected_timestamp - (int32_t)timestamp) > 32) {
			pw_log_warn("expected %u != timestamp %u", expected_timestamp, timestamp);
			impl->have_sync = false;
		} else if (filled + wanted > (int32_t)(impl->buffer_size / stride)) {
			pw_log_warn("overrun %u + %u > %u", filled, wanted, impl->buffer_size / stride);
			impl->have_sync = false;
		}
	}

	spa_ringbuffer_write_data(&impl->ring,
			impl->buffer,
			impl->buffer_size,
			(filled * stride) & impl->buffer_mask,
			SPA_PTROFF(d[0].data, offs, void), wanted * stride);
	expected_timestamp += wanted;
	spa_ringbuffer_write_update(&impl->ring, expected_timestamp);

	pw_stream_queue_buffer(impl->stream, buf);

	rtp_opus_flush_packets(impl);
}

static void rtp_opus_deinit(struct impl *impl, enum spa_direction direction)
{
	if (impl->stream_data) {
		if (direction == SPA_DIRECTION_INPUT)
			opus_multistream_encoder_destroy(impl->stream_data);
		else
			opus_multistream_decoder_destroy(impl->stream_data);
	}
}

static int rtp_opus_init(struct impl *impl, enum spa_direction direction)
{
	int err;
	unsigned char mapping[255];
	uint32_t i;

	if (impl->info.info.opus.channels > 255)
		return -EINVAL;

	if (impl->psamples >= 2880)
		impl->psamples = 2880;
	else if (impl->psamples >= 1920)
		impl->psamples = 1920;
	else if (impl->psamples >= 960)
		impl->psamples = 960;
	else if (impl->psamples >= 480)
		impl->psamples = 480;
	else if (impl->psamples >= 240)
		impl->psamples = 240;
	else
		impl->psamples = 120;

	for (i = 0; i < impl->info.info.opus.channels; i++)
		mapping[i] = i;

	impl->deinit = rtp_opus_deinit;
	impl->receive_rtp = rtp_opus_receive;
	if (direction == SPA_DIRECTION_INPUT) {
		impl->stream_events.process = rtp_opus_process_capture;

		impl->stream_data = opus_multistream_encoder_create(
			impl->info.info.opus.rate,
			impl->info.info.opus.channels,
			impl->info.info.opus.channels, 0,
			mapping,
			OPUS_APPLICATION_AUDIO,
			&err);
	}
	else {
		impl->stream_events.process = rtp_opus_process_playback;

		impl->stream_data = opus_multistream_decoder_create(
			impl->info.info.opus.rate,
			impl->info.info.opus.channels,
			impl->info.info.opus.channels, 0,
			mapping,
			&err);
	}
	if (!impl->stream_data)
		pw_log_error("opus error: %d", err);
	return impl->stream_data ? 0 : err;
}
#else
static int rtp_opus_init(struct impl *impl, enum spa_direction direction)
{
	return -ENOTSUP;
}
#endif
