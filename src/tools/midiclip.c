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

#include "midiclip.h"

#define DEFAULT_BPM	120
#define SEC_AS_10NS	100000000.0
#define MINUTE_10NS	6000000000		/* in 10ns units */
#define DEFAULT_TEMPO	MINUTE_10NS/DEFAULT_BPM

struct midi_clip {
	int mode;
	FILE *file;
	bool close;
	int64_t count;

	uint8_t data[16];

	uint32_t next[4];
	int num;

	bool pass_all;
	struct midi_clip_info info;
	uint32_t tempo;

	int64_t tick;
	int64_t tick_start;
	double tick_sec;
};

static int read_header(struct midi_clip *mc)
{
	uint8_t data[8];

	if (fread(data, sizeof(data), 1, mc->file) != 1 ||
	    memcmp(data, "SMF2CLIP", 4) != 0)
		return -EINVAL;
	return 0;
}

static inline int read_word(struct midi_clip *mc, uint32_t *val)
{
	uint32_t v;
	if (fread(&v, 4, 1, mc->file) != 1)
		return 0;
	*val = be32toh(v);
	return 1;
}

static inline int read_ump(struct midi_clip *mc)
{
	int i, num;
	mc->num = 0;
	if (read_word(mc, &mc->next[0]) != 1)
		return 0;
	num = spa_ump_message_size(mc->next[0]>>28);
	for (i = 1; i < num; i++) {
		if (read_word(mc, &mc->next[i]) != 1)
                        return 0;
        }
	return mc->num = num;
}

static int next_packet(struct midi_clip *mc)
{
	while (read_ump(mc) > 0) {
		uint8_t type = mc->next[0] >> 28;

		switch (type) {
		case 0x0: /* utility */
			switch ((mc->next[0] >> 20) & 0xf) {
			case 0x3: /* DCTPQ */
				mc->info.division = (mc->next[0] & 0xffff);
				break;
			case 0x4: /* DC */
				mc->tick += (mc->next[0] & 0xfffff);
				break;
			}
			break;
		case 0x2: /* midi 1.0 */
		case 0x3: /* sysex 7bits */
		case 0x4: /* midi 2.0 */
			return mc->num;
		case 0xd: /* flex data */
			if (((mc->next[0] >> 8) & 0xff) == 0 &&
			    (mc->next[0] & 0xff) == 0)
				mc->tempo = mc->next[1];
			break;
		case 0xf: /* stream */
			break;
		default:
			break;
		}
		if (mc->pass_all)
			return mc->num;
	}
	return 0;
}

static int open_read(struct midi_clip *mc, const char *filename, struct midi_clip_info *info)
{
	int res;

	if (strcmp(filename, "-") != 0) {
		if ((mc->file = fopen(filename, "r")) == NULL) {
			res = -errno;
			goto exit;
		}
		mc->close = true;
	} else {
		mc->file = stdin;
		mc->close = false;
	}

	if ((res = read_header(mc)) < 0)
		goto exit_close;

	mc->tempo = DEFAULT_TEMPO;
	mc->tick = 0;
	mc->mode = 1;

	next_packet(mc);
	*info = mc->info;
	return 0;

exit_close:
	if (mc->close)
		fclose(mc->file);
exit:
	return res;
}

static inline int write_n(FILE *file, const void *buf, int count)
{
	return fwrite(buf, 1, count, file) == (size_t)count ? count : -errno;
}

static inline int write_be32(FILE *file, uint32_t val)
{
	uint8_t buf[4] = { val >> 24, val >> 16, val >> 8, val };
	return write_n(file, buf, 4);
}

#define CHECK_RES(expr) if ((res = (expr)) < 0) return res

static int write_headers(struct midi_clip *mc)
{
	int res;
	CHECK_RES(write_n(mc->file, "SMF2CLIP", 8));

	/* DC 0 */
	CHECK_RES(write_be32(mc->file, 0x00400000));
	/* DCTPQ division */
	CHECK_RES(write_be32(mc->file, 0x00300000 | mc->info.division));
	/* tempo */
	CHECK_RES(write_be32(mc->file, 0xd0100000));
	CHECK_RES(write_be32(mc->file, mc->tempo));
	CHECK_RES(write_be32(mc->file, 0x00000000));
	CHECK_RES(write_be32(mc->file, 0x00000000));
	/* start */
	CHECK_RES(write_be32(mc->file, 0xf0200000));
	CHECK_RES(write_be32(mc->file, 0x00000000));
	CHECK_RES(write_be32(mc->file, 0x00000000));
	CHECK_RES(write_be32(mc->file, 0x00000000));

	return 0;
}

