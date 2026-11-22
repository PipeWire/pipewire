/* Spa A2DP AAC LATM tests */
/* SPDX-FileCopyrightText: Copyright © 2026 Nils Schimmelmann */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>

#include <spa/utils/defs.h>

#include "aac-latm.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
		abort(); \
	} \
} while (0)

static void fill(uint8_t *buf, size_t size, uint8_t seed)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = (uint8_t)(seed + 7 * i);
}

static void test_asc(void)
{
	uint8_t asc[2];

	/* AAC-LC, 44.1 kHz, stereo: 0001 0010 0001 0000 */
	CHECK(latm_make_asc(44100, 2, asc) == 0);
	CHECK(asc[0] == 0x12 && asc[1] == 0x10);

	/* AAC-LC, 48 kHz, mono */
	CHECK(latm_make_asc(48000, 1, asc) == 0);
	CHECK(asc[0] == 0x11 && asc[1] == 0x88);

	CHECK(latm_make_asc(44101, 2, asc) == LATM_ERROR_BAD_FREQUENCY);
	CHECK(latm_make_asc(44100, 6, asc) == LATM_ERROR_BAD_CHANNELS);
}

/* StreamMuxConfig headers as written by the FDK-AAC encoder (audioMuxVersion 2, LATM MCP1) */
static void test_fdk_header(void)
{
	static const uint8_t fdk_44100_2[] = { 0x47, 0xfc, 0x00, 0x00, 0xb0, 0x90, 0x80, 0x03, 0x00 };
	static const uint8_t fdk_48000_1[] = { 0x47, 0xfc, 0x00, 0x00, 0xb0, 0x8c, 0x40, 0x03, 0x00 };
	struct latm_enc enc;
	uint8_t asc[2];

	CHECK(latm_make_asc(44100, 2, asc) == 0);
	CHECK(latm_enc_init(&enc, asc, sizeof(asc)) == 0);
	CHECK(enc.head_size == sizeof(fdk_44100_2));
	CHECK(memcmp(enc.head, fdk_44100_2, sizeof(fdk_44100_2)) == 0);

	CHECK(latm_make_asc(48000, 1, asc) == 0);
	CHECK(latm_enc_init(&enc, asc, sizeof(asc)) == 0);
	CHECK(enc.head_size == sizeof(fdk_48000_1));
	CHECK(memcmp(enc.head, fdk_48000_1, sizeof(fdk_48000_1)) == 0);
}

static void test_bits(void)
{
	uint8_t buf[8] = {0};
	struct latm_bits_out o = { buf, sizeof(buf), 0 };
	struct latm_bits_in i = { buf, 8 * sizeof(buf), 0 };
	uint8_t bytes[3] = { 0xff, 0xcc, 0xb1 }, out[3];

	CHECK(latm_bits_out_put(&o, 1, 1) == 0);
	CHECK(latm_bits_out_put(&o, 2, 2) == 0);
	CHECK(latm_bits_out_put(&o, 7, 0x7f) == 0);
	CHECK(o.pos == 10);
	CHECK(latm_bits_out_put_bytes(&o, bytes, sizeof(bytes)) == 0);
	CHECK(o.pos == 34);
	CHECK(latm_bits_out_put(&o, 32, 0) == LATM_ERROR_NOSPC);
	CHECK(latm_bits_out_put(&o, 30, 0) == 0);
	CHECK(latm_bits_out_put(&o, 1, 0) == LATM_ERROR_NOSPC);

	CHECK(latm_bits_in_get(&i, 1) == 1);
	CHECK(latm_bits_in_get(&i, 2) == 2);
	CHECK(latm_bits_in_get(&i, 7) == 0x7f);
	CHECK(latm_bits_in_get_bytes(&i, out, sizeof(out)) == 0);
	CHECK(memcmp(out, bytes, sizeof(out)) == 0);
	CHECK(latm_bits_in_get(&i, 30) == 0);
	CHECK(latm_bits_in_get(&i, 1) == LATM_ERROR_EOF);
}

static void roundtrip(uint32_t rate, uint32_t channels, size_t size)
{
	struct latm_enc enc;
	struct latm_dec dec;
	uint8_t asc[2], src[1100], mux[1300], out[1100];
	bool changed;
	int res;

	CHECK(latm_make_asc(rate, channels, asc) == 0);
	CHECK(latm_enc_init(&enc, asc, sizeof(asc)) == 0);
	CHECK(enc.head_size == 9);
	CHECK(latm_enc_maxsize(&enc) == 9 + LATM_LENGTH_MAXSIZE);

	fill(src, size, (uint8_t)size);
	res = latm_enc_write(&enc, src, size, mux, sizeof(mux));
	CHECK(res > (int)size);
	CHECK((size_t)res <= size + latm_enc_maxsize(&enc));

	latm_dec_init(&dec);
	res = latm_dec_read(&dec, mux, res, out, sizeof(out), &changed);
	CHECK(res == (int)size);
	CHECK(changed);
	CHECK(memcmp(out, src, size) == 0);
	CHECK(dec.asc[0] == asc[0] && dec.asc[1] == asc[1]);
	CHECK(latm_rates[dec.rate_index] == rate);
	CHECK((uint32_t)dec.channel_config == channels);
}

