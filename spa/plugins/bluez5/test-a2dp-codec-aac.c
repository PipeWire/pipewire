/* Spa A2DP AAC codec tests */
/* SPDX-FileCopyrightText: Copyright © 2026 Nils Schimmelmann */
/* SPDX-License-Identifier: MIT */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>

#include <spa/utils/defs.h>
#include <spa/support/log.h>

#include "media-codecs.h"
#include "rtp.h"
#include "aac-latm.h"

extern const struct media_codec a2dp_codec_aac;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
		abort(); \
	} \
} while (0)

#define MAX_PACKETS	1024
#define MAX_PACKET_SIZE	4096
#define SECONDS		1

struct packet {
	uint8_t data[MAX_PACKET_SIZE];
	size_t size;
};

struct stats {
	unsigned int n_packets;
	size_t max_size;
	size_t total;
	unsigned int gaps;	/* frames dropped by the encoder */
	size_t aac_bytes;	/* AAC frame bytes, without RTP and LATM */
	uint32_t frame_samples;
	long lag;		/* measured delay, in samples */
	double snr;		/* decoded against input, dB */
	uint32_t delay;		/* delay reported by the codec */
	bool first_missing;	/* the decoder produced nothing for the first packet */
	int pns_sections;	/* noise substitution sections in mono streams, -1 if unknown */
	double corr;
	uint32_t rate;
	uint32_t channels;
};

static void test_log(void *object, enum spa_log_level level, const char *file,
		int line, const char *func, const char *fmt, ...) SPA_PRINTF_FUNC(6, 7);

static void test_log(void *object, enum spa_log_level level, const char *file,
		int line, const char *func, const char *fmt, ...)
{
	va_list args;

	fprintf(stderr, "[%d] %s:%d %s: ", (int)level, file, line, func);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

static const struct spa_log_methods log_methods = {
	.version = 0,
	.log = test_log,
};

static struct spa_log test_logger = {
	.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG, &log_methods, NULL),
	.level = SPA_LOG_LEVEL_WARN,
};

static uint32_t rng_state = 12345;

