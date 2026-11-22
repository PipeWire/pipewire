/* Spa A2DP AAC MPEG-4 LATM minimal mux/demux */
/* SPDX-FileCopyrightText: Copyright © 2026 Nils Schimmelmann */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_AAC_LATM_H
#define SPA_BLUEZ5_AAC_LATM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>

/*
 * A2DP AAC payload: MPEG-4 AudioMuxElement with muxConfigPresent=1
 * (RFC 6416, section 6), one program, one layer, AAC-LC.
 *
 * The muxer writes audioMuxVersion=1 with the StreamMuxConfig in every
 * element, so a receiver can join at any packet. The demuxer accepts
 * audioMuxVersion 0 and 1 and a StreamMuxConfig that is repeated or reused
 * (useSameStreamMux).
 */

#define LATM_ERROR_NOSPC		(-1)
#define LATM_ERROR_BAD_FREQUENCY	(-2)
#define LATM_ERROR_BAD_CHANNELS		(-3)
#define LATM_ERROR_EOF			(-4)
#define LATM_ERROR_BAD_AOT		(-5)
#define LATM_ERROR_BAD_EXT		(-6)
#define LATM_ERROR_BAD_FILL		(-7)
#define LATM_ERROR_BAD_FRAMELEN		(-8)
#define LATM_ERROR_BAD_ASC		(-9)
#define LATM_ERROR_NO_CONFIG		(-10)
#define LATM_ERROR_UNIMPLEMENTED(code)	(-(1000 | (int)(code)))

/**
 * latmBufferFullness written by the muxer. A fixed 6144 bits per channel in
 * units of 32 bits, as FDK-AAC writes with audioMuxVersion 2, so that the
 * header is the same as the one headsets already receive from FDK-AAC.
 */
#define LATM_BUFFER_FULLNESS		0xc0

/** Upper bound of the PayloadLengthInfo field in bytes */
#define LATM_LENGTH_MAXSIZE		8
/** Largest AudioSpecificConfig the muxer accepts, in bytes */
#define LATM_ASC_MAXSIZE		14
/** Largest AAC frame the muxer and demuxer handle, in bytes */
#define LATM_FRAME_MAXSIZE		(255 * LATM_LENGTH_MAXSIZE - 1)

struct latm_bits_out {
	uint8_t *buf;
	size_t size;	/* bytes */
	size_t pos;	/* bits */
};

struct latm_bits_in {
	const uint8_t *buf;
	size_t end;	/* bits */
	size_t pos;	/* bits */
};

struct latm_enc {
	uint8_t head[7 + LATM_ASC_MAXSIZE];
	uint8_t head_size;
};

struct latm_dec {
	bool have_config;
	int rate_index;
	int channel_config;
	uint8_t asc[2];
};

static const uint32_t latm_rates[] = {
	96000, 88200, 64000, 48000, 44100, 32000, 24000,
	22050, 16000, 12000, 11025, 8000, 7350,
};

/** \return sampling frequency index of rate, or error */
static inline int latm_freq_index(uint32_t rate)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(latm_rates); i++)
		if (latm_rates[i] == rate)
			return (int)i;
	return LATM_ERROR_BAD_FREQUENCY;
}

/** Build the 2-byte AAC-LC AudioSpecificConfig for rate and channel count (1 or 2) */
static inline int latm_make_asc(uint32_t rate, uint32_t channels, uint8_t asc[2])
{
	int freq = latm_freq_index(rate);

	if (freq < 0)
		return freq;
	if (channels != 1 && channels != 2)
		return LATM_ERROR_BAD_CHANNELS;

	/* object type 2 (5 bits), freq index (4), channel config (4), 3 zero bits */
	asc[0] = (uint8_t)((2 << 3) | (freq >> 1));
	asc[1] = (uint8_t)(((freq & 1) << 7) | (channels << 3));
	return 0;
}

