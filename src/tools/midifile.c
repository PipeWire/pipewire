/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include <spa/utils/string.h>
#include <spa/control/ump-utils.h>

#include "midifile.h"

#define DEFAULT_TEMPO	500000	/* 500ms per quarter note (120 BPM) is the default */

struct midi_track {
	uint16_t id;

	long start;
	uint32_t size;
	long pos;

	int64_t tick;
	unsigned int eof:1;
	uint8_t event[4];
};

struct midi_file {
	int mode;
	FILE *file;
	bool close;
	long pos;

	uint8_t *buffer;
	size_t buffer_size;

	struct midi_file_info info;
	uint32_t length;
	uint32_t tempo;

	int64_t tick;
	double tick_sec;
	double tick_start;

	struct midi_track tracks[64];
};

static inline uint16_t parse_be16(const uint8_t *in)
{
	return (in[0] << 8) | in[1];
}

static inline uint32_t parse_be32(const uint8_t *in)
{
	return (in[0] << 24) | (in[1] << 16) | (in[2] << 8) | in[3];
}

static inline int mf_seek(struct midi_file *mf, long offs)
{
	int res;
	if (mf->pos == offs)
		return 0;
	if ((res = fseek(mf->file, offs, SEEK_SET)) != 0)
		return -errno;
	mf->pos = offs;
	return 0;
}

static inline int mf_read(struct midi_file *mf, void *data, size_t size)
{
	if (fread(data, size, 1, mf->file) != 1)
		return 0;
	mf->pos += size;
	return 1;
}

static inline int tr_avail(struct midi_track *tr)
{
	if (tr->eof)
		return 0;
	if (tr->size == 0)
		return 1;
	if (tr->pos < tr->start + tr->size)
		return tr->size + tr->start - tr->pos;
	tr->eof = true;
	return 0;
}

static int read_mthd(struct midi_file *mf)
{
	uint8_t data[14];

	if (mf_read(mf, data, sizeof(data)) != 1 ||
	    memcmp(data, "MThd", 4) != 0)
		return -EINVAL;

	mf->length = parse_be32(data + 4);
	mf->info.format = parse_be16(data + 8);
	mf->info.ntracks = parse_be16(data + 10);
	mf->info.division = parse_be16(data + 12);
	return 0;
}

static int parse_varlen(struct midi_file *mf, struct midi_track *tr, uint32_t *result)
{
	uint32_t value = 0;
	uint8_t data[1];

	while (mf_read(mf, data, 1) == 1) {
		value = (value << 7) | (data[0] & 0x7f);
		if ((data[0] & 0x80) == 0)
			break;
	}
	*result = value;
	return 0;
}

static int read_delta_time(struct midi_file *mf, struct midi_track *tr)
{
	int res;
	uint32_t delta_time;

	if ((res = parse_varlen(mf, tr, &delta_time)) < 0)
		return res;

	tr->tick += delta_time;
	tr->pos = mf->pos;
	return 0;

}

static int read_mtrk(struct midi_file *mf, struct midi_track *track)
{
	uint8_t data[8];

	if (mf_read(mf, data, sizeof(data)) != 1 ||
	    memcmp(data, "MTrk", 4) != 0)
		return -EINVAL;

	track->start = track->pos = mf->pos;
	track->size = parse_be32(data + 4);

	return read_delta_time(mf, track);
}

static uint8_t *ensure_buffer(struct midi_file *mf, struct midi_track *tr, size_t size)
{
	if (size <= 4)
		return tr->event;

	if (size > mf->buffer_size) {
		mf->buffer = realloc(mf->buffer, size);
		mf->buffer_size = size;
	}
	return mf->buffer;
}

static int open_read(struct midi_file *mf, const char *filename, struct midi_file_info *info)
{
	int res;
	uint16_t i;

	if (strcmp(filename, "-") != 0) {
		if ((mf->file = fopen(filename, "r")) == NULL) {
			res = -errno;
			goto exit;
		}
		mf->close = true;
	} else {
		mf->file = stdin;
		mf->close = false;
	}

	if ((res = read_mthd(mf)) < 0)
		goto exit_close;

	mf->tempo = DEFAULT_TEMPO;
	mf->tick = 0;

	for (i = 0; i < mf->info.ntracks; i++) {
		struct midi_track *tr = &mf->tracks[i];

		if ((res = read_mtrk(mf, tr)) < 0)
			goto exit_close;

		tr->id = i;

		if (i + 1 < mf->info.ntracks &&
		    (res = mf_seek(mf, tr->start + tr->size)) < 0)
			goto exit_close;
	}
	mf->mode = 1;
	*info = mf->info;
	return 0;

exit_close:
	if (mf->close)
		fclose(mf->file);
exit:
	return res;
}

