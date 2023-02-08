/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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