static inline int latm_bits_out_put(struct latm_bits_out *b, unsigned int nbits, uint32_t value)
{
	if (nbits > 32 || b->pos + nbits > 8 * b->size)
		return LATM_ERROR_NOSPC;

	while (nbits > 0) {
		nbits--;
		if ((value >> nbits) & 1)
			b->buf[b->pos / 8] |= (uint8_t)(0x80 >> (b->pos % 8));
		else
			b->buf[b->pos / 8] &= (uint8_t)~(0x80 >> (b->pos % 8));
		b->pos++;
	}
	return 0;
}

static inline int latm_bits_out_put_bytes(struct latm_bits_out *b, const uint8_t *src, size_t size)
{
	size_t i;
	int res;

	if (b->pos + 8 * size > 8 * b->size)
		return LATM_ERROR_NOSPC;

	if (b->pos % 8 == 0) {
		memcpy(b->buf + b->pos / 8, src, size);
		b->pos += 8 * size;
		return 0;
	}
	for (i = 0; i < size; i++)
		if ((res = latm_bits_out_put(b, 8, src[i])) < 0)
			return res;
	return 0;
}

/** \return value, or LATM_ERROR_EOF */
static inline int64_t latm_bits_in_get(struct latm_bits_in *b, unsigned int nbits)
{
	uint32_t value = 0;

	if (nbits > 32 || b->pos + nbits > b->end)
		return LATM_ERROR_EOF;

	while (nbits-- > 0) {
		value = (value << 1) | ((b->buf[b->pos / 8] >> (7 - b->pos % 8)) & 1);
		b->pos++;
	}
	return value;
}

static inline int latm_bits_in_get_bytes(struct latm_bits_in *b, uint8_t *dst, size_t size)
{
	size_t i;

	if (b->pos + 8 * size > b->end)
		return LATM_ERROR_EOF;

	if (b->pos % 8 == 0) {
		memcpy(dst, b->buf + b->pos / 8, size);
		b->pos += 8 * size;
		return 0;
	}
	for (i = 0; i < size; i++)
		dst[i] = (uint8_t)latm_bits_in_get(b, 8);
	return 0;
}

#define LATM_PUT(b, nbits, value) ({					\
	int _r = latm_bits_out_put((b), (nbits), (value));		\
	if (_r < 0)							\
		return _r;						\
})

#define LATM_GET(b, nbits) ({						\
	int64_t _v = latm_bits_in_get((b), (nbits));			\
	if (_v < 0)							\
		return (int)_v;						\
	(uint32_t)_v;							\
})

#define LATM_SKIP(b, nbits) ((void)LATM_GET((b), (nbits)))

/**
 * Prepare the muxer for a stream with the given AudioSpecificConfig.
 * The ASC is padded with fill bits so that the StreamMuxConfig ends on a
 * byte boundary.
 */
static inline int latm_enc_init(struct latm_enc *enc, const uint8_t *asc, size_t asc_size)
{
	struct latm_bits_out b = { enc->head, sizeof(enc->head), 0 };

	if (asc_size == 0 || asc_size > LATM_ASC_MAXSIZE)
		return LATM_ERROR_BAD_ASC;

	spa_zero(*enc);

	LATM_PUT(&b, 1, 0);		/* useSameStreamMux: config present */
	LATM_PUT(&b, 1, 1);		/* audioMuxVersion */
	LATM_PUT(&b, 1, 0);		/* audioMuxVersionA */
	LATM_PUT(&b, 2, 0);		/* taraBufferFullness: LatmGetValue, 1 byte */
	LATM_PUT(&b, 8, 0xff);
	LATM_PUT(&b, 1, 1);		/* allStreamsSameTimeFraming */
	LATM_PUT(&b, 6, 0);		/* numSubFrames */
	LATM_PUT(&b, 4, 0);		/* numProgram */
	LATM_PUT(&b, 3, 0);		/* numLayer */
	LATM_PUT(&b, 2, 0);		/* ascLen: LatmGetValue, 1 byte */
	LATM_PUT(&b, 8, 8 * asc_size + 6);
	if (latm_bits_out_put_bytes(&b, asc, asc_size) < 0)
		return LATM_ERROR_NOSPC;
	LATM_PUT(&b, 6, 0);		/* fill bits to byte align */
	LATM_PUT(&b, 3, 0);		/* frameLengthType */
	LATM_PUT(&b, 8, LATM_BUFFER_FULLNESS);
	LATM_PUT(&b, 1, 0);		/* otherDataPresent */
	LATM_PUT(&b, 1, 0);		/* crcCheckPresent */

	if (b.pos % 8 != 0)
		return LATM_ERROR_BAD_FILL;

	enc->head_size = (uint8_t)(b.pos / 8);
	return 0;
}