static inline int write_n(FILE *file, const void *buf, int count)
{
	return fwrite(buf, 1, count, file) == (size_t)count ? count : -errno;
}

static inline int write_be16(FILE *file, uint16_t val)
{
	uint8_t buf[2] = { val >> 8, val };
	return write_n(file, buf, 2);
}

static inline int write_be32(FILE *file, uint32_t val)
{
	uint8_t buf[4] = { val >> 24, val >> 16, val >> 8, val };
	return write_n(file, buf, 4);
}

#define CHECK_RES(expr) if ((res = (expr)) < 0) return res

static int write_headers(struct midi_file *mf)
{
	struct midi_track *tr = &mf->tracks[0];
	int res;

	mf_seek(mf, 0);

	mf->length = 6;
	CHECK_RES(write_n(mf->file, "MThd", 4));
	CHECK_RES(write_be32(mf->file, mf->length));
	CHECK_RES(write_be16(mf->file, mf->info.format));
	CHECK_RES(write_be16(mf->file, mf->info.ntracks));
	CHECK_RES(write_be16(mf->file, mf->info.division));

	CHECK_RES(write_n(mf->file, "MTrk", 4));
	CHECK_RES(write_be32(mf->file, tr->size));

	return 0;
}

static int open_write(struct midi_file *mf, const char *filename, struct midi_file_info *info)
{
	int res;

	if (info->format != 0)
		return -EINVAL;
	if (info->ntracks == 0)
		info->ntracks = 1;
	else if (info->ntracks != 1)
		return -EINVAL;
	if (info->division == 0)
		info->division = 96;

	if (strcmp(filename, "-") != 0) {
		if ((mf->file = fopen(filename, "w")) == NULL) {
			res = -errno;
			goto exit;
		}
		mf->close = true;
	} else {
		mf->file = stdout;
		mf->close = false;
	}
	mf->mode = 2;
	mf->tempo = DEFAULT_TEMPO;
	mf->info = *info;

	res = write_headers(mf);
exit:
	return res;
}

struct midi_file *
midi_file_open(const char *filename, const char *mode, struct midi_file_info *info)
{
	int res;
	struct midi_file *mf;

	mf = calloc(1, sizeof(struct midi_file));
	if (mf == NULL)
		return NULL;

	if (spa_streq(mode, "r")) {
		if ((res = open_read(mf, filename, info)) < 0)
			goto exit_free;
	} else if (spa_streq(mode, "w")) {
		if ((res = open_write(mf, filename, info)) < 0)
			goto exit_free;
	} else {
		res = -EINVAL;
		goto exit_free;
	}
	return mf;

exit_free:
	free(mf);
	errno = -res;
	return NULL;
}

int midi_file_close(struct midi_file *mf)
{
	int res;

	if (mf->mode == 2) {
		uint8_t buf[4] = { 0x00, 0xff, 0x2f, 0x00 };
		CHECK_RES(write_n(mf->file, buf, 4));
		mf->tracks[0].size += 4;
		CHECK_RES(write_headers(mf));
	} else
		return -EINVAL;

	if (mf->close)
		fclose(mf->file);
	free(mf->buffer);
	free(mf);
	return 0;
}

static int peek_next(struct midi_file *mf, struct midi_event *ev)
{
	struct midi_track *tr, *found = NULL;
	uint16_t i;

	for (i = 0; i < mf->info.ntracks; i++) {
		tr = &mf->tracks[i];
		if (tr_avail(tr) == 0)
			continue;
		if (found == NULL || tr->tick < found->tick)
			found = tr;
	}
	if (found == NULL)
		return 0;

	ev->track = found->id;
	ev->sec = mf->tick_sec + ((found->tick - mf->tick_start) * (double)mf->tempo) / (1000000.0 * mf->info.division);
	ev->type = MIDI_EVENT_TYPE_MIDI1;
	return 1;
}

int midi_file_next_time(struct midi_file *mf, double *sec)
{
	struct midi_event ev;
	int res;

	if ((res = peek_next(mf, &ev)) <= 0)
		return res;

	*sec = ev.sec;
	return 1;
}