static uint32_t rng(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

/*
 * Tones plus decaying noise bursts every ~110 ms: hard for a frame size limit.
 * The harsh signal alternates 150 ms of silence with full-scale white noise.
 */
static void make_signal(int16_t *pcm, uint32_t rate, uint32_t channels, size_t frames, bool harsh)
{
	size_t i, since_burst = 0;
	uint32_t c;

	rng_state = 12345;
	for (i = 0; i < frames; i++) {
		double t = (double)i / rate, burst = 0.0;

		if (harsh) {
			bool noisy = (i / (rate * 3 / 20)) % 2 == 1;

			for (c = 0; c < channels; c++)
				pcm[i * channels + c] = noisy ?
					(int16_t)((int)(rng() % 60001) - 30000) : 0;
			continue;
		}

		if (i % (rate / 9) == 0)
			since_burst = 0;
		if (since_burst < 600)
			burst = 14000.0 * exp(-(double)since_burst / 80.0) *
				((double)(rng() % 20001) / 10000.0 - 1.0);
		since_burst++;

		for (c = 0; c < channels; c++) {
			double v = 5000.0 * sin(2 * M_PI * 220.0 * t + c) +
				2500.0 * sin(2 * M_PI * 3300.0 * t) + burst;
			pcm[i * channels + c] = (int16_t)SPA_CLAMP(v, -32000.0, 32000.0);
		}
	}
}

/* Count noise substitution (codebook 13) sections in a raw mono AAC-LC frame, -1 if not parsed */
static int count_pns_sections(const uint8_t *frame, size_t size)
{
	struct latm_bits_in b = { frame, 8 * size, 0 };
	int64_t v;
	int count = 0, n_groups = 1, max_sfb, sect_bits, g, k;
	bool is_short;

#define GET(var, n) do { v = latm_bits_in_get(&b, (n)); if (v < 0) return -1; (var) = (int)v; } while (0)
	int id, tmp, ws, grouping = 0;

	GET(id, 3);
	if (id != 0)			/* single channel element */
		return -1;
	GET(tmp, 4);			/* element_instance_tag */
	GET(tmp, 8);			/* global_gain */
	GET(tmp, 1);			/* ics_reserved_bit */
	GET(ws, 2);			/* window_sequence */
	GET(tmp, 1);			/* window_shape */
	is_short = ws == 2;
	if (is_short) {
		GET(max_sfb, 4);
		GET(grouping, 7);
		for (g = 0; g < 7; g++)
			if (!((grouping >> (6 - g)) & 1))
				n_groups++;
	} else {
		GET(max_sfb, 6);
		GET(tmp, 1);		/* predictor_data_present */
		if (tmp)
			return -1;
	}

	sect_bits = is_short ? 3 : 5;
	for (g = 0; g < n_groups; g++) {
		k = 0;
		while (k < max_sfb) {
			int cb, len = 0, incr;

			GET(cb, 4);
			do {
				GET(incr, sect_bits);
				len += incr;
			} while (incr == (1 << sect_bits) - 1);
			if (len == 0 || k + len > max_sfb)
				return -1;
			k += len;
			if (cb == 13)
				count++;
		}
	}
#undef GET
	return count;
}

static double correlation(const int16_t *in, const int16_t *out, uint32_t channels,
		size_t start, size_t len, long min_lag, long max_lag, long *best_lag)
{
	double best = -1.0;
	long lag;
	size_t i;

	for (lag = min_lag; lag < max_lag; lag++) {
		double xy = 0, xx = 0, yy = 0;

		for (i = start; i < start + len; i++) {
			double x = in[i * channels], y = out[((long)i + lag) * channels];
			xy += x * y;
			xx += x * x;
			yy += y * y;
		}
		if (xx > 0 && yy > 0 && xy / sqrt(xx * yy) > best) {
			best = xy / sqrt(xx * yy);
			*best_lag = lag;
		}
	}
	return best;
}

static double snr_at(const int16_t *in, const int16_t *out, uint32_t channels,
		size_t start, size_t len, long lag)
{
	double sig = 0, err = 0;
	size_t i;
	uint32_t c;

	for (i = start; i < start + len; i++) {
		for (c = 0; c < channels; c++) {
			double x = in[i * channels + c], y = out[((long)i + lag) * channels + c];

			sig += x * x;
			err += (x - y) * (x - y);
		}
	}
	return err > 0 ? 10 * log10(sig / err) : 99.0;
}

static void run(uint8_t object_type, uint32_t rate, uint32_t freq_flag,
		uint32_t channels, uint8_t chan_flag, size_t mtu, uint32_t negotiated,
		bool harsh, struct stats *st)
{
	const struct media_codec *codec = &a2dp_codec_aac;
	a2dp_aac_t conf = {
		.object_type = object_type,
		AAC_INIT_FREQUENCY(freq_flag)
		.channels = chan_flag,
		.vbr = 0,
		AAC_INIT_BITRATE(negotiated)
	};
	struct spa_audio_info info;
	struct spa_dict_item items[1];
	struct spa_dict settings = SPA_DICT_INIT(items, 0);
	static struct packet packets[MAX_PACKETS];
	size_t frames = (size_t)rate * SECONDS, in_frames = 0;
	int16_t *in, *out;
	size_t out_bytes = 0;
	void *props, *data;
	int block, res;
	uint32_t seq = 0, frame_samples = 0, prev_ts = 0;
	size_t i;
	bool have_prev_ts = false;
	int pns_total = 0, pns_parsed = 0;

	memset(st, 0, sizeof(*st));
	st->rate = rate;
	st->channels = channels;

	CHECK(codec->validate_config(codec, 0, &conf, sizeof(conf), &info) == 0);
	CHECK(info.info.raw.rate == rate && info.info.raw.channels == channels);

	props = codec->init_props(codec, 0, &settings);
	CHECK(props != NULL);
	data = codec->init(codec, 0, &conf, sizeof(conf), &info, props, mtu);
	if (data == NULL)
		fprintf(stderr, "init failed: %s\n", strerror(errno));
	CHECK(data != NULL);

	block = codec->get_block_size(data);
	CHECK(block > 0 && block % (2 * channels) == 0);
	frame_samples = block / (2 * channels);
	st->frame_samples = frame_samples;

	in = calloc(frames + 2 * frame_samples, channels * sizeof(int16_t));
	out = calloc(frames + 16 * frame_samples, channels * sizeof(int16_t));
	CHECK(in && out);
	make_signal(in, rate, channels, frames, harsh);

	{
		/* encode, packet building as the sink does */
		uint8_t pkt[MAX_PACKET_SIZE];
		size_t hdr = 0, used = 0;
		uint32_t sample_count = 0;
		bool started = false;

		while (in_frames + frame_samples <= frames) {
			size_t out_size = 0;
			int need_flush = 0;

			if (!started) {
				hdr = codec->start_encode(data, pkt, sizeof(pkt), (uint16_t)seq, sample_count);
				used = hdr;
				started = true;
			}
			res = codec->encode(data, in + in_frames * channels, block,
					pkt + used, sizeof(pkt) - used, &out_size, &need_flush);
			CHECK(res == block);
			in_frames += frame_samples;
			sample_count += frame_samples;
			used += out_size;

			if (!need_flush) {
				CHECK(out_size == 0);
				continue;
			}
			CHECK(out_size > 0);
			started = false;
			seq++;

			/* the whole packet must fit the MTU: no fragmentation */
			CHECK(used <= mtu);
			CHECK(st->n_packets < MAX_PACKETS);
			memcpy(packets[st->n_packets].data, pkt, used);
			packets[st->n_packets].size = used;
			st->n_packets++;
			st->total += used;
			st->max_size = SPA_MAX(st->max_size, used);
		}
	}
	CHECK(st->n_packets > 10);

	/* RTP header and timestamps, one frame per packet */
	for (i = 0; i < st->n_packets; i++) {
		const struct rtp_header *h = (const struct rtp_header *)packets[i].data;
		uint32_t ts = ntohl(h->timestamp);

		CHECK(h->v == 2 && h->pt == 96 && h->m == 1);
		CHECK(ntohs(h->sequence_number) == (uint16_t)i);
		if (have_prev_ts) {
			/* a dropped frame leaves a gap of whole frames */
			CHECK(ts > prev_ts && (ts - prev_ts) % frame_samples == 0);
			st->gaps += (ts - prev_ts) / frame_samples - 1;
		}
		prev_ts = ts;
		have_prev_ts = true;
	}

	/* LATM: complete AudioMuxElement with muxConfigPresent, and PNS usage */
	for (i = 0; i < st->n_packets; i++) {
		struct latm_dec dec;
		uint8_t frame[LATM_FRAME_MAXSIZE];
		bool changed;

		latm_dec_init(&dec);
		res = latm_dec_read(&dec, packets[i].data + sizeof(struct rtp_header),
				packets[i].size - sizeof(struct rtp_header), frame, sizeof(frame), &changed);
		CHECK(res > 0 && changed);
		st->aac_bytes += res;
		CHECK(latm_rates[dec.rate_index] == rate && (uint32_t)dec.channel_config == channels);
		if (channels == 1) {
			int n = count_pns_sections(frame, res);

			if (n >= 0) {
				pns_total += n;
				pns_parsed++;
			}
			if (object_type == AAC_OBJECT_TYPE_MPEG2_AAC_LC)
				CHECK(n <= 0);
		}
	}
	st->pns_sections = channels == 1 && pns_parsed ? pns_total : -1;

	/* decode everything back */
	for (i = 0; i < st->n_packets; i++) {
		uint16_t seqnum;
		uint32_t ts;
		size_t written = 0;

		res = codec->start_decode(data, packets[i].data, packets[i].size, &seqnum, &ts);
		CHECK(res == (int)sizeof(struct rtp_header));
		CHECK(seqnum == (uint16_t)i);

		res = codec->decode(data, packets[i].data + res, packets[i].size - res,
				(uint8_t *)out + out_bytes, 16384, &written);
		if (written != (size_t)block)
			fprintf(stderr, "packet %zu: decoded %zu bytes, expected %d (res %d)\n",
					i, written, block, res);
		CHECK(res == (int)(packets[i].size - sizeof(struct rtp_header)));
		/* FAAD2 emits nothing for the first frame */
		CHECK(written == (size_t)block || (i == 0 && written == 0));
		if (i == 0 && written == 0)
			st->first_missing = true;
		out_bytes += written;
	}
	CHECK(out_bytes >= (size_t)(st->n_packets - 1) * block);

	/* the output is the input, delayed */
	{
		uint32_t enc_delay = 0, dec_delay = 0;

		st->corr = correlation(in, out, channels, rate / 2, rate / 4,
				-(long)frame_samples, 4 * (long)frame_samples, &st->lag);
		st->snr = snr_at(in, out, channels, rate / 2, rate / 4, st->lag);
		codec->get_delay(data, &enc_delay, &dec_delay);
		st->delay = enc_delay + dec_delay;
	}

	/* bad packets are rejected without stalling or breaking later decoding */
	{
		struct packet *p = &packets[st->n_packets / 2];
		uint8_t bad[MAX_PACKET_SIZE];
		size_t written = 0, n;
		uint8_t *pcm = malloc(16384);

		CHECK(pcm);
		for (n = 0; n < p->size - sizeof(struct rtp_header); n += 7) {
			memcpy(bad, p->data + sizeof(struct rtp_header), n);
			res = codec->decode(data, bad, n, pcm, 16384, &written);
			CHECK(res < 0 || written == (size_t)block || written == 0);
		}
		memcpy(bad, p->data + sizeof(struct rtp_header), p->size - sizeof(struct rtp_header));
		memset(bad + 12, 0xff, p->size - sizeof(struct rtp_header) - 12);
		res = codec->decode(data, bad, p->size - sizeof(struct rtp_header), pcm, 16384, &written);
		CHECK(res < 0 || written <= 16384);
		memset(bad, 0, sizeof(bad));
		res = codec->decode(data, bad, 64, pcm, 16384, &written);
		CHECK(res < 0 || written <= 16384);

		/* the valid packet still decodes afterwards */
		res = codec->decode(data, p->data + sizeof(struct rtp_header),
				p->size - sizeof(struct rtp_header), pcm, 16384, &written);
		CHECK(res == (int)(p->size - sizeof(struct rtp_header)));
		CHECK(written == (size_t)block);
		free(pcm);
	}

	codec->deinit(data);
	codec->clear_props(props);
	free(in);
	free(out);
}

/* The port format is fixed: packets in another format are dropped, not played */
static void test_format_mismatch(void)
{
#if defined(HAVE_FAAC)
	const struct media_codec *codec = &a2dp_codec_aac;
	static const struct { uint32_t rate, flag, channels; uint8_t chan_flag; } other[] = {
		{ 44100, AAC_SAMPLING_FREQ_44100, 2, AAC_CHANNELS_2 },	/* other rate */
		{ 48000, AAC_SAMPLING_FREQ_48000, 1, AAC_CHANNELS_1 },	/* other channels */
	};
	static struct packet packets[8];
	size_t i, k, n = 0;
	struct spa_dict settings = SPA_DICT_INIT(NULL, 0);
	a2dp_aac_t conf = {
		.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC,
		AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_48000)
		.channels = AAC_CHANNELS_2,
		AAC_INIT_BITRATE(320000)
	};
	struct spa_audio_info info;
	void *props, *src;
	int16_t *pcm;
	int block;

	CHECK(codec->validate_config(codec, 0, &conf, sizeof(conf), &info) == 0);
	props = codec->init_props(codec, 0, &settings);
	src = codec->init(codec, 0, &conf, sizeof(conf), &info, props, 672);
	CHECK(src != NULL);
	block = codec->get_block_size(src);
	pcm = calloc(12, block);
	CHECK(pcm);
	make_signal(pcm, 48000, 2, 12 * block / 4, false);
	for (i = 0; i < 12 && n < SPA_N_ELEMENTS(packets); i++) {
		size_t hdr = codec->start_encode(src, packets[n].data, sizeof(packets[n].data), n, i * 1024);
		size_t out_size = 0;
		int need_flush = 0;

		CHECK(codec->encode(src, (uint8_t *)pcm + i * block, block, packets[n].data + hdr,
				sizeof(packets[n].data) - hdr, &out_size, &need_flush) == block);
		if (need_flush) {
			packets[n].size = hdr + out_size;
			n++;
		}
	}
	CHECK(n >= 4);
	free(pcm);

	for (k = 0; k < SPA_N_ELEMENTS(other); k++) {
		a2dp_aac_t oconf = {
			.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC,
			AAC_INIT_FREQUENCY(other[k].flag)
			.channels = other[k].chan_flag,
			AAC_INIT_BITRATE(320000)
		};
		struct spa_audio_info oinfo;
		void *oprops, *dst;
		uint8_t out[16384];

		CHECK(codec->validate_config(codec, 0, &oconf, sizeof(oconf), &oinfo) == 0);
		oprops = codec->init_props(codec, 0, &settings);
		dst = codec->init(codec, 0, &oconf, sizeof(oconf), &oinfo, oprops, 672);
		CHECK(dst != NULL);

		/* every packet, not only the first one with the stream configuration */
		for (i = 0; i < n; i++) {
			size_t written = 0;
			int res = codec->decode(dst, packets[i].data + sizeof(struct rtp_header),
					packets[i].size - sizeof(struct rtp_header), out, sizeof(out), &written);

			CHECK(res < 0);
			CHECK(written == 0);
		}
		codec->deinit(dst);
		codec->clear_props(oprops);
	}
	codec->deinit(src);
	codec->clear_props(props);
	printf("packets in another format are rejected\n");
#endif
}

