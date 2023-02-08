/* BLE MIDI parser */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/utils/defs.h>

#include "midi.h"

enum midi_event_class {
	MIDI_BASIC,
	MIDI_SYSEX,
	MIDI_SYSCOMMON,
	MIDI_REALTIME,
	MIDI_ERROR
};

static enum midi_event_class midi_event_info(uint8_t status, unsigned int *size)
{
	switch (status) {
	case 0x80 ... 0x8f:
	case 0x90 ... 0x9f:
	case 0xa0 ... 0xaf:
	case 0xb0 ... 0xbf:
	case 0xe0 ... 0xef:
		*size = 3;
		return MIDI_BASIC;
	case 0xc0 ... 0xcf:
	case 0xd0 ... 0xdf:
		*size = 2;
		return MIDI_BASIC;
	case 0xf0:
		/* variable; count only status byte here */
		*size = 1;
		return MIDI_SYSEX;
	case 0xf1:
	case 0xf3:
		*size = 2;
		return MIDI_SYSCOMMON;
	case 0xf2:
		*size = 3;
		return MIDI_SYSCOMMON;
	case 0xf6:
	case 0xf7:
		*size = 1;
		return MIDI_SYSCOMMON;
	case 0xf8 ... 0xff:
		*size = 1;
		return MIDI_REALTIME;
	case 0xf4:
	case 0xf5:
	default:
		/* undefined MIDI status */
		*size = 0;
		return MIDI_ERROR;
	}
}

static void timestamp_set_high(uint16_t *time, uint8_t byte)
{
	*time = (byte & 0x3f) << 7;
}

static void timestamp_set_low(uint16_t *time, uint8_t byte)
{
	if ((*time & 0x7f) > (byte & 0x7f))
		*time += 0x80;

	*time &= ~0x7f;
	*time |= byte & 0x7f;
}

int spa_bt_midi_parser_parse(struct spa_bt_midi_parser *parser,
		const uint8_t *src, size_t src_size, bool only_time,
		void (*event)(void *user_data, uint16_t time, uint8_t *event, size_t event_size),
		void *user_data)
{
	const uint8_t *src_end = src + src_size;
	uint8_t running_status = 0;
	uint16_t time;
	uint8_t byte;

#define NEXT() do { if (src == src_end) return -EINVAL; byte = *src++; } while (0)
#define PUT(byte) do { if (only_time) { parser->size++; break; }	\
		if (parser->size == sizeof(parser->buf)) return -ENOSPC; \
		parser->buf[parser->size++] = (byte); } while (0)

	/* Header */
	NEXT();
	if (!(byte & 0x80))
		return -EINVAL;
	timestamp_set_high(&time, byte);

	while (src < src_end) {
		NEXT();

		if (!parser->sysex) {
			uint8_t status = 0;
			unsigned int event_size;

			if (byte & 0x80) {
				/* Timestamp */
				timestamp_set_low(&time, byte);
				NEXT();

				/* Status? */
				if (byte & 0x80) {
					parser->size = 0;
					PUT(byte);
					status = byte;
				}
			}

			if (status == 0) {
				/* Running status */
				parser->size = 0;
				PUT(running_status);
				PUT(byte);
				status = running_status;
			}

			switch (midi_event_info(status, &event_size)) {
			case MIDI_BASIC:
				running_status = (event_size > 1) ? status : 0;
				break;
			case MIDI_REALTIME:
			case MIDI_SYSCOMMON:
				/* keep previous running status */
				break;
			case MIDI_SYSEX:
				parser->sysex = true;
				/* XXX: not fully clear if SYSEX can be running status, assume no */
				running_status = 0;
				continue;
			default:
				goto malformed;
			}

			/* Event data */
			while (parser->size < event_size) {
				NEXT();
				if (byte & 0x80) {
					/* BLE MIDI allows no interleaved events */
					goto malformed;
				}
				PUT(byte);
			}

			event(user_data, time, parser->buf, parser->size);
		} else {
			if (byte & 0x80) {
				/* Timestamp */
				timestamp_set_low(&time, byte);
				NEXT();

				if (byte == 0xf7) {
					/* Sysex end */
					PUT(byte);
					event(user_data, time, parser->buf, parser->size);
					parser->sysex = false;
				} else {
					/* Interleaved realtime event */
					unsigned int event_size;

					if (midi_event_info(byte, &event_size) != MIDI_REALTIME)
						goto malformed;
					spa_assert(event_size == 1);
					event(user_data, time, &byte, 1);
				}
			} else {
				PUT(byte);
			}
		}
	}

#undef NEXT
#undef PUT

	return 0;

malformed:
	/* Error (potentially recoverable) */
	return -EINVAL;
}


int spa_bt_midi_writer_write(struct spa_bt_midi_writer *writer,
		uint64_t time, const uint8_t *event, size_t event_size)
{
	/* BLE MIDI-1.0: maximum payload size is MTU - 3 */
	const unsigned int max_size = writer->mtu - 3;
	const uint64_t time_msec = (time / SPA_NSEC_PER_MSEC);
	const uint16_t timestamp = time_msec & 0x1fff;

#define PUT(byte) do { if (writer->size >= max_size) return -ENOSPC; \
		writer->buf[writer->size++] = (byte); } while (0)

	if (writer->mtu < 5+3)
		return -ENOSPC;  /* all events must fit */

	spa_assert(max_size <= sizeof(writer->buf));
	spa_assert(writer->size <= max_size);

	if (event_size == 0)
		return 0;

	if (writer->flush) {
		writer->flush = false;
		writer->size = 0;
	}

	if (writer->size == max_size)
		goto flush;

	/* Packet header */
	if (writer->size == 0) {
		PUT(0x80 | (timestamp >> 7));
		writer->running_status = 0;
		writer->running_time_msec = time_msec;
	}

	/* Timestamp low bits can wrap around, but not multiple times */
	if (time_msec > writer->running_time_msec + 0x7f)
		goto flush;

	spa_assert(writer->pos < event_size);

	for (; writer->pos < event_size; ++writer->pos) {
		const unsigned int unused = max_size - writer->size;
		const uint8_t byte = event[writer->pos];

		if (byte & 0x80) {
			enum midi_event_class class;
			unsigned int expected_size;

			class = midi_event_info(event[0], &expected_size);

			if (class == MIDI_BASIC && expected_size > 1 &&
					writer->running_status == byte &&
					writer->running_time_msec == time_msec) {
				/* Running status: continue with data */
				continue;
			}

			if (unused < expected_size + 1)
				goto flush;

			/* Timestamp before status */
			PUT(0x80 | (timestamp & 0x7f));
			writer->running_time_msec = time_msec;

			if (class == MIDI_BASIC && expected_size > 1)
				writer->running_status = byte;
			else
				writer->running_status = 0;
		} else if (unused == 0) {
			break;
		}

		PUT(byte);
	}

	if (writer->pos < event_size)
		goto flush;

	writer->pos = 0;
	return 0;

flush:
	writer->flush = true;
	return 1;

#undef PUT
}
