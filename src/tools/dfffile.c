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
#include <spa/debug/mem.h>

#include "dfffile.h"

struct dff_file {
	uint8_t *data;
	size_t size;

	int mode;
	int fd;

	struct dff_file_info info;

	uint8_t *p;
	size_t offset;
};

struct dff_chunk {
	uint32_t id;
	uint64_t size;
	void *data;
};

#define FOURCC(a,b,c,d) (d | (c << 8) | (b << 16) | (a << 24))

static inline uint16_t parse_be16(const uint8_t *in)
{
	return (in[0] << 8) | in[1];
}
static inline uint32_t parse_be32(const uint8_t *in)
{
	return FOURCC(in[0], in[1], in[2], in[3]);
}

static inline uint64_t parse_be64(const uint8_t *in)
{
	uint64_t res = in[7];
	res |= ((uint64_t)in[6]) << 8;
	res |= ((uint64_t)in[5]) << 16;
	res |= ((uint64_t)in[4]) << 24;
	res |= ((uint64_t)in[3]) << 32;
	res |= ((uint64_t)in[2]) << 40;
	res |= ((uint64_t)in[1]) << 48;
	res |= ((uint64_t)in[0]) << 56;
	return res;
}

static inline int f_avail(struct dff_file *f)
{
	if (f->p < f->data + f->size)
		return f->size + f->data - f->p;
	return 0;
}

static int read_chunk(struct dff_file *f, struct dff_chunk *c)
{
	if (f_avail(f) < 12)
		return -ENOSPC;

	c->id = parse_be32(f->p);	/* id of this chunk */
	c->size = parse_be64(f->p + 4);	/* size of this chunk */
	f->p += 12;
	c->data = f->p;
	return 0;
}

static int skip_chunk(struct dff_file *f, const struct dff_chunk *c)
{
	f->p = SPA_PTROFF(c->data, c->size, uint8_t);
	return 0;
}

static int read_PROP(struct dff_file *f, struct dff_chunk *prop)
{
	struct dff_chunk c[1];
	int res;

	if (f_avail(f) < 4 ||
	    memcmp(prop->data, "SND ", 4) != 0)
		return -EINVAL;
	f->p += 4;

	while (f->p < SPA_PTROFF(prop->data, prop->size, uint8_t)) {
		if ((res = read_chunk(f, &c[0])) < 0)
			return res;

		switch (c[0].id) {
		case FOURCC('F', 'S', ' ', ' '):
			f->info.rate = parse_be32(f->p);
			break;
		case FOURCC('C', 'H', 'N', 'L'):
			f->info.channels = parse_be16(f->p);
			switch (f->info.channels) {
			case 2:
				f->info.channel_type = 2;
				break;
			case 5:
				f->info.channel_type = 6;
				break;
			case 6:
				f->info.channel_type = 7;
				break;
			}
			break;
		case FOURCC('C', 'M', 'P', 'R'):
		{
			uint32_t cmpr = parse_be32(f->p);
			if (cmpr != FOURCC('D', 'S', 'D', ' '))
				return -ENOTSUP;
			break;
		}
		case FOURCC('A', 'B', 'S', 'S'):
			break;
		case FOURCC('L', 'S', 'C', 'O'):
			break;
		default:
			break;
		}
		skip_chunk(f, &c[0]);
	}
	return 0;
}

static int read_FRM8(struct dff_file *f)
{
	struct dff_chunk c[2];
	int res;
	bool found_dsd = false;

	if ((res = read_chunk(f, &c[0])) < 0)
		return res;
	if (c[0].id != FOURCC('F','R','M','8'))
		return -EINVAL;
	if (f_avail(f) < 4 ||
	    memcmp(c[0].data, "DSD ", 4) != 0)
		return -EINVAL;
	f->p += 4;

	while (true) {
		if ((res = read_chunk(f, &c[1])) < 0)
			return res;

		switch (c[1].id) {
		case FOURCC('F', 'V', 'E', 'R'):
			break;
		case FOURCC('P', 'R', 'O', 'P'):
			read_PROP(f, &c[1]);
			break;
		case FOURCC('D', 'S', 'D', ' '):
		{
			f->info.length = c[1].size;
			f->info.samples = c[1].size / f->info.channels;
			f->info.lsb = 0;
			f->info.blocksize = 1;
			found_dsd = true;
			break;
		}
		default:
			break;
		}
		if (found_dsd)
			break;

		skip_chunk(f, &c[1]);
	}
	return 0;
}

static int open_read(struct dff_file *f, const char *filename, struct dff_file_info *info)
{
	int res;
	struct stat st;

	if ((f->fd = open(filename, O_RDONLY)) < 0) {
		res = -errno;
		goto exit;
	}
	if (fstat(f->fd, &st) < 0) {
		res = -errno;
		goto exit_close;
	}
	f->size = st.st_size;

	f->data = mmap(NULL, f->size, PROT_READ, MAP_SHARED, f->fd, 0);
	if (f->data == MAP_FAILED) {
		res = -errno;
		goto exit_close;
	}

	f->p = f->data;

	if ((res = read_FRM8(f)) < 0)
		goto exit_unmap;

	f->mode = 1;
	*info = f->info;
	return 0;

exit_unmap:
	munmap(f->data, f->size);
exit_close:
	close(f->fd);
exit:
	return res;
}

struct dff_file *
dff_file_open(const char *filename, const char *mode, struct dff_file_info *info)
{
        int res;
        struct dff_file *f;

        f = calloc(1, sizeof(struct dff_file));
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
dff_file_read(struct dff_file *f, void *data, size_t samples, const struct dff_layout *layout)
{
	uint8_t *d = data;
	int32_t step = SPA_ABS(layout->interleave);
	uint32_t channels = f->info.channels;
	bool rev = layout->lsb != f->info.lsb;
	size_t total, offset, scale;

	offset = f->offset;
	scale = SPA_CLAMP(f->info.rate / (44100u * 64u), 1u, 4u);

	samples *= step;
	samples *= scale;

	for (total = 0; total < samples && offset < f->info.length; total++) {
		uint32_t i;
		int32_t j;
		const uint8_t *s = f->p + offset;

		for (i = 0; i < layout->channels; i++) {
			if (layout->interleave > 0) {
				for (j = 0; j < step; j++)
					*d++ = rev ?
						bitrev[s[j * channels + i]] :
						s[j * channels + i];
			} else {
				for (j = step-1; j >= 0; j--)
					*d++ = rev ?
						bitrev[s[j * channels + i]] :
						s[j * channels + i];
			}
		}
		offset += step * channels;
	}
	f->offset = offset;

	return total;
}

int dff_file_close(struct dff_file *f)
{
	if (f->mode == 1) {
		munmap(f->data, f->size);
	} else
		return -EINVAL;

	close(f->fd);
	free(f);
	return 0;
}