/* The sink passes everything it has: the encoder must consume at most one block per call */
static void test_multi_block_input(void)
{
	const struct media_codec *codec = &a2dp_codec_aac;
	struct spa_dict settings = SPA_DICT_INIT(NULL, 0);
	a2dp_aac_t conf = {
		.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC,
		AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_48000)
		.channels = AAC_CHANNELS_2,
		AAC_INIT_BITRATE(320000)
	};
	struct spa_audio_info info;
	uint8_t pkt[MAX_PACKET_SIZE];
	void *props, *enc;
	int16_t *pcm;
	int block, total, left;
	unsigned int calls = 0;

	CHECK(codec->validate_config(codec, 0, &conf, sizeof(conf), &info) == 0);
	props = codec->init_props(codec, 0, &settings);
	enc = codec->init(codec, 0, &conf, sizeof(conf), &info, props, 672);
	CHECK(enc != NULL);
	block = codec->get_block_size(enc);
	total = 4 * block;
	pcm = calloc(4, block);
	CHECK(pcm);
	make_signal(pcm, 48000, 2, total / 4, false);

	for (left = total; left > 0; calls++) {
		size_t hdr = codec->start_encode(enc, pkt, sizeof(pkt), calls, calls * 1024);
		size_t out_size = 0;
		int need_flush = 0;
		int res = codec->encode(enc, (uint8_t *)pcm + (total - left), left,
				pkt + hdr, sizeof(pkt) - hdr, &out_size, &need_flush);

		CHECK(res > 0 && res <= left);
		left -= res;
	}
	CHECK(calls >= 4);
	free(pcm);
	codec->deinit(enc);
	codec->clear_props(props);
	printf("input of several blocks is consumed in %u calls\n", calls);
}

