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

#include <errno.h>

#include "midifile.h"

#define DEFAULT_TEMPO	500000	/* 500ms per quarter note (120 BPM) is the default */

static inline uint16_t parse_be16(const uint8_t *in)
{
	return (in[0] << 8) | in[1];
}

static inline uint32_t parse_be32(const uint8_t *in)
{
	return (in[0] << 24) | (in[1] << 16) | (in[2] << 8) | in[3];
}

static inline int mf_avail(struct midi_file *mf)
{
	if (mf->p < mf->data + mf->size)
		return mf->size + mf->data - mf->p;
	return 0;
}

static inline int tr_avail(struct midi_track *tr)
{
	if (tr->eof)
		return 0;
	if (tr->p < tr->data + tr->size)
		return tr->size + tr->data - tr->p;
	tr->eof = true;
	return 0;
}

static int read_mthd(struct midi_file *mf)
{
	if (mf_avail(mf) < 14 ||
	    memcmp(mf->p, "MThd", 4) != 0)
		return -EIO;

	mf->length = parse_be32(mf->p + 4);
	mf->format = parse_be16(mf->p + 8);
	mf->ntracks = parse_be16(mf->p + 10);
	mf->division = parse_be16(mf->p + 12);

	mf->p += 14;
	return 0;
}

int midi_file_init(struct midi_file *mf, const char *mode,
		void *data, size_t size)
{
	int res;

	spa_zero(*mf);
	mf->data = mf->p = data;
	mf->size = size;

	if ((res = read_mthd(mf)) < 0)
		return res;

	spa_list_init(&mf->tracks);

	mf->tempo = DEFAULT_TEMPO;
	mf->tick = 0;

	return 0;
}

static int read_mtrk(struct midi_file *mf, struct midi_track *track)
{
	if (mf_avail(mf) < 8 ||
	    memcmp(mf->p, "MTrk", 4) != 0)
		return -EIO;

	track->data = track->p = mf->p + 8;
	track->size = parse_be32(mf->p + 4);

	mf->p = track->data + track->size;
	return 0;
}

static int parse_varlen(struct midi_file *mf, struct midi_track *tr, uint32_t *result)
{
	uint32_t value = 0;

	while (tr_avail(tr) > 0) {
		uint8_t b = *tr->p++;
		value = (value << 7) | (b & 0x7f);
		if ((b & 0x80) == 0)
			break;
	}
	*result = value;
	return 0;
}

static int peek_event(struct midi_file *mf, struct midi_track *tr, struct midi_event *event)
{
	uint8_t *save, status;
	uint32_t size;
	int res;

	if (tr_avail(tr) == 0)
		return 0;

	save = tr->p;
	status = *tr->p;

	event->track = tr;
	event->sec = mf->tick_sec + ((tr->tick - mf->tick_start) * (double)mf->tempo) / (1000000.0 * mf->division);

	if ((status & 0x80) == 0) {
		status = tr->running_status;
	} else {
		tr->running_status = status;
		tr->p++;
	}
	event->status = status;

	switch (status) {
	case 0xc0 ... 0xdf:
		size = 1;
		break;

	case 0x00 ... 0xbf:
	case 0xe0 ... 0xef:
		size = 2;
		break;

	case 0xff:
		event->meta = *tr->p++;

		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;

		switch (event->meta) {
		case 0x2f:
			tr->eof = true;
			break;
		case 0x51:
			if (size < 3)
				return -EIO;
			mf->tick_sec = event->sec;
			mf->tick_start = tr->tick;
			mf->tempo = (tr->p[0]<<16) | (tr->p[1]<<8) | tr->p[2];
			break;
		}
		break;

	case 0xf0:
	case 0xf7:
		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;
		break;
	default:
		return -EIO;
	}
	event->data = tr->p;
	tr->p = save;

	event->size = size;
	return 1;
}

int midi_file_add_track(struct midi_file *mf, struct midi_track *track)
{
	int res;
	uint32_t delta_time;

	if ((res = read_mtrk(mf, track)) < 0)
		return res;

	if ((res = parse_varlen(mf, track, &delta_time)) < 0)
		return res;

	track->tick = delta_time;
	spa_list_append(&mf->tracks, &track->link);

	return 0;

}
int midi_file_peek_event(struct midi_file *mf, struct midi_event *event)
{
	struct midi_track *tr, *found = NULL;

	spa_list_for_each(tr, &mf->tracks, link) {
		if (tr_avail(tr) == 0)
			continue;
		if (found == NULL || tr->tick < found->tick)
			found = tr;
	}
	if (found == NULL)
		return 0;

	return peek_event(mf, found, event);
}

int midi_file_consume_event(struct midi_file *mf, struct midi_event *event)
{
	struct midi_track *tr = event->track;
	uint32_t delta_time;
	int res;

	tr->p = event->data + event->size;

	if ((res = parse_varlen(mf, tr, &delta_time)) < 0)
		return res;

	tr->tick += delta_time;
	return 0;
}
