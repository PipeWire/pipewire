/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>

#include <spa/control/ump-utils.h>

#define spa_assert(cond)						\
	do {								\
		if (SPA_UNLIKELY(!(cond))) {				\
			fprintf(stderr, "FAIL: %s at %s:%d\n",		\
				#cond, __FILE__, __LINE__);		\
			abort();					\
		}							\
	} while (0)

/* spa_ump_from_midi returns bytes (size * 4) */
/* spa_ump_to_midi returns number of MIDI bytes written */

static void test_ump_from_midi_note_on(void)
{
	uint8_t midi[] = { 0x90, 0x3c, 0x7f };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 4);
	spa_assert(midi_size == 0);
	spa_assert(ump[0] == 0x20903c7f);
}

static void test_ump_from_midi_program_change(void)
{
	uint8_t midi[] = { 0xc0, 0x05 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 4);
	spa_assert(midi_size == 0);
	spa_assert(ump[0] == 0x20c00500);
}

static void test_ump_from_midi_sysex_complete(void)
{
	/* Short sysex that fits in one UMP packet: F0 01 02 03 F7 */
	uint8_t midi[] = { 0xf0, 0x01, 0x02, 0x03, 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* message type 0x3, status 0x0 (complete), 3 bytes */
	spa_assert((ump[0] >> 20) == 0x300);
	spa_assert(((ump[0] >> 16) & 0xf) == 3);
	spa_assert(state == 0);
}

static void test_ump_from_midi_sysex_complete_max(void)
{
	/* Sysex with exactly 6 data bytes: F0 01 02 03 04 05 06 F7 */
	uint8_t midi[] = { 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x0 (complete), 6 bytes */
	spa_assert((ump[0] >> 20) == 0x300);
	spa_assert(((ump[0] >> 16) & 0xf) == 6);
	spa_assert(state == 0);
}

static void test_ump_from_midi_sysex_multi_packet(void)
{
	/* Sysex longer than 6 data bytes: F0 01 02 03 04 05 06 07 08 F7 */
	uint8_t midi[] = { 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4];
	uint64_t state = 0;
	int size;

	/* First call: Start, 6 data bytes */
	memset(ump, 0, sizeof(ump));
	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	/* status 0x1 (start), 6 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x1);
	spa_assert(((ump[0] >> 16) & 0xf) == 6);
	spa_assert(state == 2);

	/* Second call: End, 2 data bytes */
	memset(ump, 0, sizeof(ump));
	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	/* status 0x3 (end) */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x3);
	spa_assert(((ump[0] >> 16) & 0xf) == 2);
	spa_assert(state == 0);
	spa_assert(midi_size == 0);
}

static void test_ump_from_midi_sysex_continue(void)
{
	/* Sysex with 13 data bytes needs start + continue + end */
	uint8_t midi[] = { 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
			   0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
			   0x0d, 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4];
	uint64_t state = 0;
	int size;

	/* First: Start, 6 bytes */
	memset(ump, 0, sizeof(ump));
	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(((ump[0] >> 20) & 0xf) == 0x1);
	spa_assert(((ump[0] >> 16) & 0xf) == 6);
	spa_assert(state == 2);

	/* Second: Continue, 6 bytes */
	memset(ump, 0, sizeof(ump));
	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(((ump[0] >> 20) & 0xf) == 0x2);
	spa_assert(((ump[0] >> 16) & 0xf) == 6);
	spa_assert(state == 2);

	/* Third: End, 1 byte */
	memset(ump, 0, sizeof(ump));
	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(((ump[0] >> 20) & 0xf) == 0x3);
	spa_assert(((ump[0] >> 16) & 0xf) == 1);
	spa_assert(state == 0);
	spa_assert(midi_size == 0);
}

static void test_ump_from_midi_sysex_minimal(void)
{
	/* Minimal sysex: F0 F7 */
	uint8_t midi[] = { 0xf0, 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	/* status 0x0 (complete), 0 data bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x0);
	spa_assert(((ump[0] >> 16) & 0xf) == 0);
	spa_assert(state == 0);
	spa_assert(midi_size == 0);
}

static void test_ump_to_midi_note_on(void)
{
	uint32_t ump[] = { 0x20903c7f };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 3);
	spa_assert(midi[0] == 0x90);
	spa_assert(midi[1] == 0x3c);
	spa_assert(midi[2] == 0x7f);
}

static void test_ump_to_midi_sysex_complete(void)
{
	/* Complete sysex: status 0x0, 3 bytes */
	uint32_t ump[] = { 0x30030102, 0x03000000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 5);
	spa_assert(midi[0] == 0xf0);
	spa_assert(midi[1] == 0x01);
	spa_assert(midi[2] == 0x02);
	spa_assert(midi[3] == 0x03);
	spa_assert(midi[4] == 0xf7);
}

static void test_ump_to_midi_sysex_start_end(void)
{
	/* Start: status 0x1, 3 bytes */
	uint32_t ump_start[] = { 0x30130102, 0x03000000 };
	/* End: status 0x3, 2 bytes */
	uint32_t ump_end[] = { 0x30320405, 0x00000000 };
	const uint32_t *p;
	size_t ump_size;
	uint8_t midi[8];
	uint64_t state = 0;

	p = ump_start;
	ump_size = sizeof(ump_start);
	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 4);
	spa_assert(midi[0] == 0xf0);
	spa_assert(midi[1] == 0x01);
	spa_assert(midi[2] == 0x02);
	spa_assert(midi[3] == 0x03);

	p = ump_end;
	ump_size = sizeof(ump_end);
	size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 3);
	spa_assert(midi[0] == 0x04);
	spa_assert(midi[1] == 0x05);
	spa_assert(midi[2] == 0xf7);
}

