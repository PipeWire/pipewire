/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>

#include <spa/utils/defs.h>

struct dff_file;

struct dff_file_info {
	uint32_t channel_type;
	uint32_t channels;
	uint32_t rate;
	bool lsb;
	uint64_t samples;
	uint64_t length;
	uint32_t blocksize;
};

struct dff_layout {
	int32_t interleave;
	uint32_t channels;
	bool lsb;
};

struct dff_file * dff_file_open(const char *filename, const char *mode, struct dff_file_info *info);

ssize_t dff_file_read(struct dff_file *f, void *data, size_t samples, const struct dff_layout *layout);

int dff_file_close(struct dff_file *f);