static void test_roundtrip(void)
{
	static const size_t sizes[] = { 0, 1, 4, 254, 255, 256, 509, 510, 511, 765, 1019 };
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(sizes); i++) {
		roundtrip(44100, 2, sizes[i]);
		roundtrip(48000, 1, sizes[i]);
		roundtrip(96000, 2, sizes[i]);
	}
}

static void test_limits(void)
{
	struct latm_enc enc;
	uint8_t asc[2], src[LATM_FRAME_MAXSIZE + 1] = {0}, mux[256];

	CHECK(latm_make_asc(48000, 2, asc) == 0);
	CHECK(latm_enc_init(&enc, asc, 0) == LATM_ERROR_BAD_ASC);
	CHECK(latm_enc_init(&enc, asc, LATM_ASC_MAXSIZE + 1) == LATM_ERROR_BAD_ASC);
	CHECK(latm_enc_init(&enc, asc, sizeof(asc)) == 0);

	CHECK(latm_enc_write(&enc, src, sizeof(src), mux, sizeof(mux)) == LATM_ERROR_NOSPC);
	/* frame fits the limit, but not the destination */
	CHECK(latm_enc_write(&enc, src, 100, mux, 100) == LATM_ERROR_NOSPC);
	CHECK(latm_enc_write(&enc, src, 100, mux, 100 + latm_enc_maxsize(&enc)) > 0);
}

/* Same stream mux, with the config of the earlier element */
static void test_same_stream_mux(void)
{
	struct latm_enc enc;
	struct latm_dec dec;
	uint8_t asc[2], src[16], mux[64], out[64], reuse[32];
	bool changed;
	int res, size;

	CHECK(latm_make_asc(44100, 2, asc) == 0);
	CHECK(latm_enc_init(&enc, asc, sizeof(asc)) == 0);
	fill(src, sizeof(src), 3);
	size = latm_enc_write(&enc, src, sizeof(src), mux, sizeof(mux));
	CHECK(size > 0);

	/* useSameStreamMux=1 element: one bit, then payload length and payload */
	memset(reuse, 0, sizeof(reuse));
	{
		struct latm_bits_out o = { reuse, sizeof(reuse), 0 };
		CHECK(latm_bits_out_put(&o, 1, 1) == 0);
		CHECK(latm_bits_out_put(&o, 8, sizeof(src)) == 0);
		CHECK(latm_bits_out_put_bytes(&o, src, sizeof(src)) == 0);
	}

	latm_dec_init(&dec);
	/* no config seen yet */
	CHECK(latm_dec_read(&dec, reuse, sizeof(reuse), out, sizeof(out), &changed)
			== LATM_ERROR_NO_CONFIG);
	CHECK(!dec.have_config);

	res = latm_dec_read(&dec, mux, size, out, sizeof(out), &changed);
	CHECK(res == (int)sizeof(src) && changed);

	/* unaligned payload, config reused */
	res = latm_dec_read(&dec, reuse, sizeof(reuse), out, sizeof(out), &changed);
	CHECK(res == (int)sizeof(src) && !changed);
	CHECK(memcmp(out, src, sizeof(src)) == 0);

	/* config repeated and unchanged */
	res = latm_dec_read(&dec, mux, size, out, sizeof(out), &changed);
	CHECK(res == (int)sizeof(src) && !changed);
}

static void test_config_change(void)
{
	struct latm_enc enc1, enc2;
	struct latm_dec dec;
	uint8_t asc[2], src[8], mux1[64], mux2[64], out[64];
	bool changed;
	int size1, size2;

	fill(src, sizeof(src), 1);
	CHECK(latm_make_asc(44100, 2, asc) == 0);
	CHECK(latm_enc_init(&enc1, asc, sizeof(asc)) == 0);
	size1 = latm_enc_write(&enc1, src, sizeof(src), mux1, sizeof(mux1));
	CHECK(latm_make_asc(48000, 2, asc) == 0);
	CHECK(latm_enc_init(&enc2, asc, sizeof(asc)) == 0);
	size2 = latm_enc_write(&enc2, src, sizeof(src), mux2, sizeof(mux2));

	latm_dec_init(&dec);
	CHECK(latm_dec_read(&dec, mux1, size1, out, sizeof(out), &changed) == (int)sizeof(src));
	CHECK(changed && latm_rates[dec.rate_index] == 44100);
	CHECK(latm_dec_read(&dec, mux2, size2, out, sizeof(out), &changed) == (int)sizeof(src));
	CHECK(changed && latm_rates[dec.rate_index] == 48000);
	CHECK(latm_dec_read(&dec, mux2, size2, out, sizeof(out), &changed) == (int)sizeof(src));
	CHECK(!changed);
}

