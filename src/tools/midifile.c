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
	uint8_t running_status;
	unsigned int eof:1;
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

static int write_headers(struct midi_file *mf)
{
	uint8_t buf[4];
	struct midi_track *tr = &mf->tracks[0];

	mf->length = 6;

	lseek(mf->fd, 0, SEEK_SET);

	write(mf->fd, "MThd", 4);
	buf[0] = mf->length >> 24;
	buf[1] = mf->length >> 16;
	buf[2] = mf->length >> 8;
	buf[3] = mf->length;
	write(mf->fd, buf, 4);
	buf[0] = mf->info.format >> 8;
	buf[1] = mf->info.format;
	write(mf->fd, buf, 2);
	buf[0] = mf->info.ntracks >> 8;
	buf[1] = mf->info.ntracks;
	write(mf->fd, buf, 2);
	buf[0] = mf->info.division >> 8;
	buf[1] = mf->info.division;
	write(mf->fd, buf, 2);

	write(mf->fd, "MTrk", 4);
	buf[0] = tr->size >> 24;
	buf[1] = tr->size >> 16;
	buf[2] = tr->size >> 8;
	buf[3] = tr->size;
	write(mf->fd, buf, 4);

	tr->size = 4;

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
	uint32_t delta_time;
	uint32_t size;
	uint8_t status;
	int res;

	if ((res = peek_next(mf, event)) <= 0)
		return res;

	tr = &mf->tracks[event->track];
	status = *tr->p;

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
				return -EINVAL;
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
		return -ENOENT;
	}

	event->data = tr->p;
	event->size = size;

	tr->p += size;

	if ((res = parse_varlen(mf, tr, &delta_time)) < 0)
		return res;

	tr->tick += delta_time;
	return 1;
}

static int write_varlen(struct midi_file *mf, struct midi_track *tr, uint32_t value)
{
	uint64_t buffer;

	buffer = value & 0x7f;
	while ((value >>= 7)) {
		buffer <<= 8;
		buffer |= ((value & 0x7f) | 0x80);
	}
        for (;;) {
		uint8_t b = buffer & 0xff;
		write(mf->fd, &b, 1);
		tr->size++;
                if (buffer & 0x80)
                        buffer >>= 8;
                else
                        break;
        }
	return 0;
}

int midi_file_write_event(struct midi_file *mf, const struct midi_event *event)
{
	struct midi_track *tr;
	uint32_t size, tick;
	uint8_t status, *data;

	spa_return_val_if_fail(event != NULL, -EINVAL);
	spa_return_val_if_fail(mf != NULL, -EINVAL);
	spa_return_val_if_fail(event->track == 0, -EINVAL);
	spa_return_val_if_fail(event->size > 1, -EINVAL);

	tr = &mf->tracks[event->track];
	data = event->data;
	size = event->size;

	tick = event->sec * (1000000.0 * mf->info.division) / (double)mf->tempo;

	write_varlen(mf, tr, tick - tr->tick);
	tr->tick = tick;

	status = *data++;
	size--;

	switch (status) {
	case 0xc0 ... 0xdf:
		if (size != 1)
			return -EINVAL;
		break;

	case 0x00 ... 0xbf:
	case 0xe0 ... 0xef:
		if (size != 2)
			return -EINVAL;
		break;

	case 0xff:
	case 0xf0:
	case 0xf7:
		return -ENOTSUP;
	default:
		return -EINVAL;
	}

	write(mf->fd, &status, 1);
	write(mf->fd, data, size);
	tr->size += 1 + size;

	return 0;
}