static void test_roundtrip_sysex_short(void)
{
	/* Roundtrip: MIDI -> UMP -> MIDI for short sysex */
	uint8_t orig[] = { 0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7 };
	uint8_t *p = orig;
	size_t midi_size = sizeof(orig);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int ump_bytes = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(ump_bytes == 8);
	spa_assert(midi_size == 0);
	spa_assert(state == 0);

	/* Convert back */
	const uint32_t *up = ump;
	size_t usize = ump_bytes;
	uint8_t result[8];
	state = 0;

	int rsize = spa_ump_to_midi(&up, &usize, result, sizeof(result), &state);
	spa_assert(rsize == (int)sizeof(orig));
	spa_assert(memcmp(result, orig, sizeof(orig)) == 0);
}

static void test_roundtrip_sysex_long(void)
{
	/* Roundtrip: MIDI -> UMP -> MIDI for long sysex (10 data bytes) */
	uint8_t orig[] = { 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05,
			   0x06, 0x07, 0x08, 0x09, 0x0a, 0xf7 };
	uint8_t *p = orig;
	size_t midi_size = sizeof(orig);
	uint32_t ump[8];
	uint64_t state = 0;
	int total_bytes = 0;

	/* Encode all UMP packets */
	while (midi_size > 0) {
		memset(&ump[total_bytes / 4], 0, 8);
		int s = spa_ump_from_midi(&p, &midi_size, &ump[total_bytes / 4], 16, 0, &state);
		spa_assert(s == 8);
		total_bytes += s;
	}
	spa_assert(state == 0);

	/* Decode back */
	const uint32_t *up = ump;
	size_t usize = total_bytes;
	uint8_t result[32];
	int rsize = 0;
	state = 0;

	while (usize > 0) {
		int s = spa_ump_to_midi(&up, &usize, &result[rsize], sizeof(result) - rsize, &state);
		if (s <= 0)
			break;
		rsize += s;
	}
	spa_assert(rsize == (int)sizeof(orig));
	spa_assert(memcmp(result, orig, sizeof(orig)) == 0);
}

static void test_roundtrip_note_on(void)
{
	uint8_t orig[] = { 0x90, 0x3c, 0x7f };
	uint8_t *p = orig;
	size_t midi_size = sizeof(orig);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int ump_bytes = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(ump_bytes == 4);

	const uint32_t *up = ump;
	size_t usize = ump_bytes;
	uint8_t result[8];
	state = 0;

	int rsize = spa_ump_to_midi(&up, &usize, result, sizeof(result), &state);
	spa_assert(rsize == 3);
	spa_assert(memcmp(result, orig, 3) == 0);
}

