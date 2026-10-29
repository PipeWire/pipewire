/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifdef HAVE_OPUS

#include <opus/opus.h>
#include <opus/opus_multistream.h>

/* Opus send state */
struct rtp_opus_tx {
	OpusMSEncoder *enc;
	float stage[];
};

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

static uint32_t opus_packet_samples(struct rtp_stream *impl, uint32_t num)
{
	if (num >= 2880)
		return 2880;
	else if (num >= 1920)
		return 1920;
	else if (num >= 960)
		return 960;
	else if (num >= 480)
		return 480;
	else if (num >= 240)
		return 240;
	else
		return 120;
}

static int opus_packet_encode(struct rtp_stream *impl, struct rtp_packet *p, const void *src,
		uint32_t *num)
{
	struct rtp_opus_tx *tx = impl->stream_data;
	int res;

	*num = SPA_MIN(impl->psamples - p->duration, *num);
	/* collect a packet that spans cycles in the stage */
	if (*num < impl->psamples) {
		spa_memcpy(SPA_PTROFF(tx->stage, p->duration * impl->stride, void), src,
				*num * impl->stride);
		src = tx->stage;
	}
	p->duration += *num;
	if (p->duration < impl->psamples)
		return 0;

	res = opus_multistream_encode_float(tx->enc, src, impl->psamples,
			SPA_PTROFF(p->data, p->size, void), impl->payload_size);
	if (res < 0) {
		pw_log_error("opus encode error %d (%s)", res, opus_strerror(res));
		return res;
	}
	p->size += res;
	return 1;
}

static void rtp_opus_deinit(struct rtp_stream *impl, enum spa_direction direction)
{
	if (impl->stream_data) {
		if (direction == SPA_DIRECTION_INPUT) {
			struct rtp_opus_tx *tx = impl->stream_data;
			opus_multistream_encoder_destroy(tx->enc);
			free(tx);
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

	for (i = 0; i < impl->info.info.opus.channels; i++)
		mapping[i] = i;

	if (direction == SPA_DIRECTION_INPUT) {
		struct rtp_opus_tx *tx;

		tx = calloc(1, sizeof(*tx) + impl->psamples * impl->stride);
		if (tx == NULL)
			return -errno;

		tx->enc = opus_multistream_encoder_create(
			impl->info.info.opus.rate,
			impl->info.info.opus.channels,
			impl->info.info.opus.channels, 0,
			mapping,
			OPUS_APPLICATION_AUDIO,
			&err);
		if (tx->enc != NULL)
			impl->stream_data = tx;
		else
			free(tx);
	}
	else {
		struct rtp_opus_rx *rx;

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
	if (impl->stream_data == NULL) {
		pw_log_error("opus error: %d (%s)", err, opus_strerror(err));
		return -EINVAL;
	}
	return 0;
}

static const struct rtp_codec rtp_opus_codec = {
	.init = rtp_opus_init,
	.deinit = rtp_opus_deinit,
	.packet_samples = opus_packet_samples,
	.duration = opus_packet_duration,
	.decode = opus_packet_decode,
	.conceal = opus_packet_conceal,
	.reset = opus_reset,
	.encode = opus_packet_encode,
};
#else
static const struct rtp_codec rtp_opus_codec;
#endif
