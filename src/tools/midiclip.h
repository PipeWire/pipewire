/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>

#include <spa/utils/defs.h>

#include "midievent.h"

struct midi_clip;

struct midi_clip_info {
	uint16_t format;
	uint16_t division;
};

struct midi_clip *
midi_clip_open(const char *filename, const char *mode, struct midi_clip_info *info);

int midi_clip_close(struct midi_clip *mc);

int midi_clip_next_time(struct midi_clip *mc, double *sec);

int midi_clip_read_event(struct midi_clip *mc, struct midi_event *event);

int midi_clip_write_event(struct midi_clip *mc, const struct midi_event *event);