static void test_ump_from_midi_system_realtime(void)
{
	uint8_t midi[] = { 0xf8 }; /* timing clock */
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 4);
	spa_assert((ump[0] >> 16) == 0x10f8);
}

static void test_ump_from_midi_channel_pressure(void)
{
	uint8_t midi[] = { 0xd0, 0x40 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 4);
	spa_assert(midi_size == 0);
	spa_assert(ump[0] == 0x20d04000);
}

static void test_ump_from_midi_song_position(void)
{
	/* F2 + LSB + MSB */
	uint8_t midi[] = { 0xf2, 0x10, 0x20 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 4);
	spa_assert(midi_size == 0);
	spa_assert(ump[0] == 0x10f21020);
}

static void test_ump_from_midi_mtc_quarter_frame(void)
{
	/* F1 + data */
	uint8_t midi[] = { 0xf1, 0x30 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 4);
	spa_assert(midi_size == 0);
	spa_assert(ump[0] == 0x10f13000);
}

static void test_ump_from_midi_song_select(void)
{
	/* F3 + song number */
	uint8_t midi[] = { 0xf3, 0x05 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 4);
	spa_assert(midi_size == 0);
	spa_assert(ump[0] == 0x10f30500);
}

static void test_ump_to_midi_system_realtime(void)
{
	/* Type 0x1, F8 timing clock */
	uint32_t ump[] = { 0x10f80000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 1);
	spa_assert(midi[0] == 0xf8);
}

static void test_ump_to_midi_song_position(void)
{
	/* Type 0x1, F2 + LSB + MSB */
	uint32_t ump[] = { 0x10f21020 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 3);
	spa_assert(midi[0] == 0xf2);
	spa_assert(midi[1] == 0x10);
	spa_assert(midi[2] == 0x20);
}

static void test_ump_to_midi_mtc_quarter_frame(void)
{
	/* Type 0x1, F1 + data */
	uint32_t ump[] = { 0x10f13000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0xf1);
	spa_assert(midi[1] == 0x30);
}

static void test_ump_to_midi_program_change(void)
{
	/* Type 0x2, C0 + program */
	uint32_t ump[] = { 0x20c00500 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0xc0);
	spa_assert(midi[1] == 0x05);
}

static void test_ump_to_midi_channel_pressure(void)
{
	/* Type 0x2, D0 + pressure */
	uint32_t ump[] = { 0x20d04000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0xd0);
	spa_assert(midi[1] == 0x40);
}

static void test_ump_to_midi_sysex_continue(void)
{
	/* Start: status 0x1, 3 bytes */
	uint32_t ump_start[] = { 0x30130102, 0x03000000 };
	/* Continue: status 0x2, 2 bytes */
	uint32_t ump_cont[] = { 0x30220405, 0x00000000 };
	/* End: status 0x3, 1 byte */
	uint32_t ump_end[] = { 0x30310600, 0x00000000 };
	const uint32_t *p;
	size_t ump_size;
	uint8_t midi[8];
	uint64_t state = 0;
	int size;

	p = ump_start;
	ump_size = sizeof(ump_start);
	size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 4);
	spa_assert(midi[0] == 0xf0);
	spa_assert(midi[1] == 0x01);
	spa_assert(midi[2] == 0x02);
	spa_assert(midi[3] == 0x03);

	p = ump_cont;
	ump_size = sizeof(ump_cont);
	size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0x04);
	spa_assert(midi[1] == 0x05);

	p = ump_end;
	ump_size = sizeof(ump_end);
	size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0x06);
	spa_assert(midi[1] == 0xf7);
}

static void test_roundtrip_program_change(void)
{
	uint8_t orig[] = { 0xc0, 0x05 };
	uint8_t *p = orig;
	size_t midi_size = sizeof(orig);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int ump_bytes = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(ump_bytes == 4);

	const uint32_t *up = ump;
	size_t usize = ump_bytes;
	uint8_t result[8];
	state = 0;

	int rsize = spa_ump_to_midi(&up, &usize, result, sizeof(result), &state);
	spa_assert(rsize == 2);
	spa_assert(memcmp(result, orig, 2) == 0);
}