static const struct { uint32_t rate, flag; } all_rates[] = {
	{ 8000, AAC_SAMPLING_FREQ_8000 },
	{ 11025, AAC_SAMPLING_FREQ_11025 },
	{ 12000, AAC_SAMPLING_FREQ_12000 },
	{ 16000, AAC_SAMPLING_FREQ_16000 },
	{ 22050, AAC_SAMPLING_FREQ_22050 },
	{ 24000, AAC_SAMPLING_FREQ_24000 },
	{ 32000, AAC_SAMPLING_FREQ_32000 },
	{ 44100, AAC_SAMPLING_FREQ_44100 },
	{ 48000, AAC_SAMPLING_FREQ_48000 },
	{ 64000, AAC_SAMPLING_FREQ_64000 },
	{ 88200, AAC_SAMPLING_FREQ_88200 },
	{ 96000, AAC_SAMPLING_FREQ_96000 },
};

/* Every advertised sampling frequency can be opened and encodes within the MTU */
static void test_all_rates(void)
{
	const struct media_codec *codec = &a2dp_codec_aac;
	static const uint8_t types[] = { AAC_OBJECT_TYPE_MPEG2_AAC_LC, AAC_OBJECT_TYPE_MPEG4_AAC_LC };
	const size_t mtu = 672;
	size_t r, t;
	uint32_t channels;

	for (t = 0; t < SPA_N_ELEMENTS(types); t++)
	for (r = 0; r < SPA_N_ELEMENTS(all_rates); r++)
	for (channels = 1; channels <= 2; channels++) {
		a2dp_aac_t conf = {
			.object_type = types[t],
			AAC_INIT_FREQUENCY(all_rates[r].flag)
			.channels = channels == 1 ? AAC_CHANNELS_1 : AAC_CHANNELS_2,
			.vbr = 0,
			AAC_INIT_BITRATE(320000)
		};
		struct spa_audio_info info;
		struct spa_dict settings = SPA_DICT_INIT(NULL, 0);
		void *props, *data;
		int16_t *pcm;
		uint8_t pkt[MAX_PACKET_SIZE], pcm_out[16384];
		int block, i, n_packets = 0;

		CHECK(codec->validate_config(codec, 0, &conf, sizeof(conf), &info) == 0);
		CHECK(info.info.raw.rate == all_rates[r].rate && info.info.raw.channels == channels);
		props = codec->init_props(codec, 0, &settings);
		data = codec->init(codec, 0, &conf, sizeof(conf), &info, props, mtu);
		if (data == NULL)
			fprintf(stderr, "init failed: mpeg%d %u Hz %u ch: %s\n",
					types[t] == AAC_OBJECT_TYPE_MPEG2_AAC_LC ? 2 : 4,
					all_rates[r].rate, channels, strerror(errno));
		CHECK(data != NULL);

		block = codec->get_block_size(data);
		CHECK(block > 0);
		pcm = calloc(8, block);
		CHECK(pcm);
		make_signal(pcm, all_rates[r].rate, channels, 8 * block / (2 * channels), false);
		for (i = 0; i < 8; i++) {
			size_t hdr, out_size = 0;
			int need_flush = 0;

			hdr = codec->start_encode(data, pkt, sizeof(pkt), i, i * block);
			CHECK(codec->encode(data, (uint8_t *)pcm + i * block, block, pkt + hdr,
					sizeof(pkt) - hdr, &out_size, &need_flush) == block);
			CHECK(hdr + out_size <= mtu);
			if (need_flush) {
				size_t written = 0;
				int res;

				res = codec->start_decode(data, pkt, hdr + out_size, NULL, NULL);
				CHECK(res == (int)hdr);
				res = codec->decode(data, pkt + hdr, out_size, pcm_out, sizeof(pcm_out), &written);
				if (res != (int)out_size || (written != (size_t)block && !(n_packets == 0 && written == 0)))
					fprintf(stderr, "decode at %u Hz %u ch: res %d, %zu bytes, expected %d\n",
							all_rates[r].rate, channels, res, written, block);
				CHECK(res == (int)out_size);
				/* FAAD2 emits nothing for the first frame */
				CHECK(written == (size_t)block || (n_packets == 0 && written == 0));
				n_packets++;
			}
		}
		CHECK(n_packets > 0);
		free(pcm);
		codec->deinit(data);
		codec->clear_props(props);
	}
	printf("all advertised sampling frequencies encode and decode\n");
}

