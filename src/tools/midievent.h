/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef MIDI_EVENT_H
#define MIDI_EVENT_H

#include <stdio.h>

#include <spa/utils/defs.h>

struct midi_event {
#define MIDI_EVENT_TYPE_MIDI1		0
#define MIDI_EVENT_TYPE_UMP		1
	uint32_t type;
	uint32_t track;
	double sec;
	uint8_t *data;
	uint32_t size;
	struct {
		uint32_t offset;
		uint32_t size;
		union {
			struct {
				uint32_t uspqn; /* microseconds per quarter note */
			} tempo;
		} parsed;
	} meta;
};

int midi_event_dump(FILE *out, const struct midi_event *event);

#endif /* MIDI_EVENT_H */