static void test_roundtrip_channel_pressure(void)
{
	uint8_t orig[] = { 0xd0, 0x40 };
	uint8_t *p = orig;
	size_t midi_size = sizeof(orig);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int ump_bytes = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(ump_bytes == 4);

	const uint32_t *up = ump;
	size_t usize = ump_bytes;
	uint8_t result[8];
	state = 0;

	int rsize = spa_ump_to_midi(&up, &usize, result, sizeof(result), &state);
	spa_assert(rsize == 2);
	spa_assert(memcmp(result, orig, 2) == 0);
}

static void test_roundtrip_song_position(void)
{
	uint8_t orig[] = { 0xf2, 0x10, 0x20 };
	uint8_t *p = orig;
	size_t midi_size = sizeof(orig);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int ump_bytes = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(ump_bytes == 4);

	const uint32_t *up = ump;
	size_t usize = ump_bytes;
	uint8_t result[8];
	state = 0;

	int rsize = spa_ump_to_midi(&up, &usize, result, sizeof(result), &state);
	spa_assert(rsize == 3);
	spa_assert(memcmp(result, orig, 3) == 0);
}

static void test_roundtrip_system_realtime(void)
{
	uint8_t orig[] = { 0xf8 };
	uint8_t *p = orig;
	size_t midi_size = sizeof(orig);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int ump_bytes = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(ump_bytes == 4);

	const uint32_t *up = ump;
	size_t usize = ump_bytes;
	uint8_t result[8];
	state = 0;

	int rsize = spa_ump_to_midi(&up, &usize, result, sizeof(result), &state);
	spa_assert(rsize == 1);
	spa_assert(result[0] == 0xf8);
}

static void test_ump_from_midi_sysex_trailing_f0(void)
{
	/* Sysex start with trailing F0: F0 01 02 03 F0 */
	uint8_t midi[] = { 0xf0, 0x01, 0x02, 0x03, 0xf0 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x1 (start), 3 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x1);
	spa_assert(((ump[0] >> 16) & 0xf) == 3);
	spa_assert(state == 2);
}

static void test_ump_from_midi_continuation_trailing_f0(void)
{
	/* Continuation with trailing F0, sysex already active */
	uint8_t midi[] = { 0x01, 0x02, 0x03, 0xf0 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 2;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x2 (continue), 3 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x2);
	spa_assert(((ump[0] >> 16) & 0xf) == 3);
	spa_assert(state == 2);
}

static void test_ump_from_midi_sysex_split_with_f0(void)
{
	/* Full sysex split using F0 continuation markers */
	uint8_t midi1[] = { 0xf0, 0x01, 0x02, 0x03, 0xf0 };
	uint8_t *p = midi1;
	size_t midi_size = sizeof(midi1);
	uint32_t ump[4];
	uint64_t state = 0;

	/* First call: Start, 3 data bytes */
	memset(ump, 0, sizeof(ump));
	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	spa_assert(((ump[0] >> 20) & 0xf) == 0x1);
	spa_assert(((ump[0] >> 16) & 0xf) == 3);
	spa_assert(state == 2);

	/* Second call: continuation data with F7 end */
	uint8_t midi2[] = { 0x04, 0x05, 0xf7 };
	p = midi2;
	midi_size = sizeof(midi2);
	memset(ump, 0, sizeof(ump));
	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	spa_assert(((ump[0] >> 20) & 0xf) == 0x3);
	spa_assert(((ump[0] >> 16) & 0xf) == 2);
	spa_assert(state == 0);
}

static void test_ump_from_midi_f7_continuation(void)
{
	/* F7 continuation with data, no prior sysex */
	uint8_t midi[] = { 0xf7, 0x01, 0x02, 0x03 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x2 (continue), 3 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x2);
	spa_assert(((ump[0] >> 16) & 0xf) == 3);
	spa_assert(state == 2);
}