static int open_write(struct midi_clip *mc, const char *filename, struct midi_clip_info *info)
{
	int res;

	if (info->format != 0)
		return -EINVAL;
	if (info->division == 0)
		info->division = 96;

	if (strcmp(filename, "-") != 0) {
		if ((mc->file = fopen(filename, "w")) == NULL) {
			res = -errno;
			goto exit;
		}
		mc->close = true;
	} else {
		mc->file = stdout;
		mc->close = false;
	}
	mc->mode = 2;
	mc->tempo = DEFAULT_TEMPO;
	mc->info = *info;

	res = write_headers(mc);
exit:
	return res;
}

struct midi_clip *
midi_clip_open(const char *filename, const char *mode, struct midi_clip_info *info)
{
	int res;
	struct midi_clip *mc;

	mc = calloc(1, sizeof(struct midi_clip));
	if (mc == NULL)
		return NULL;

	if (spa_streq(mode, "r")) {
		if ((res = open_read(mc, filename, info)) < 0)
			goto exit_free;
	} else if (spa_streq(mode, "w")) {
		if ((res = open_write(mc, filename, info)) < 0)
			goto exit_free;
	} else {
		res = -EINVAL;
		goto exit_free;
	}
	return mc;

exit_free:
	free(mc);
	errno = -res;
	return NULL;
}

int midi_clip_close(struct midi_clip *mc)
{
	int res;

	if (mc->mode == 2) {
		CHECK_RES(write_be32(mc->file, 0xf0210000));
		CHECK_RES(write_be32(mc->file, 0x00000000));
		CHECK_RES(write_be32(mc->file, 0x00000000));
		CHECK_RES(write_be32(mc->file, 0x00000000));
	} else if (mc->mode != 1)
		return -EINVAL;

	if (mc->close)
		fclose(mc->file);
	free(mc);
	return 0;
}

int midi_clip_next_time(struct midi_clip *mc, double *sec)
{
	if (mc->num <= 0)
		return 0;

	if (mc->info.division == 0)
		*sec = 0.0;
	else
		*sec = mc->tick_sec + ((mc->tick - mc->tick_start) * (double)mc->tempo) /
			(SEC_AS_10NS * mc->info.division);
	return 1;
}

int midi_clip_read_event(struct midi_clip *mc, struct midi_event *event)
{
	if (midi_clip_next_time(mc, &event->sec) != 1)
		return 0;
	event->track = 0;
	event->type = MIDI_EVENT_TYPE_UMP;
	event->data = mc->data;
	event->size = mc->num * 4;
	memcpy(mc->data, mc->next, event->size);

	next_packet(mc);
	return 1;
}

int midi_clip_write_event(struct midi_clip *mc, const struct midi_event *event)
{
	uint32_t tick;
	void *data;
	size_t size;
	int res, i, ump_size;
	int32_t diff;
	uint32_t ump[4], *ump_data;
	uint64_t state = 0;

	spa_return_val_if_fail(event != NULL, -EINVAL);
	spa_return_val_if_fail(mc != NULL, -EINVAL);
	spa_return_val_if_fail(event->track == 0, -EINVAL);
	spa_return_val_if_fail(event->size > 1, -EINVAL);

	data = event->data;
	size = event->size;

	tick = (uint32_t)(event->sec * (SEC_AS_10NS * mc->info.division) / (double)mc->tempo);

	diff = mc->count++ == 0 ? 0 : tick - mc->tick;
	if (diff > 0 || mc->count == 1)
		CHECK_RES(write_be32(mc->file, 0x00400000 | diff));
	mc->tick = tick;

	while (size > 0) {
		switch (event->type) {
		case MIDI_EVENT_TYPE_UMP:
			ump_data = data;
			ump_size = size;
			size = 0;
			break;
		case MIDI_EVENT_TYPE_MIDI1:
			ump_size = spa_ump_from_midi((uint8_t**)&data, &size,
					ump, sizeof(ump), event->track, &state);
			if (ump_size <= 0)
				return ump_size;
			ump_data = ump;
			break;
		default:
			return -EINVAL;
		}
		for (i = 0; i < ump_size/4; i++)
			CHECK_RES(write_be32(mc->file, ump_data[i]));
	}
	return 0;
}