/* MPEG-4 AAC LC is preferred with FAAC (PNS), MPEG-2 AAC LC with FDK-AAC */
static void test_select_config(void)
{
	const struct media_codec *codec = &a2dp_codec_aac;
	a2dp_aac_t caps = {
		.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC | AAC_OBJECT_TYPE_MPEG4_AAC_LC,
		AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_44100 | AAC_SAMPLING_FREQ_48000)
		.channels = AAC_CHANNELS_1 | AAC_CHANNELS_2,
		.vbr = 1,
		AAC_INIT_BITRATE(320000)
	};
	struct media_codec_audio_info audio = { .rate = 48000, .channels = 2 };
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	void *config_data = NULL;
	a2dp_aac_t sel;
	int size;

	size = codec->select_config(codec, 0, &caps, sizeof(caps), &audio, NULL, config, &config_data);
	CHECK(size == (int)sizeof(sel));
	memcpy(&sel, config, sizeof(sel));
#if defined(HAVE_FAAC)
	CHECK(sel.object_type == AAC_OBJECT_TYPE_MPEG4_AAC_LC);
#else
	CHECK(sel.object_type == AAC_OBJECT_TYPE_MPEG2_AAC_LC);
#endif

	/* with only one of them offered, that one is selected */
	caps.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC;
	size = codec->select_config(codec, 0, &caps, sizeof(caps), &audio, NULL, config, &config_data);
	CHECK(size == (int)sizeof(sel));
	memcpy(&sel, config, sizeof(sel));
	CHECK(sel.object_type == AAC_OBJECT_TYPE_MPEG2_AAC_LC);
	caps.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
	size = codec->select_config(codec, 0, &caps, sizeof(caps), &audio, NULL, config, &config_data);
	CHECK(size == (int)sizeof(sel));
	memcpy(&sel, config, sizeof(sel));
	CHECK(sel.object_type == AAC_OBJECT_TYPE_MPEG4_AAC_LC);
	printf("object type selection OK\n");
}