static void test_ump_from_midi_f7_continuation_with_end(void)
{
	/* F7 continuation with data and F7 end, no prior sysex */
	uint8_t midi[] = { 0xf7, 0x01, 0x02, 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x3 (end), 2 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x3);
	spa_assert(((ump[0] >> 16) & 0xf) == 2);
	spa_assert(state == 0);
}

static void test_ump_from_midi_f7_continuation_active(void)
{
	/* F7 continuation when sysex already active */
	uint8_t midi[] = { 0xf7, 0x01, 0x02, 0x03 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 2;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x2 (continue), 3 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x2);
	spa_assert(((ump[0] >> 16) & 0xf) == 3);
	spa_assert(state == 2);
}

static void test_ump_from_midi_bare_f7_end(void)
{
	/* Bare F7 ending an active sysex */
	uint8_t midi[] = { 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 2;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x3 (end), 0 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x3);
	spa_assert(((ump[0] >> 16) & 0xf) == 0);
	spa_assert(state == 0);
}

static void test_ump_from_midi_bare_f7_complete(void)
{
	/* F0 alone in first call, then bare F7 → Complete */
	uint8_t midi1[] = { 0xf0 };
	uint8_t *p = midi1;
	size_t midi_size = sizeof(midi1);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 0);
	spa_assert(midi_size == 0);
	spa_assert(state == 1);

	uint8_t midi2[] = { 0xf7 };
	p = midi2;
	midi_size = sizeof(midi2);
	memset(ump, 0, sizeof(ump));

	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	/* status 0x0 (complete), 0 bytes */
	spa_assert(((ump[0] >> 20) & 0xf) == 0x0);
	spa_assert(((ump[0] >> 16) & 0xf) == 0);
	spa_assert(state == 0);
}

static void test_ump_from_midi_bare_f7_orphan(void)
{
	/* Bare F7 with no sysex in progress: consume, no UMP */
	uint8_t midi[] = { 0xf7 };
	uint8_t *p = midi;
	size_t midi_size = sizeof(midi);
	uint32_t ump[4] = {0};
	uint64_t state = 0;

	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 0);
	spa_assert(midi_size == 0);
	spa_assert(state == 0);
}

static void test_ump_from_midi_sysex_split_with_f7(void)
{
	/* Full sysex split with F7 continuation markers */
	uint8_t midi1[] = { 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
	uint8_t *p = midi1;
	size_t midi_size = sizeof(midi1);
	uint32_t ump[4];
	uint64_t state = 0;

	/* First call: Start, 6 data bytes */
	memset(ump, 0, sizeof(ump));
	int size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	spa_assert(((ump[0] >> 20) & 0xf) == 0x1);
	spa_assert(((ump[0] >> 16) & 0xf) == 6);
	spa_assert(state == 2);

	/* Second call with F7 continuation marker */
	uint8_t midi2[] = { 0xf7, 0x07, 0x08, 0xf7 };
	p = midi2;
	midi_size = sizeof(midi2);
	memset(ump, 0, sizeof(ump));
	size = spa_ump_from_midi(&p, &midi_size, ump, sizeof(ump), 0, &state);
	spa_assert(size == 8);
	spa_assert(midi_size == 0);
	spa_assert(((ump[0] >> 20) & 0xf) == 0x3);
	spa_assert(((ump[0] >> 16) & 0xf) == 2);
	spa_assert(state == 0);
}

static void test_ump_to_midi2_channel_pressure(void)
{
	/* MIDI 2.0 channel pressure: type 4, status D, channel 0 */
	uint32_t ump[] = { 0x40D00000, 0xFC000000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0xd0);
	spa_assert(midi[1] == 0x7e);
}

static void test_ump_to_midi2_note_on(void)
{
	/* MIDI 2.0 note on: type 4, status 9, channel 0, note 0x3C */
	uint32_t ump[] = { 0x40903C00, 0xFC000000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 3);
	spa_assert(midi[0] == 0x90);
	spa_assert(midi[1] == 0x3c);
	spa_assert(midi[2] == 0x7e);
}

static void test_ump_to_midi2_pitch_bend(void)
{
	/* MIDI 2.0 pitch bend: type 4, status E, channel 0
	 * 32-bit value 0x80A00000 → 14-bit = 0x80A00000 >> 18 = 0x2028
	 * LSB = 0x28, MSB = 0x40 */
	uint32_t ump[] = { 0x40E00000, 0x80A00000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 3);
	spa_assert(midi[0] == 0xe0);
	spa_assert(midi[1] == 0x28);
	spa_assert(midi[2] == 0x40);
}

static void test_ump_to_midi2_program_change_with_bank(void)
{
	/* MIDI 2.0 program change with bank valid, bank bytes have bit 7 set
	 * to verify masking: MSB=0x83 → 0x03, LSB=0x87 → 0x07 */
	uint32_t ump[] = { 0x40C00001, 0x05008387 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;
	int size;

	/* First call: Bank Select MSB (CC #0) */
	size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 3);
	spa_assert(midi[0] == 0xb0);
	spa_assert(midi[1] == 0x00);
	spa_assert(midi[2] == 0x03);

	/* Second call: Bank Select LSB (CC #32) */
	size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 3);
	spa_assert(midi[0] == 0xb0);
	spa_assert(midi[1] == 0x20);
	spa_assert(midi[2] == 0x07);

	/* Third call: Program Change */
	size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0xc0);
	spa_assert(midi[1] == 0x05);
}