int midi_file_read_event(struct midi_file *mf, struct midi_event *event)
{
	struct midi_track *tr;
	uint32_t size;
	uint8_t status, meta;
	int res, running;

	event->data = NULL;

	if ((res = peek_next(mf, event)) <= 0)
		return res;

	tr = &mf->tracks[event->track];

	if ((res = mf_seek(mf, tr->pos)) < 0)
		return res;

	mf_read(mf, &status, 1);

	running = (status & 0x80) == 0;
	if (running) {
		tr->event[1] = status;
		status = tr->event[0];
	} else {
		tr->event[0] = status;
	}

	switch (status) {
	case 0xc0 ... 0xdf:
		size = 2;
		break;

	case 0x80 ... 0xbf:
	case 0xe0 ... 0xef:
		size = 3;
		break;

	case 0xff:
		if (running)
			return -EINVAL;

		mf_read(mf, &meta, 1);

		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;

		event->meta.offset = 2;
		event->meta.size = size;

		if ((event->data = ensure_buffer(mf, tr, size + event->meta.offset)) == NULL)
			return -ENOMEM;

		event->data[0] = status;
		event->data[1] = meta;
		if (size > 0 && mf_read(mf, &event->data[2], size) != 1)
			return -EINVAL;

		switch (meta) {
		case 0x2f:
			tr->eof = true;
			break;
		case 0x51:
		{
			if (size < 3)
				return -EINVAL;
			mf->tick_sec = event->sec;
			mf->tick_start = tr->tick;
			event->meta.parsed.tempo.uspqn = mf->tempo =
				(event->data[2]<<16) | (event->data[3]<<8) | event->data[4];
			break;
		}
		}
		size += event->meta.offset;
		break;

	case 0xf0:
	case 0xf7:
		if (running)
			return -EINVAL;

		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;

		if ((event->data = ensure_buffer(mf, tr, size + 1)) == NULL)
			return -ENOMEM;

		event->data[0] = status;
		if (mf_read(mf, &event->data[1], size) != 1)
			return -EINVAL;

		size += 1;
		break;
	default:
		return -EINVAL;
	}

	event->size = size;
	if (event->data == NULL) {
		if ((event->data = ensure_buffer(mf, tr, size)) == NULL)
			return -ENOMEM;
		event->data[0] = tr->event[0];
		if (running) {
			event->data[1] = tr->event[1];
			if (size > 2 && mf_read(mf, &event->data[2], size - 2) != 1)
				return -EINVAL;
		} else {
			if (size > 1 && mf_read(mf, &event->data[1], size - 1) != 1)
				return -EINVAL;
		}
	}

	if ((res = read_delta_time(mf, tr)) < 0)
		return res;

	return 1;
}

static int write_varlen(struct midi_file *mf, struct midi_track *tr, uint32_t value)
{
	uint64_t buffer;
	uint8_t b;
	int res;

	buffer = value & 0x7f;
	while ((value >>= 7)) {
		buffer <<= 8;
		buffer |= ((value & 0x7f) | 0x80);
	}
        do  {
		b = buffer & 0xff;
		CHECK_RES(write_n(mf->file, &b, 1));
		tr->size++;
		buffer >>= 8;
	} while (b & 0x80);

	return 0;
}

int midi_file_write_event(struct midi_file *mf, const struct midi_event *event)
{
	struct midi_track *tr;
	uint32_t tick;
	void *data, *ev_data;
	size_t size;
	int res, ev_size;
	uint8_t ev[32];
	uint64_t state = 0;

	spa_return_val_if_fail(event != NULL, -EINVAL);
	spa_return_val_if_fail(mf != NULL, -EINVAL);
	spa_return_val_if_fail(event->track == 0, -EINVAL);
	spa_return_val_if_fail(event->size > 1, -EINVAL);

	data = event->data;
	size = event->size;

	tr = &mf->tracks[event->track];
	tick = (uint32_t)(event->sec * (1000000.0 * mf->info.division) / (double)mf->tempo);

	while (size > 0) {
		switch (event->type) {
		case MIDI_EVENT_TYPE_MIDI1:
			ev_data = data;
			ev_size = size;
			size = 0;
			break;
		case MIDI_EVENT_TYPE_UMP:
			ev_size = spa_ump_to_midi((const uint32_t**)&data, &size, ev, sizeof(ev), &state);
			if (ev_size <= 0)
				return ev_size;
			ev_data = ev;
			break;
		default:
			return -EINVAL;
		}

		CHECK_RES(write_varlen(mf, tr, tick - tr->tick));
		tr->tick = tick;

		CHECK_RES(write_n(mf->file, ev_data, ev_size));
		tr->size += ev_size;
	}
	return 0;
}
