/* SPDX-FileCopyrightText: Copyright © 2026 cochcoder */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>

#include "g722/g722_enc_dec.h"

#define SAMPLES 8192

/* Whole-state comparisons below require no padding in either structure. */
_Static_assert(sizeof(unsigned int) == sizeof(int), "integer sizes differ");
_Static_assert(sizeof(g722_band_t) == 38 * sizeof(int), "band has padding");
_Static_assert(sizeof(g722_encode_state_t) == 108 * sizeof(int), "state has padding");

static uint32_t random_sample(uint32_t *seed)
{
	*seed ^= *seed << 13;
	*seed ^= *seed >> 17;
	*seed ^= *seed << 5;
	return *seed;
}

static void fill_samples(int16_t *pcm, unsigned int pattern)
{
	uint32_t seed = 1;

	for (unsigned int i = 0; i < SAMPLES; i++) {
		switch (pattern) {
		case 0: pcm[i] = 0; break;
		case 1: pcm[i] = i & 1 ? INT16_MIN : INT16_MAX; break;
		case 2: pcm[i] = (int32_t)(random_sample(&seed) >> 16) - 32768; break;
		case 3: pcm[i] = i % 320 == 0 ? INT16_MAX : 0; break;
		case 4: pcm[i] = (int32_t)((i * 131u) & 65535) - 32768; break;
		case 5: pcm[i] = i & 1024 ? INT16_MIN : INT16_MAX; break;
		}
	}
}

static uint64_t hash_bytes(const uint8_t *bytes, unsigned int count)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (unsigned int i = 0; i < count; i++)
		hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
	return hash;
}

static void test_vectors(void)
{
	/* Outputs of the original 64 kbit/s encoder, including its saturation behavior. */
	static const uint64_t expected_hash[] = {
		UINT64_C(0x9d2321b9c03b2f10), UINT64_C(0x6d509c92ef1a62d4),
		UINT64_C(0x62e93a517b072e66), UINT64_C(0xb744207e1b41b579),
		UINT64_C(0xf165960b87d1acfc), UINT64_C(0xbf71e5d4e2f0a039),
	};
	static const unsigned int chunks[] = { 2, 14, 16, 318, 320, 1024 };
	int16_t pcm[SAMPLES];
	uint8_t expected[SAMPLES / 2], actual[SAMPLES / 2];
	g722_encode_state_t state, whole;

	for (unsigned int pattern = 0; pattern < SPA_N_ELEMENTS(expected_hash); pattern++) {
		fill_samples(pcm, pattern);
		spa_assert_se(g722_encode_init(&state, 64000, G722_PACKED) == &state);
		spa_assert_se(g722_encode(&state, expected, pcm, SAMPLES) == SAMPLES / 2);
		spa_assert_se(hash_bytes(expected, sizeof(expected)) == expected_hash[pattern]);
		whole = state;

		for (unsigned int chunk = 0; chunk < SPA_N_ELEMENTS(chunks); chunk++) {
			unsigned int pos = 0, out = 0;

			g722_encode_init(&state, 64000, G722_PACKED);
			while (pos < SAMPLES) {
				unsigned int count = SPA_MIN(chunks[chunk], SAMPLES - pos);
				int written = g722_encode(&state, actual + out, pcm + pos, count);

				spa_assert_se(written == (int)count / 2);
				pos += count;
				out += written;
			}
			spa_assert_se(out == sizeof(expected));
			spa_assert_se(memcmp(actual, expected, sizeof(expected)) == 0);
			spa_assert_se(memcmp(&state, &whole, sizeof(state)) == 0);
		}
	}
}

static void test_qmf_history(void)
{
	int16_t pcm[64];
	uint8_t expected[32], actual[32];
	g722_encode_state_t whole, chunked;

	for (unsigned int i = 0; i < SPA_N_ELEMENTS(pcm); i++)
		pcm[i] = (int)(i * 977u % 65536u) - 32768;
	g722_encode_init(&whole, 64000, G722_PACKED);
	g722_encode_init(&chunked, 64000, G722_PACKED);
	spa_assert_se(g722_encode(&whole, expected, pcm, SPA_N_ELEMENTS(pcm)) == 32);
	for (int offset = 0; offset < 64; offset += 2) {
		spa_assert_se(g722_encode(&chunked, actual + offset / 2, pcm + offset, 2) == 1);
		for (int i = 0; i < 24; i++) {
			int sample = offset + 2 - 24 + i;
			spa_assert_se(chunked.x[i] == (sample < 0 ? 0 : pcm[sample]));
		}
	}
	spa_assert_se(memcmp(expected, actual, sizeof(expected)) == 0);
	spa_assert_se(memcmp(&whole, &chunked, sizeof(whole)) == 0);
}

static void test_itu_mode(void)
{
	int16_t pcm[SAMPLES];
	uint8_t expected[319], actual[319];
	g722_encode_state_t state, whole;

	fill_samples(pcm, 2);
	g722_encode_init(&state, 64000, G722_PACKED);
	state.itu_test_mode = 1;
	spa_assert_se(g722_encode(&state, expected, pcm, 319) == 319);
	spa_assert_se(hash_bytes(expected, sizeof(expected)) == UINT64_C(0x799042c09eb61ff2));
	whole = state;
	g722_encode_init(&state, 64000, G722_PACKED);
	state.itu_test_mode = 1;
	for (unsigned int i = 0; i < 319; i++)
		spa_assert_se(g722_encode(&state, actual + i, pcm + i, 1) == 1);
	spa_assert_se(memcmp(actual, expected, 319) == 0);
	spa_assert_se(memcmp(&state, &whole, sizeof(state)) == 0);
}

int main(void)
{
	test_vectors();
	test_qmf_history();
	test_itu_mode();
	return 0;
}
