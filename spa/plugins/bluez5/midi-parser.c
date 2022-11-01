/* BLE MIDI parser
 *
 * Copyright © 2022 Pauli Virtanen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

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
