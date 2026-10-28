/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifdef HAVE_OPUS

#include <opus/opus.h>
#include <opus/opus_multistream.h>

/* Opus receive state */
struct rtp_opus_rx {
	OpusMSDecoder *dec;
	uint32_t ts;
	bool have_ts;
	uint32_t plc_ts;
	uint32_t plc_len;
	float plc[];
};

static uint32_t opus_packet_duration(struct rtp_stream *impl, const struct rtp_packet *p)
{
	int res = opus_packet_get_nb_samples(SPA_PTROFF(p->data, p->hlen, uint8_t),
			p->size - p->hlen, impl->rate);
	return SPA_MAX(res, 0);
}

static int opus_packet_decode(struct rtp_stream *impl, struct rtp_packet *p)
{
	struct rtp_opus_rx *rx = impl->stream_data;
	int res;

	/* The decoder takes only full packets and only moves forward in time. */
	if (rx->have_ts && rtp_timestamp_delta(p->timestamp, rx->ts) < 0)
		return -EALREADY;

	res = opus_multistream_decode_float(rx->dec,
			SPA_PTROFF(p->data, p->hlen, void), p->size - p->hlen,
			(float*)p->tmp, p->tmp_size/impl->stride, 0);
	if (res < 0) {
		pw_log_warn("opus decode error %d (%s)", res, opus_strerror(res));
		return res;
	}
	p->decoded = p->tmp;
	p->decoded_len = res * impl->stride;
	p->duration = res;
	rx->ts = p->timestamp + res;
	rx->have_ts = true;
	return 0;
}

static int opus_packet_conceal(struct rtp_stream *impl, const struct rtp_packet *last,
		const struct rtp_packet *next, uint32_t timestamp, void *dst, uint32_t num)
{
	while (num > 0) {
		struct rtp_opus_rx *rx = impl->stream_data;
		const int32_t lost_packets = rtp_seqnum_delta(next->seq, last->seq) - 1;
		const int32_t offs = rtp_timestamp_delta(timestamp, rx->plc_ts);
		uint32_t n;

		if (offs >= 0 && (uint32_t)offs < rx->plc_len) {
			n = SPA_MIN(num, rx->plc_len - offs);
			memcpy(dst, SPA_PTROFF(rx->plc, offs * impl->stride, void), n * impl->stride);
		} else if (rx->have_ts && lost_packets > 0 &&
				rtp_timestamp_delta(timestamp, rx->ts) >= 0 &&
				rtp_timestamp_delta(next->timestamp, rx->ts) > 0) {
			const uint32_t end = last->timestamp + opus_packet_duration(impl, last);
			const int32_t span = rtp_timestamp_delta(next->timestamp, end);
			const int32_t len = SPA_MIN(span / lost_packets, rtp_timestamp_delta(next->timestamp, rx->ts));

			if (len <= 0 || len > 2880)
				break;

			if (rx->ts == end)
				pw_log_info("conceal seq %u-%u from %u", (uint16_t)(last->seq + 1),
						(uint16_t)(next->seq - 1), timestamp);

			int res = opus_multistream_decode_float(rx->dec, NULL, 0, rx->plc, len, 0);
			if (res <= 0) {
				pw_log_warn("opus PLC error %d (%s)", res, opus_strerror(res));
				break;
			}
			rx->plc_ts = rx->ts;
			rx->plc_len = res;
			rx->ts += res;
			continue;
		} else {
			break;
		}
		dst = SPA_PTROFF(dst, n * impl->stride, void);
		timestamp += n;
		num -= n;
	}
	memset(dst, 0, num * impl->stride);
	return 0;
}

static void opus_reset(struct rtp_stream *impl)
{
	struct rtp_opus_rx *rx = impl->stream_data;

	opus_multistream_decoder_ctl(rx->dec, OPUS_RESET_STATE);
	rx->have_ts = false;
	rx->plc_len = 0;
}

static const struct rtp_codec rtp_opus_codec = {
	.duration = opus_packet_duration,
	.decode = opus_packet_decode,
	.conceal = opus_packet_conceal,
	.reset = opus_reset,
};

static void rtp_opus_process_capture(void *data)
{
	struct rtp_stream *impl = data;
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
		p = rtp_stream_peek_pending_packet(impl);

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

				rtp_stream_queue_packet(impl, p);

				impl->seq++;
			} else {
				pw_log_error("opus encoder error %d", res);
			}
			rtp_stream_clear_pending_packet(impl);
		}
		impl->first = false;
		expected_timestamp += to_send;
	}
	impl->expected_timestamp = expected_timestamp;

	pw_stream_queue_buffer(impl->stream, buf);

	spa_list_for_each_safe(p, t, &impl->queued, link)
		rtp_stream_send_packet(impl, p);
}

static void rtp_opus_deinit(struct rtp_stream *impl, enum spa_direction direction)
{
	if (impl->stream_data) {
		if (direction == SPA_DIRECTION_INPUT) {
			opus_multistream_encoder_destroy(impl->stream_data);
		} else {
			struct rtp_opus_rx *rx = impl->stream_data;
			opus_multistream_decoder_destroy(rx->dec);
			free(rx);
		}
	}
}

static int rtp_opus_init(struct rtp_stream *impl, enum spa_direction direction)
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
		struct rtp_opus_rx *rx;

		impl->stream_events.process = rtp_audio_process_playback;

		rx = calloc(1, sizeof(*rx) + 2880 * impl->stride);
		if (rx == NULL)
			return -errno;

		rx->dec = opus_multistream_decoder_create(
			impl->info.info.opus.rate,
			impl->info.info.opus.channels,
			impl->info.info.opus.channels, 0,
			mapping,
			&err);
		if (rx->dec != NULL) {
			opus_multistream_decoder_ctl(rx->dec, OPUS_SET_COMPLEXITY(5));
			impl->stream_data = rx;
		} else {
			free(rx);
		}
	}
	if (!impl->stream_data)
		pw_log_error("opus error: %d (%s)", err, opus_strerror(err));
	return impl->stream_data ? 0 : err;
}
#else
static const struct rtp_codec rtp_opus_codec;

static int rtp_opus_init(struct rtp_stream *impl, enum spa_direction direction)
{
	return -ENOTSUP;
}
#endif
