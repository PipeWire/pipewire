/* PipeWire
 *
 * Copyright © 2022 Wim Taymans
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
#include <fcntl.h>
#include <unistd.h>

#include <spa/utils/string.h>

#include "wavfile.h"

#define BLOCK_SIZE	4096

struct wav_file {
	struct spa_audio_info info;
	int fd;
	const struct format_info *fi;

	uint32_t length;

	uint32_t stride;
	uint32_t blocks;
};

static inline ssize_t write_data(struct wav_file *wf, const void *data, size_t size)
{
	ssize_t len;
	len = write(wf->fd, data, size);
	if (len > 0)
		wf->length += len;
	return len;
}

static ssize_t writei(struct wav_file *wf, const void **data, size_t samples)
{
	return write_data(wf, data[0], samples * wf->stride);
}

typedef struct {
	uint8_t v[3];
} __attribute__ ((packed)) uint24_t;

#define MAKE_WRITEN_FUNC(name, type)						\
static ssize_t name (struct wav_file *wf, const void **data, size_t samples)	\
{										\
	uint32_t b, n, k, blocks = wf->blocks, chunk;				\
	uint8_t buf[BLOCK_SIZE];						\
	ssize_t res = 0;							\
	type **d = (type**)data;						\
	uint32_t chunk_size = sizeof(buf) / (blocks * sizeof(type));		\
	for (n = 0; n < samples; ) {						\
		type *p = (type*)buf;						\
		chunk = SPA_MIN(samples - n, chunk_size);			\
		for (k = 0; k < chunk; k++, n++) {				\
			for (b = 0; b < blocks; b++)				\
				*p++ = d[b][n];					\
		}								\
		res += write_data(wf, buf,					\
				chunk * blocks * sizeof(type));			\
	}									\
	return res;								\
}

MAKE_WRITEN_FUNC(writen_8, uint8_t);
MAKE_WRITEN_FUNC(writen_16, uint16_t);
MAKE_WRITEN_FUNC(writen_24, uint24_t);
MAKE_WRITEN_FUNC(writen_32, uint32_t);
MAKE_WRITEN_FUNC(writen_64, uint64_t);

static inline int write_n(int fd, const void *buf, int count)
{
	return write(fd, buf, count) == (ssize_t)count ? count : -errno;
}

static inline int write_le16(int fd, uint16_t val)
{
	uint8_t buf[2] = { val, val >> 8 };
	return write_n(fd, buf, 2);
}

static inline int write_le32(int fd, uint32_t val)
{
	uint8_t buf[4] = { val, val >> 8, val >> 16, val >> 24 };
	return write_n(fd, buf, 4);
}

#define MAKE_AUDIO_RAW(format,bits,planar,fmt,...) \
	{ SPA_MEDIA_TYPE_audio, SPA_MEDIA_SUBTYPE_raw, format, bits, planar, fmt, __VA_ARGS__ }

static struct format_info {
	uint32_t media_type;
	uint32_t media_subtype;
	uint32_t format;
	uint32_t bits;
	bool planar;
	uint32_t fmt;
	ssize_t (*write) (struct wav_file *wf, const void **data, size_t samples);
} format_info[] = {
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_U8P,		 8, true, 1, writen_8),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_U8,		 8, false, 1, writei),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S16P,		16, true, 1, writen_16),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S16_LE,		16, false, 1, writei),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S24P,		24, true, 1, writen_24),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S24_LE,		24, false, 1, writei),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S24_32P,	32, true, 1, writen_32),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S32P,		32, true, 1, writen_32),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S24_32_LE,	32, false, 1, writei),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_S32_LE,		32, false, 1, writei),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_F32P,		32, true, 3, writen_32),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_F32_LE,		32, false, 3, writei),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_F64P,		64, true, 3, writen_64),
	MAKE_AUDIO_RAW(SPA_AUDIO_FORMAT_F64_LE,		32, false, 3, writei),
};

#define CHECK_RES(expr) if ((res = (expr)) < 0) return res

static int write_headers(struct wav_file *wf)
{
	int res;
	uint32_t channels, rate, bps, bits;
	const struct format_info *fi = wf->fi;

	lseek(wf->fd, 0, SEEK_SET);

	rate = wf->info.info.raw.rate;
	channels = wf->info.info.raw.channels;
	bits = fi->bits;
	bps = channels * bits / 8;

	CHECK_RES(write_n(wf->fd, "RIFF", 4));
	CHECK_RES(write_le32(wf->fd, wf->length == 0 ? (uint32_t)-1 : wf->length + 12 + 8 + 16));
	CHECK_RES(write_n(wf->fd, "WAVE", 4));
	CHECK_RES(write_n(wf->fd, "fmt ", 4));
	CHECK_RES(write_le32(wf->fd, 16));
	CHECK_RES(write_le16(wf->fd, fi->fmt));			/* format */
	CHECK_RES(write_le16(wf->fd, channels));		/* channels */
	CHECK_RES(write_le32(wf->fd, rate));			/* rate */
	CHECK_RES(write_le32(wf->fd, bps * rate));		/* bytes per sec */
	CHECK_RES(write_le16(wf->fd, bps));			/* bytes per samples */
	CHECK_RES(write_le16(wf->fd, bits));			/* bits per sample */
	CHECK_RES(write_n(wf->fd, "data", 4));
	CHECK_RES(write_le32(wf->fd, wf->length == 0 ? (uint32_t)-1 : wf->length));

	return 0;
}

static const struct format_info *find_info(struct wav_file_info *info)
{
	uint32_t i;

	if (info->info.media_type != SPA_MEDIA_TYPE_audio ||
	    info->info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return NULL;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].format == info->info.info.raw.format)
			return &format_info[i];
	}
	return NULL;
}

static int open_write(struct wav_file *wf, const char *filename, struct wav_file_info *info)
{
	int res;
	const struct format_info *fi;

	fi = find_info(info);
	if (fi == NULL)
		return -ENOTSUP;

	if ((wf->fd = open(filename, O_WRONLY | O_CREAT | O_CLOEXEC | O_TRUNC, 0660)) < 0) {
		res = -errno;
		goto exit;
	}
	wf->info = info->info;
	wf->fi = fi;
	if (fi->planar) {
		wf->stride = fi->bits / 8;
		wf->blocks = info->info.info.raw.channels;
	} else {
		wf->stride = info->info.info.raw.channels * (fi->bits / 8);
		wf->blocks = 1;
	}

	res = write_headers(wf);
exit:
	return res;
}

struct wav_file *
wav_file_open(const char *filename, const char *mode, struct wav_file_info *info)
{
	int res;
	struct wav_file *wf;

	wf = calloc(1, sizeof(struct wav_file));
	if (wf == NULL)
		return NULL;

	if (spa_streq(mode, "w")) {
		if ((res = open_write(wf, filename, info)) < 0)
			goto exit_free;
	} else {
		res = -EINVAL;
		goto exit_free;
	}
	return wf;

exit_free:
	free(wf);
	errno = -res;
	return NULL;
}

int wav_file_close(struct wav_file *wf)
{
	int res;

	CHECK_RES(write_headers(wf));

	close(wf->fd);
	free(wf);
	return 0;
}

ssize_t wav_file_write(struct wav_file *wf, const void **data, size_t samples)
{
	return wf->fi->write(wf, data, samples);
}