/* audioMuxVersion=0: AudioSpecificConfig inline, payload not byte aligned */
static void test_version0(void)
{
	struct latm_dec dec;
	uint8_t buf[64] = {0}, src[20], out[64], asc[2];
	struct latm_bits_out o = { buf, sizeof(buf), 0 };
	bool changed;
	int res;

	fill(src, sizeof(src), 9);
	CHECK(latm_make_asc(44100, 2, asc) == 0);

	CHECK(latm_bits_out_put(&o, 1, 0) == 0);	/* useSameStreamMux */
	CHECK(latm_bits_out_put(&o, 1, 0) == 0);	/* audioMuxVersion */
	CHECK(latm_bits_out_put(&o, 1, 1) == 0);	/* allStreamsSameTimeFraming */
	CHECK(latm_bits_out_put(&o, 6, 0) == 0);
	CHECK(latm_bits_out_put(&o, 4, 0) == 0);
	CHECK(latm_bits_out_put(&o, 3, 0) == 0);
	CHECK(latm_bits_out_put_bytes(&o, asc, sizeof(asc)) == 0);
	CHECK(latm_bits_out_put(&o, 3, 0) == 0);	/* frameLengthType */
	CHECK(latm_bits_out_put(&o, 8, 0xff) == 0);
	CHECK(latm_bits_out_put(&o, 1, 0) == 0);	/* otherDataPresent */
	CHECK(latm_bits_out_put(&o, 1, 0) == 0);	/* crcCheckPresent */
	CHECK(o.pos % 8 != 0);
	CHECK(latm_bits_out_put(&o, 8, sizeof(src)) == 0);
	CHECK(latm_bits_out_put_bytes(&o, src, sizeof(src)) == 0);

	latm_dec_init(&dec);
	res = latm_dec_read(&dec, buf, (o.pos + 7) / 8, out, sizeof(out), &changed);
	CHECK(res == (int)sizeof(src) && changed);
	CHECK(memcmp(out, src, sizeof(src)) == 0);
}

static void test_malformed(void)
{
	struct latm_enc enc;
	struct latm_dec dec;
	uint8_t asc[2], src[40], mux[128], out[128];
	bool changed;
	int size;
	size_t i;

	fill(src, sizeof(src), 5);
	CHECK(latm_make_asc(44100, 2, asc) == 0);
	CHECK(latm_enc_init(&enc, asc, sizeof(asc)) == 0);
	size = latm_enc_write(&enc, src, sizeof(src), mux, sizeof(mux));
	CHECK(size > 0);

	/* every truncation is rejected without reading past the end */
	for (i = 0; i < (size_t)size; i++) {
		latm_dec_init(&dec);
		CHECK(latm_dec_read(&dec, mux, i, out, sizeof(out), &changed) < 0);
	}

	/* destination too small */
	latm_dec_init(&dec);
	CHECK(latm_dec_read(&dec, mux, size, out, sizeof(src) - 1, &changed) == LATM_ERROR_NOSPC);

	/* single bit corruption never reads out of bounds nor reports > dst_size */
	for (i = 0; i < 8 * (size_t)size; i++) {
		uint8_t bad[128];
		int res;

		memcpy(bad, mux, size);
		bad[i / 8] ^= (uint8_t)(0x80 >> (i % 8));
		latm_dec_init(&dec);
		res = latm_dec_read(&dec, bad, size, out, sizeof(out), &changed);
		CHECK(res <= (int)sizeof(out));
	}

	/* unsupported object type (AAC Main) */
	{
		struct latm_enc bad_enc;
		uint8_t bad_asc[2] = { (uint8_t)(1 << 3 | 2), 0x10 };

		CHECK(latm_enc_init(&bad_enc, bad_asc, sizeof(bad_asc)) == 0);
		size = latm_enc_write(&bad_enc, src, sizeof(src), mux, sizeof(mux));
		latm_dec_init(&dec);
		CHECK(latm_dec_read(&dec, mux, size, out, sizeof(out), &changed) == LATM_ERROR_BAD_AOT);
		CHECK(!dec.have_config);
	}

	/* empty and garbage input */
	latm_dec_init(&dec);
	CHECK(latm_dec_read(&dec, mux, 0, out, sizeof(out), &changed) < 0);
	memset(mux, 0xff, sizeof(mux));
	CHECK(latm_dec_read(&dec, mux, sizeof(mux), out, sizeof(out), &changed) < 0);
}

int main(void)
{
	test_asc();
	test_fdk_header();
	test_bits();
	test_roundtrip();
	test_limits();
	test_same_stream_mux();
	test_config_change();
	test_version0();
	test_malformed();
	return 0;
}