/** Upper bound of the bytes added around an AAC frame by latm_enc_write() */
static inline size_t latm_enc_maxsize(const struct latm_enc *enc)
{
	return enc->head_size + LATM_LENGTH_MAXSIZE;
}

/** \return number of bytes written to dst, or error */
static inline int latm_enc_write(const struct latm_enc *enc,
		const uint8_t *src, size_t src_size, void *dst, size_t dst_size)
{
	struct latm_bits_out b = { (uint8_t *)dst, dst_size, 0 };
	size_t left = src_size;

	if (src_size > LATM_FRAME_MAXSIZE)
		return LATM_ERROR_NOSPC;
	if (dst_size < src_size + latm_enc_maxsize(enc))
		return LATM_ERROR_NOSPC;

	if (latm_bits_out_put_bytes(&b, enc->head, enc->head_size) < 0)
		return LATM_ERROR_NOSPC;

	while (left >= 255) {
		LATM_PUT(&b, 8, 255);
		left -= 255;
	}
	LATM_PUT(&b, 8, (uint32_t)left);

	if (latm_bits_out_put_bytes(&b, src, src_size) < 0)
		return LATM_ERROR_NOSPC;

	return (int)(b.pos / 8);
}

static inline void latm_dec_init(struct latm_dec *dec)
{
	spa_zero(*dec);
}

static inline int latm_get_value(struct latm_bits_in *b, uint32_t *value)
{
	unsigned int nbytes = LATM_GET(b, 2) + 1;
	uint32_t v = 0;

	while (nbytes-- > 0)
		v = (v << 8) | LATM_GET(b, 8);

	*value = v;
	return 0;
}

static inline int latm_dec_parse_asc(struct latm_bits_in *b, int *rate_index, int *channel_config)
{
	uint32_t aot, freq, chan;

	aot = LATM_GET(b, 5);
	if (aot != 2)
		return LATM_ERROR_BAD_AOT;

	freq = LATM_GET(b, 4);
	if (freq >= SPA_N_ELEMENTS(latm_rates))
		return LATM_ERROR_BAD_FREQUENCY;

	chan = LATM_GET(b, 4);
	if (chan != 1 && chan != 2)
		return LATM_ERROR_BAD_CHANNELS;

	if (LATM_GET(b, 1) != 0)	/* frameLengthFlag: 1024 samples only */
		return LATM_ERROR_UNIMPLEMENTED(5);
	if (LATM_GET(b, 1) == 1)	/* dependsOnCoreCoder */
		LATM_SKIP(b, 14);	/* coreCoderDelay */
	if (LATM_GET(b, 1) != 0)	/* extensionFlag */
		return LATM_ERROR_BAD_EXT;

	*rate_index = (int)freq;
	*channel_config = (int)chan;
	return 0;
}

/**
 * Demux one AudioMuxElement carrying one AAC-LC frame.
 *
 * A StreamMuxConfig in the element updates the decoder state. Without a
 * previous StreamMuxConfig an element that reuses it is rejected.
 * config_changed is set when the first or a different AudioSpecificConfig
 * is received; dec->asc then holds the new one.
 *
 * \return size of the AAC frame written to dst, or error
 */
