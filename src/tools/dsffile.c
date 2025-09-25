/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include <spa/utils/string.h>

#include "dsffile.h"

struct dsf_file {
	uint8_t *buffer;
	size_t offset;

	int mode;
	bool close;
	FILE *file;

	struct dsf_file_info info;
};

static inline uint32_t parse_le32(const uint8_t *in)
{
	return in[0] | (in[1] << 8) | (in[2] << 16) | (in[3] << 24);
}

static inline uint64_t parse_le64(const uint8_t *in)
{
	uint64_t res = in[0];
	res |= ((uint64_t)in[1]) << 8;
	res |= ((uint64_t)in[2]) << 16;
	res |= ((uint64_t)in[3]) << 24;
	res |= ((uint64_t)in[4]) << 32;
	res |= ((uint64_t)in[5]) << 40;
	res |= ((uint64_t)in[6]) << 48;
	res |= ((uint64_t)in[7]) << 56;
	return res;
}

static inline int f_skip(struct dsf_file *f, size_t bytes)
{
	uint8_t data[256];
	while (bytes > 0) {
		size_t s = fread(data, 1, SPA_MIN(bytes, sizeof(data)), f->file);
		bytes -= s;
	}
	return 0;
}

static int read_DSD(struct dsf_file *f)
{
	size_t s;
	uint64_t size;
	uint8_t data[28];

	s = fread(data, 1, 28, f->file);
	if (s < 28 || memcmp(data, "DSD ", 4) != 0)
		return -EINVAL;

	size = parse_le64(data + 4);	/* size of this chunk */
	parse_le64(data + 12);		/* total size */
	parse_le64(data + 20);		/* metadata */
	if (size > s)
		f_skip(f, size - s);
	return 0;
}

static int read_fmt(struct dsf_file *f)
{
	size_t s;
	uint64_t size;
	uint8_t data[52];

	s = fread(data, 1, 52, f->file);
	if (s < 52 || memcmp(data, "fmt ", 4) != 0)
		return -EINVAL;

	size = parse_le64(data + 4);	/* size of this chunk */
	if (parse_le32(data + 12) != 1)	/* version */
		return -EINVAL;
	if (parse_le32(data + 16) != 0)	/* format id */
		return -EINVAL;

	f->info.channel_type = parse_le32(data + 20);
	f->info.channels = parse_le32(data + 24);
	f->info.rate = parse_le32(data + 28);
	f->info.lsb = parse_le32(data + 32) == 1;
	f->info.samples = parse_le64(data + 36);
	f->info.blocksize = parse_le32(data + 44);
	if (size > s)
		f_skip(f, size - s);

	f->buffer = calloc(1, f->info.blocksize * f->info.channels);
	if (f->buffer == NULL)
		return -errno;

	return 0;
}

static int read_data(struct dsf_file *f)
{
	size_t s;
	uint64_t size;
	uint8_t data[12];

	s = fread(data, 1, 12, f->file);
	if (s < 12 || memcmp(data, "data", 4) != 0)
		return -EINVAL;

	size = parse_le64(data + 4);	/* size of this chunk */
	f->info.length = size - 12;
	return 0;
}

static int open_read(struct dsf_file *f, const char *filename, struct dsf_file_info *info)
{
	int res;

	if (strcmp(filename, "-") != 0) {
		if ((f->file = fopen(filename, "r")) == NULL) {
			res = -errno;
			goto exit;
		}
		f->close = true;
	} else {
		f->close = false;
		f->file = stdin;
	}

	if ((res = read_DSD(f)) < 0)
		goto exit_close;
	if ((res = read_fmt(f)) < 0)
		goto exit_close;
	if ((res = read_data(f)) < 0)
		goto exit_close;

	f->mode = 1;
	*info = f->info;
	return 0;

exit_close:
	if (f->close)
		fclose(f->file);
exit:
	return res;
}

struct dsf_file *
dsf_file_open(const char *filename, const char *mode, struct dsf_file_info *info)
{
        int res;
        struct dsf_file *f;

        f = calloc(1, sizeof(struct dsf_file));
        if (f == NULL)
                return NULL;

        if (spa_streq(mode, "r")) {
                if ((res = open_read(f, filename, info)) < 0)
                        goto exit_free;
        } else {
                res = -EINVAL;
                goto exit_free;
        }
        return f;

exit_free:
        free(f);
        errno = -res;
        return NULL;
}

uint32_t dsf_layout_stride(const struct dsf_layout *layout)
{
	return layout->channels * SPA_ABS(layout->interleave);
}

static const uint8_t bitrev[256] = {
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
};

ssize_t
dsf_file_read(struct dsf_file *f, void *data, size_t samples, const struct dsf_layout *layout)
{
	uint8_t *d = data;
	int step = SPA_ABS(layout->interleave);
	bool rev = layout->lsb != f->info.lsb;
	size_t total, block, offset, pos;
	size_t blocksize = f->info.blocksize * f->info.channels;

	block = f->offset / f->info.blocksize;
	offset = block * blocksize;
	pos = f->offset % f->info.blocksize;

	for (total = 0; total < samples; total++) {
		uint32_t i;

		if (pos == 0) {
			if (fread(f->buffer, 1, blocksize, f->file) != blocksize)
				break;
		}
		if (f->info.length > 0 && offset + pos >= f->info.length) {
			break;
		}
		for (i = 0; i < layout->channels; i++) {
			const uint8_t *c = &f->buffer[f->info.blocksize * i + pos];
			int j;

			if (layout->interleave > 0) {
				for (j = 0; j < step; j++)
					*d++ = rev ? bitrev[c[j]] : c[j];
			} else {
				for (j = step-1; j >= 0; j--)
					*d++ = rev ? bitrev[c[j]] : c[j];
			}
		}
		pos += step;
		if (pos == f->info.blocksize) {
			pos = 0;
			offset += blocksize;
		}
	}
	f->offset += total * step;

	return total;
}

int dsf_file_close(struct dsf_file *f)
{
	if (f->close)
		fclose(f->file);
	free(f->buffer);
	free(f);
	return 0;
}