/* The audio bitrate follows the negotiated one, and the frame limit holds for hard signals */
static void test_bitrates(void)
{
	static const struct { uint32_t negotiated; bool harsh; } cases[] = {
		{ 128000, false },
		{ 192000, false },
		{ 320000, true },
		{ 192000, true },
	};
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(cases); i++) {
		struct stats st;
		double kbps, target = cases[i].negotiated / 1000.0;

		/* MPEG-2: no PNS, so that the waveform SNR is meaningful */
		run(AAC_OBJECT_TYPE_MPEG2_AAC_LC, 44100, AAC_SAMPLING_FREQ_44100, 2, AAC_CHANNELS_2,
				895, cases[i].negotiated, cases[i].harsh, &st);
		kbps = (double)st.aac_bytes * 8 * st.rate /
			((double)(st.n_packets + st.gaps) * st.frame_samples) / 1000.0;
		printf("negotiated %3.0f kbit/s%s: audio %6.1f kbit/s (%+5.1f%%), max packet %zu bytes, dropped %u, SNR %.1f dB\n",
				target, cases[i].harsh ? ", noise bursts" : "", kbps,
				100.0 * (kbps - target) / target, st.max_size, st.gaps, st.snr);
#if defined(HAVE_FDK_AAC) || defined(HAVE_FAAC)
		CHECK(st.gaps == 0);
#endif
#if defined(HAVE_FAAC)
		/* CBR with a hard frame limit tracks the target */
		if (!cases[i].harsh || cases[i].negotiated < 300000)
			CHECK(kbps > 0.9 * target && kbps < 1.1 * target);
#endif
	}
}