static inline int latm_dec_read(struct latm_dec *dec, const void *src, size_t src_size,
		void *dst, size_t dst_size, bool *config_changed)
{
	struct latm_bits_in b = { (const uint8_t *)src, 8 * src_size, 0 };
	uint32_t len = 0, tmp;
	int res;

	*config_changed = false;

	if (LATM_GET(&b, 1) == 0) {		/* useSameStreamMux */
		struct latm_dec cfg;
		uint32_t version, value;

		version = LATM_GET(&b, 1);	/* audioMuxVersion */
		if (version == 1) {
			if (LATM_GET(&b, 1) != 0)	/* audioMuxVersionA */
				return LATM_ERROR_UNIMPLEMENTED(0);
			if ((res = latm_get_value(&b, &value)) < 0)	/* taraBufferFullness */
				return res;
		}

		if (LATM_GET(&b, 1) != 1)	/* allStreamsSameTimeFraming */
			return LATM_ERROR_UNIMPLEMENTED(1);
		if (LATM_GET(&b, 6) != 0)	/* numSubFrames */
			return LATM_ERROR_UNIMPLEMENTED(2);
		if (LATM_GET(&b, 4) != 0)	/* numProgram */
			return LATM_ERROR_UNIMPLEMENTED(3);
		if (LATM_GET(&b, 3) != 0)	/* numLayer */
			return LATM_ERROR_UNIMPLEMENTED(4);

		if (version == 1) {
			size_t start, used;
			uint32_t asc_len;

			if ((res = latm_get_value(&b, &asc_len)) < 0)
				return res;
			start = b.pos;
			if ((res = latm_dec_parse_asc(&b, &cfg.rate_index, &cfg.channel_config)) < 0)
				return res;
			used = b.pos - start;
			if (used > asc_len)
				return LATM_ERROR_BAD_FILL;
			if (b.pos + (asc_len - used) > b.end)
				return LATM_ERROR_EOF;
			b.pos += asc_len - used;	/* fill bits */
		} else {
			if ((res = latm_dec_parse_asc(&b, &cfg.rate_index, &cfg.channel_config)) < 0)
				return res;
		}

		if (LATM_GET(&b, 3) != 0)	/* frameLengthType: variable length only */
			return LATM_ERROR_BAD_FRAMELEN;
		LATM_SKIP(&b, 8);		/* latmBufferFullness */

		if (LATM_GET(&b, 1) == 1) {	/* otherDataPresent */
			if (version == 1) {
				if ((res = latm_get_value(&b, &value)) < 0)
					return res;
			} else {
				do {
					tmp = LATM_GET(&b, 1);	/* otherDataLenEsc */
					LATM_SKIP(&b, 8);	/* otherDataLenTmp */
				} while (tmp);
			}
		}
		if (LATM_GET(&b, 1) == 1)	/* crcCheckPresent */
			LATM_SKIP(&b, 8);	/* crcCheckSum */

		if (latm_make_asc(latm_rates[cfg.rate_index], cfg.channel_config, cfg.asc) < 0)
			return LATM_ERROR_BAD_ASC;

		*config_changed = !dec->have_config ||
			dec->rate_index != cfg.rate_index ||
			dec->channel_config != cfg.channel_config;
		dec->have_config = true;
		dec->rate_index = cfg.rate_index;
		dec->channel_config = cfg.channel_config;
		memcpy(dec->asc, cfg.asc, sizeof(dec->asc));
	} else if (!dec->have_config) {
		return LATM_ERROR_NO_CONFIG;
	}

	/* PayloadLengthInfo */
	do {
		tmp = LATM_GET(&b, 8);
		len += tmp;
		if (len > LATM_FRAME_MAXSIZE)
			return LATM_ERROR_BAD_FRAMELEN;
	} while (tmp == 255);

	/* PayloadMux, not necessarily byte aligned */
	if (len > dst_size)
		return LATM_ERROR_NOSPC;
	if ((res = latm_bits_in_get_bytes(&b, (uint8_t *)dst, len)) < 0)
		return res;

	return (int)len;
}

#undef LATM_PUT
#undef LATM_GET
#undef LATM_SKIP

#endif /* SPA_BLUEZ5_AAC_LATM_H */
