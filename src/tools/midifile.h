/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include <stdio.h>

#include <spa/utils/defs.h>
#include <spa/utils/list.h>

struct midi_event {
	struct spa_list link;

	struct midi_track *track;

	double sec;
	uint8_t status;
	uint8_t meta;

	uint8_t *data;
	uint32_t size;
};

struct midi_track {
	struct spa_list link;

	uint8_t *data;
	uint32_t size;

	uint8_t *p;
	int64_t tick;
	uint8_t running_status;
	unsigned int eof:1;

	struct spa_list events;
};

struct midi_file {
	uint8_t *data;
	size_t size;

	uint32_t length;
	uint16_t format;
	uint16_t ntracks;
	uint16_t division;
	uint32_t tempo;

	struct spa_list tracks;

	uint8_t *p;
	int64_t tick;
	double tick_sec;
	double tick_start;
};

int midi_file_init(struct midi_file *mf, const char *mode,
		void *data, size_t size);

int midi_file_add_track(struct midi_file *mf, struct midi_track *track);

int midi_file_peek_event(struct midi_file *mf, struct midi_event *event);
int midi_file_consume_event(struct midi_file *mf, struct midi_event *event);

int midi_file_add_event(struct midi_file *mf, struct midi_track *track, struct midi_event *event);


