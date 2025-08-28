/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>

#include <spa/utils/defs.h>

#include "midievent.h"

struct midi_file;

struct midi_file_info {
	uint16_t format;
	uint16_t ntracks;
	uint16_t division;
};

struct midi_file *
midi_file_open(const char *filename, const char *mode, struct midi_file_info *info);

int midi_file_close(struct midi_file *mf);

int midi_file_next_time(struct midi_file *mf, double *sec);

int midi_file_read_event(struct midi_file *mf, struct midi_event *event);

int midi_file_write_event(struct midi_file *mf, const struct midi_event *event);
