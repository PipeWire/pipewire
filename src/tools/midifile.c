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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "midifile.h"

#define DEFAULT_TEMPO	500000	/* 500ms per quarter note (120 BPM) is the default */

struct midi_track {
	uint16_t id;

	uint8_t *data;
	uint32_t size;

	uint8_t *p;
	int64_t tick;
	unsigned int eof:1;
	uint8_t event[4];
};

struct midi_file {
	uint8_t *data;
	size_t size;

	int mode;
	int fd;

	struct midi_file_info info;
	uint32_t length;
	uint32_t tempo;

	uint8_t *p;
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
		return -EINVAL;

	mf->length = parse_be32(mf->p + 4);
	mf->info.format = parse_be16(mf->p + 8);
	mf->info.ntracks = parse_be16(mf->p + 10);
	mf->info.division = parse_be16(mf->p + 12);

	mf->p += 14;
	return 0;
}

static int read_mtrk(struct midi_file *mf, struct midi_track *track)
{
	if (mf_avail(mf) < 8 ||
	    memcmp(mf->p, "MTrk", 4) != 0)
		return -EINVAL;

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

static int open_read(struct midi_file *mf, const char *filename, struct midi_file_info *info)
{
	int res;
	uint16_t i;
	struct stat st;

	if (stat(filename, &st) < 0) {
		res = -errno;
		goto exit;
	}

	mf->size = st.st_size;

	if ((mf->fd = open(filename, O_RDONLY)) < 0) {
		res = -errno;
		goto exit;
	}

	mf->data = mmap(NULL, mf->size, PROT_READ, MAP_SHARED, mf->fd, 0);
	if (mf->data == MAP_FAILED) {
		res = -errno;
		goto exit_close;
	}

	mf->p = mf->data;

	if ((res = read_mthd(mf)) < 0)
		goto exit_unmap;

	mf->tempo = DEFAULT_TEMPO;
	mf->tick = 0;

	for (i = 0; i < mf->info.ntracks; i++) {
		struct midi_track *tr = &mf->tracks[i];
		uint32_t delta_time;

		if ((res = read_mtrk(mf, tr)) < 0)
			goto exit_unmap;

		if ((res = parse_varlen(mf, tr, &delta_time)) < 0)
			goto exit_unmap;

		tr->tick = delta_time;
		tr->id = i;
	}
	mf->mode = 1;
	*info = mf->info;
	return 0;

exit_unmap:
	munmap(mf->data, mf->size);
exit_close:
	close(mf->fd);
exit:
	return res ;
}

static inline int write_be16(int fd, uint16_t val)
{
	uint8_t buf[2] = { val >> 8, val };
	return write(fd, buf, 2);
}

static inline int write_be32(int fd, uint32_t val)
{
	uint8_t buf[4] = { val >> 24, val >> 16, val >> 8, val };
	return write(fd, buf, 4);
}

static int write_headers(struct midi_file *mf)
{
	struct midi_track *tr = &mf->tracks[0];

	lseek(mf->fd, 0, SEEK_SET);

	mf->length = 6;
	write(mf->fd, "MThd", 4);
	write_be32(mf->fd, mf->length);
	write_be16(mf->fd, mf->info.format);
	write_be16(mf->fd, mf->info.ntracks);
	write_be16(mf->fd, mf->info.division);

	write(mf->fd, "MTrk", 4);
	write_be32(mf->fd, tr->size);

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

	if ((mf->fd = open(filename, O_WRONLY | O_CREAT, 0660)) < 0) {
		res = -errno;
		goto exit;
	}
	mf->mode = 2;
	mf->tempo = DEFAULT_TEMPO;
	mf->info = *info;

	write_headers(mf);

	return 0;
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

	if (strcmp(mode, "r") == 0) {
		if ((res = open_read(mf, filename, info)) < 0)
			goto exit_free;
	} else if (strcmp(mode, "w") == 0) {
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
	if (mf->mode == 1) {
		munmap(mf->data, mf->size);
	} else if (mf->mode == 2) {
		uint8_t buf[4] = { 0x00, 0xff, 0x2f, 0x00 };
		write(mf->fd, buf, 4);
		mf->tracks[0].size += 4;
		write_headers(mf);
	} else
		return -EINVAL;

	close(mf->fd);
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
	if (found == NULL ||
	    tr_avail(found) == 0)
		return 0;

	ev->track = found->id;
	ev->sec = mf->tick_sec + ((found->tick - mf->tick_start) * (double)mf->tempo) / (1000000.0 * mf->info.division);
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
	uint32_t delta_time, size;
	uint8_t status, meta;
	int res, running;

	if ((res = peek_next(mf, event)) <= 0)
		return res;

	tr = &mf->tracks[event->track];
	status = *tr->p;

	running = (status & 0x80) == 0;
	if (running) {
		status = tr->event[0];
		event->data = tr->event;
	} else {
		event->data = tr->p++;
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
		meta = *tr->p++;

		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;

		event->meta.offset = tr->p - event->data;
		event->meta.size = size;

		switch (meta) {
		case 0x2f:
			tr->eof = true;
			break;
		case 0x51:
			if (size < 3)
				return -EINVAL;
			mf->tick_sec = event->sec;
			mf->tick_start = tr->tick;
			event->meta.parsed.tempo.uspqn = mf->tempo = (tr->p[0]<<16) | (tr->p[1]<<8) | tr->p[2];
			break;
		}
		size += tr->p - event->data;
		break;

	case 0xf0:
	case 0xf7:
		if ((res = parse_varlen(mf, tr, &size)) < 0)
			return res;
		size += tr->p - event->data;
		break;
	default:
		return -EINVAL;
	}

	event->size = size;

	if (running) {
		memcpy(&event->data[1], tr->p, size - 1);
		tr->p += size - 1;
	} else {
		tr->p = event->data + event->size;
	}

	if ((res = parse_varlen(mf, tr, &delta_time)) < 0)
		return res;

	tr->tick += delta_time;
	return 1;
}

static int write_varlen(struct midi_file *mf, struct midi_track *tr, uint32_t value)
{
	uint64_t buffer;
	uint8_t b;

	buffer = value & 0x7f;
	while ((value >>= 7)) {
		buffer <<= 8;
		buffer |= ((value & 0x7f) | 0x80);
	}
        do  {
		b = buffer & 0xff;
		write(mf->fd, &b, 1);
		tr->size++;
		buffer >>= 8;
	} while (b & 0x80);

	return 0;
}

int midi_file_write_event(struct midi_file *mf, const struct midi_event *event)
{
	struct midi_track *tr;
	uint32_t tick;

	spa_return_val_if_fail(event != NULL, -EINVAL);
	spa_return_val_if_fail(mf != NULL, -EINVAL);
	spa_return_val_if_fail(event->track == 0, -EINVAL);
	spa_return_val_if_fail(event->size > 1, -EINVAL);

	tr = &mf->tracks[event->track];

	tick = event->sec * (1000000.0 * mf->info.division) / (double)mf->tempo;

	write_varlen(mf, tr, tick - tr->tick);
	tr->tick = tick;

	write(mf->fd, event->data, event->size);
	tr->size += event->size;

	return 0;
}
