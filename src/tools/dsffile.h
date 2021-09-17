/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

struct dsf_file;

struct dsf_file_info {
	uint32_t channel_type;
	uint32_t channels;
	uint32_t rate;
	bool lsb;
	uint64_t samples;
	uint64_t length;
	uint32_t blocksize;
};

struct dsf_layout {
	int32_t interleave;
	uint32_t channels;
	bool lsb;
};

struct dsf_file * dsf_file_open(const char *filename, const char *mode, struct dsf_file_info *info);

ssize_t dsf_file_read(struct dsf_file *f, void *data, size_t samples, const struct dsf_layout *layout);

int dsf_file_close(struct dsf_file *f);