int main(void)
{
	static const struct { uint32_t rate, flag; } rates[] = {
		{ 44100, AAC_SAMPLING_FREQ_44100 },
		{ 48000, AAC_SAMPLING_FREQ_48000 },
	};
	static const struct { uint32_t channels; uint8_t flag; } chans[] = {
		{ 1, AAC_CHANNELS_1 },
		{ 2, AAC_CHANNELS_2 },
	};
	static const uint8_t types[] = {
		AAC_OBJECT_TYPE_MPEG2_AAC_LC,
		AAC_OBJECT_TYPE_MPEG4_AAC_LC,
	};
	static const size_t mtus[] = { 200, 335, 672, 895 };
	size_t r, c, t, m;
	double min_corr = 1.0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (getenv("AAC_TEST_VERBOSE"))
		test_logger.level = SPA_LOG_LEVEL_DEBUG;
	if (a2dp_codec_aac.set_log)
		a2dp_codec_aac.set_log(&test_logger);

	for (t = 0; t < SPA_N_ELEMENTS(types); t++)
	for (r = 0; r < SPA_N_ELEMENTS(rates); r++)
	for (c = 0; c < SPA_N_ELEMENTS(chans); c++)
	for (m = 0; m < SPA_N_ELEMENTS(mtus); m++) {
		struct stats st;
		double kbps;

		run(types[t], rates[r].rate, rates[r].flag, chans[c].channels, chans[c].flag, mtus[m],
				320000, false, &st);
		kbps = (double)st.total * 8 * st.rate / ((double)st.n_packets * 1024) / 1000.0;
		printf("mpeg%d %5u Hz %u ch mtu %4zu: %4u packets, max %4zu bytes, %6.1f kbit/s, dropped %u, pns sections %d, corr %.3f, lag %ld (reported delay %u)\n",
				types[t] == AAC_OBJECT_TYPE_MPEG2_AAC_LC ? 2 : 4,
				st.rate, st.channels, mtus[m], st.n_packets, st.max_size,
				kbps, st.gaps, st.pns_sections, st.corr, st.lag, st.delay);
#if defined(HAVE_FDK_AAC) || defined(HAVE_FAAC)
		/* the encoder enforces the frame size limit */
		CHECK(st.gaps == 0);
#endif
		/* get_delay() is what the sink reports as latency: it must match what is heard */
		if (st.gaps == 0)
			CHECK(st.lag + (st.first_missing ? (long)st.frame_samples : 0) == (long)st.delay);
		/* with many dropped frames the output is not expected to resemble the input */
		if (st.gaps * 50 <= st.n_packets) {
			min_corr = SPA_MIN(min_corr, st.corr);
			CHECK(st.corr > 0.5);
		}
	}
	printf("minimum correlation %.3f\n", min_corr);

	test_select_config();
	test_format_mismatch();
	test_multi_block_input();
	test_all_rates();
	test_bitrates();
	return 0;
}
