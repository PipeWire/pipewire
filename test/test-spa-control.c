/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans. */
/* SPDX-License-Identifier: MIT */

#include "pwtest.h"

#include <spa/control/control.h>
#include <spa/control/ump-utils.h>

PWTEST(control_abi_types)
{
	/* contol */
	pwtest_int_eq(SPA_CONTROL_Invalid, 0);
	pwtest_int_eq(SPA_CONTROL_Properties, 1);
	pwtest_int_eq(SPA_CONTROL_Midi, 2);
	pwtest_int_eq(SPA_CONTROL_OSC, 3);
	pwtest_int_eq(SPA_CONTROL_UMP, 4);
	pwtest_int_eq(_SPA_CONTROL_LAST, 5);

	return PWTEST_PASS;
}

static inline uint32_t tohex(char v)
{
	if (v >= '0' && v <= '9')
		return v - '0';
	if (v >= 'a' && v <= 'f')
		return v - 'a' + 10;
	return 0;
}

static size_t parse_midi(const char *midi, uint8_t *data, size_t max_size)
{
	size_t size = 0;
	while (*midi) {
		while (*midi == ' ')
			midi++;
		data[size++] = tohex(*(midi+0)) << 4 |
				tohex(*(midi+1));
		midi+=2;
	}
	return size;
}

static size_t parse_ump(const char *ump, uint32_t *data, size_t max_size)
{
	size_t size = 0;
	while (*ump) {
		while (*ump == ' ')
			ump++;
		data[size++] = tohex(*(ump+0)) << 28 |
				tohex(*(ump+1)) << 24 |
				tohex(*(ump+2)) << 20 |
				tohex(*(ump+3)) << 16 |
				tohex(*(ump+4)) << 12 |
				tohex(*(ump+5)) << 8 |
				tohex(*(ump+6)) << 4 |
				tohex(*(ump+7));
		ump+=8;
	}
	return size * 4;
}

static int do_midi_to_ump_test(char *midi, char *ump)
{
	int i;
	size_t m_size, u_size, u_offs = 0;
	uint8_t *m_data = alloca(strlen(midi) / 2);
	uint32_t *u_data = alloca(strlen(ump) / 2);
	uint64_t state = 0;

	m_size = parse_midi(midi, m_data, sizeof(m_data));
	u_size = parse_ump(ump, u_data, sizeof(u_data));

	while (m_size > 0) {
		uint32_t ump[4];
		fprintf(stdout, "%zd %08x\n", m_size, *m_data);
		int ump_size = spa_ump_from_midi(&m_data, &m_size,
				ump, sizeof(ump), 0, &state);
		if (ump_size <= 0)
			return -1;

		if (u_size <= u_offs)
			return -1;

		for (i = 0; i < ump_size / 4; i++) {
			fprintf(stdout, "%08x %08x\n", u_data[u_offs], ump[i]);
			spa_assert(u_data[u_offs++] == ump[i]);
		}
	}
	return 0;
}

PWTEST(control_midi_to_ump)
{
	/* sysex */
	do_midi_to_ump_test("f0 f7",
			"30000000 00000000");

	do_midi_to_ump_test("f0 01 02 03 04 05 f7",
			"30050102 03040500");

	do_midi_to_ump_test("f0 01 02 03 04 05 06 f7",
			"30060102 03040506");
	do_midi_to_ump_test("f0 01 02 03 04 05 06 07 f7",
			"30160102 03040506 30310700 00000000");
	do_midi_to_ump_test("f0 01 02 03 04 05 06 07 08 09 10 11 12 13 f7",
			"30160102 03040506 30260708 09101112 30311300 00000000");

	do_midi_to_ump_test("f0 01 02 03 04 05 06 f0",
			"30160102 03040506");
	do_midi_to_ump_test("f7 01 02 03 04 05 06 07 08 f0",
			"30260102 03040506 30220708 00000000");
	do_midi_to_ump_test("f7 01 02 03 04 05 06 07 08 09 f7",
			"30260102 03040506 30330708 09000000");

	return PWTEST_PASS;
}

static int do_ump_to_midi_test(char *ump, char *midi)
{
	int i;
	size_t m_size, u_size, m_offs = 0;
	uint8_t *m_data = alloca(strlen(midi) / 2);
	uint32_t *u_data = alloca(strlen(ump) / 2);
	uint64_t state = 0;

	u_size = parse_ump(ump, u_data, sizeof(u_data));
	m_size = parse_midi(midi, m_data, sizeof(m_data));

	spa_assert(u_size > 0);
	spa_assert(m_size > 0);

	while (u_size > 0) {
		uint8_t midi[32];
		fprintf(stdout, "%zd %08x\n", u_size, *u_data);

		int midi_size = spa_ump_to_midi((const uint32_t**)&u_data, &u_size,
				midi, sizeof(midi), &state);
		if (midi_size <= 0)
			return midi_size;

		if (m_size <= m_offs)
			return -1;

		for (i = 0; i < midi_size; i++) {
			fprintf(stdout, "%08x %08x\n", m_data[m_offs], midi[i]);
			spa_assert(m_data[m_offs++] == midi[i]);
		}
	}
	return 0;
}

PWTEST(control_ump_to_midi)
{
	spa_assert(do_ump_to_midi_test("30000000 00000000",
			"f0 f7") >= 0);
	spa_assert(do_ump_to_midi_test("30050102 03040500",
			"f0 01 02 03 04 05 f7") >= 0);

	spa_assert(do_ump_to_midi_test("30160102 03040506 30260708 09101112 30311300 00000000",
				"f0 01 02 03 04 05 06 07 08 09 10 11 12 13 f7") >= 0);

	spa_assert(do_ump_to_midi_test("40cf0000 11000000", "cf 11") >= 0);

	spa_assert(do_ump_to_midi_test("40cf0001 11002233", "bf 00 22 bf 20 33 cf 11") >= 0);

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_buffer)
{
	pwtest_add(control_abi_types, PWTEST_NOARG);
	pwtest_add(control_midi_to_ump, PWTEST_NOARG);
	pwtest_add(control_ump_to_midi, PWTEST_NOARG);

	return PWTEST_PASS;
}
