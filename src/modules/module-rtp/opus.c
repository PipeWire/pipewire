/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifdef HAVE_OPUS

#include <opus/opus.h>
#include <opus/opus_multistream.h>

/* read wanted samples from the packet buffer at timestamp. Fill the gaps with
 * silence */
static void opus_packet_buffer_read(struct impl *impl, uint32_t timestamp, void *dst,
		uint32_t wanted, uint32_t stride)
{
	struct rtp_packet *p;
	OpusMSDecoder *dec = impl->stream_data;

	spa_list_for_each(p, &impl->queued, link) {
		uint32_t samples, skip, ts, ts_end;
		int32_t ts_delta;
		if (wanted == 0)
			break;

		if (p->decoded == NULL) {
			int res;
			res = opus_multistream_decode_float(dec,
				SPA_PTROFF(p->data, p->hlen, void), p->size - p->hlen,
				(float*)p->tmp, p->tmp_size/sizeof(float), 0);
			if (res < 0) {
				pw_log_warn("opus decode error %d", res);
				continue;
			}
			p->decoded = p->tmp;
			p->decoded_len = res;
		}

		samples = p->decoded_len;
		ts = p->timestamp;
		ts_end = ts + samples;
		if (rtp_timestamp_delta(ts_end, timestamp) <= 0)
			continue;

		ts_delta = rtp_timestamp_delta(timestamp, ts);
		if (ts_delta < 0) {
			/* there is no packet that contains the requested
			 * timestamp, we underrun */
			skip = -ts_delta;
			skip = SPA_MIN(skip, wanted);
			if (impl->have_sync)
				pw_log_warn("underrun %d %u %u, packet buffer too small", skip, ts, timestamp);
			memset(dst, 0, skip * stride);
			dst = SPA_PTROFF(dst, skip * stride, void);
			wanted -= skip;
			timestamp += skip;
			skip = 0;
		} else {
			/* packet contains requested timestamp, skip samples
			 * before timestamp */
			skip = ts_delta;
			samples -= skip;
		}
		samples = SPA_MIN(samples, wanted);
		if (samples > 0) {
			memcpy(dst, SPA_PTROFF(p->decoded, skip*stride, void), samples * stride);
			dst = SPA_PTROFF(dst, samples * stride, void);
			wanted -= samples;
			timestamp += samples;
		}
	}
	if (wanted > 0) {
		/* we ran out of packets and we could not fill the complete
		 * buffer -> underrun */
		memset(dst, 0, wanted * stride);
		if (impl->have_sync)
			pw_log_warn("underrun");
	}
}

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
		timestamp = impl->io_position->clock.position;
	} else {
		timestamp = impl->expected_timestamp;
	}

	if (!impl->direct_timestamp) {
		double error, corr;

		target_buffer = impl->target_buffer;

		if (!impl->have_sync) {
			spa_dll_init(&impl->dll);
			spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);

			avail = (int32_t)(target_buffer);
			timestamp = (int32_t)(impl->tail_timestamp - avail);
			impl->expected_timestamp = timestamp;
			impl->have_sync = impl->num_queued != 0;
			error = 0.0;

			pw_log_info("sync:%d %08x %08x target:%u synced:%u", avail,
				impl->tail_timestamp, timestamp, target_buffer, impl->have_sync);
		} else {
			avail = (int32_t)(impl->tail_timestamp - timestamp);
			error = (double)target_buffer - (double)avail;
			error = SPA_CLAMPD(error, -impl->max_error, impl->max_error);
		}
		corr = spa_dll_update(&impl->dll, error);

		pw_log_trace_fp("avail:%u target:%u error:%f corr:%f", avail,
				target_buffer, error, corr);

		pw_stream_set_rate(impl->stream, 1.0 / corr);
	}

	opus_packet_buffer_read(impl, timestamp, d[0].data, wanted, stride);

	impl->expected_timestamp = timestamp + wanted;

	d[0].chunk->offset = 0;
	d[0].chunk->size = wanted * stride;
	d[0].chunk->stride = stride;
	d[0].chunk->flags = 0;
	buf->size = wanted;

	pw_stream_queue_buffer(impl->stream, buf);
}

static void rtp_opus_process_capture(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t offs, size, timestamp, expected_timestamp, stride;
	uint32_t wanted;
	void *src, *dst;
	struct rtp_packet *p, *t;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_info("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
	stride = impl->stride;
	wanted = size / stride;

	expected_timestamp = impl->expected_timestamp;

	if (SPA_LIKELY(impl->io_position)) {
		uint32_t rate = impl->io_position->clock.rate.denom;
		timestamp = impl->io_position->clock.position * impl->rate / rate;
	} else
		timestamp = expected_timestamp;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u",
				timestamp, impl->seq, impl->ts_offset, impl->ssrc);
		impl->expected_timestamp = expected_timestamp = timestamp;
		impl->have_sync = true;
	} else {
		if (SPA_ABS((int32_t)expected_timestamp - (int32_t)timestamp) > 32) {
			pw_log_warn("expected %u != timestamp %u", expected_timestamp, timestamp);
			impl->have_sync = false;
		}
	}

	src = SPA_PTROFF(d[0].data, offs, void);
	while (wanted > 0) {
		p = rtp_stream_peek_pending_packet((struct rtp_stream*)impl);

		if (p->size < sizeof(struct rtp_header)) {
			struct rtp_header *header;

			header = p->data;
			header->v = 2;
			header->pt = impl->payload;
			header->ssrc = htonl(impl->ssrc);
			if (impl->marker_on_first && impl->first)
				header->m = 1;
			else
				header->m = 0;
			header->sequence_number = htons(impl->seq);

			p->timestamp = impl->ts_offset + impl->ts_align + expected_timestamp;
			header->timestamp = htonl(p->timestamp);

			p->size = sizeof(struct rtp_header);
			p->decoded = p->tmp;
			p->decoded_len = 0;
		}
		uint32_t prepared = p->decoded_len / stride;
		uint32_t to_send = SPA_MIN(impl->psamples - prepared, wanted);

		dst = SPA_PTROFF(p->decoded, p->decoded_len, void);

		spa_memcpy(dst, src, to_send * stride);

		p->decoded_len += to_send * stride;
		prepared += to_send;
		wanted -= to_send;

		src = SPA_PTROFF(src, to_send * stride, void);

		if (prepared >= impl->psamples) {
			OpusMSEncoder *enc = impl->stream_data;

			int res = opus_multistream_encode_float(enc,
					(const float*)p->decoded, p->decoded_len / stride,
					SPA_PTROFF(p->data, p->size, uint8_t), p->maxsize - p->size);
			if (res > 0) {
				p->size += res;

				rtp_stream_queue_packet((struct rtp_stream*)impl, p);

				impl->seq++;
			} else {
				pw_log_error("opus encoder error %d", res);
			}
			rtp_stream_clear_pending_packet((struct rtp_stream*)impl);
		}
		impl->first = false;
		expected_timestamp += to_send;
	}
	impl->expected_timestamp = expected_timestamp;

	pw_stream_queue_buffer(impl->stream, buf);

	spa_list_for_each_safe(p, t, &impl->queued, link)
		rtp_stream_send_packet((struct rtp_stream*)impl, p);
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