static void test_ump_to_midi2_program_change_no_bank(void)
{
	/* MIDI 2.0 program change without bank (option_flags = 0) */
	uint32_t ump[] = { 0x40C00000, 0x05000000 };
	const uint32_t *p = ump;
	size_t ump_size = sizeof(ump);
	uint8_t midi[8];
	uint64_t state = 0;

	int size = spa_ump_to_midi(&p, &ump_size, midi, sizeof(midi), &state);
	spa_assert(size == 2);
	spa_assert(midi[0] == 0xc0);
	spa_assert(midi[1] == 0x05);
}

int main(void)
{
	test_ump_from_midi_note_on();
	test_ump_from_midi_program_change();
	test_ump_from_midi_sysex_complete();
	test_ump_from_midi_sysex_complete_max();
	test_ump_from_midi_sysex_multi_packet();
	test_ump_from_midi_sysex_continue();
	test_ump_from_midi_sysex_minimal();
	test_ump_to_midi_note_on();
	test_ump_to_midi_sysex_complete();
	test_ump_to_midi_sysex_start_end();
	test_roundtrip_sysex_short();
	test_roundtrip_sysex_long();
	test_roundtrip_note_on();
	test_ump_from_midi_channel_pressure();
	test_ump_from_midi_song_position();
	test_ump_from_midi_mtc_quarter_frame();
	test_ump_from_midi_song_select();
	test_ump_from_midi_system_realtime();
	test_ump_to_midi_system_realtime();
	test_ump_to_midi_song_position();
	test_ump_to_midi_mtc_quarter_frame();
	test_ump_to_midi_program_change();
	test_ump_to_midi_channel_pressure();
	test_ump_to_midi_sysex_continue();
	test_roundtrip_program_change();
	test_roundtrip_channel_pressure();
	test_roundtrip_song_position();
	test_roundtrip_system_realtime();
	test_ump_from_midi_sysex_trailing_f0();
	test_ump_from_midi_continuation_trailing_f0();
	test_ump_from_midi_sysex_split_with_f0();
	test_ump_from_midi_f7_continuation();
	test_ump_from_midi_f7_continuation_with_end();
	test_ump_from_midi_f7_continuation_active();
	test_ump_from_midi_bare_f7_end();
	test_ump_from_midi_bare_f7_complete();
	test_ump_from_midi_bare_f7_orphan();
	test_ump_from_midi_sysex_split_with_f7();
	test_ump_to_midi2_channel_pressure();
	test_ump_to_midi2_note_on();
	test_ump_to_midi2_pitch_bend();
	test_ump_to_midi2_program_change_with_bank();
	test_ump_to_midi2_program_change_no_bank();

	printf("All UMP utils tests passed.\n");
	return 0;
}
