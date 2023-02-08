/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_MESSAGE_H
#define PULSE_SERVER_MESSAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <spa/utils/list.h>
#include <spa/support/log.h>

struct impl;

struct message {
	struct spa_list link;
	struct impl *impl;
	uint32_t extra[4];
	uint32_t channel;
	uint32_t allocated;
	uint32_t length;
	uint32_t offset;
	uint8_t *data;
};

enum {
	TAG_INVALID = 0,
	TAG_STRING = 't',
	TAG_STRING_NULL = 'N',
	TAG_U32 = 'L',
	TAG_U8 = 'B',
	TAG_U64 = 'R',
	TAG_S64 = 'r',
	TAG_SAMPLE_SPEC = 'a',
	TAG_ARBITRARY = 'x',
	TAG_BOOLEAN_TRUE = '1',
	TAG_BOOLEAN_FALSE = '0',
	TAG_BOOLEAN = TAG_BOOLEAN_TRUE,
	TAG_TIMEVAL = 'T',
	TAG_USEC = 'U'  /* 64bit unsigned */,
	TAG_CHANNEL_MAP = 'm',
	TAG_CVOLUME = 'v',
	TAG_PROPLIST = 'P',
	TAG_VOLUME = 'V',
	TAG_FORMAT_INFO = 'f',
};

struct message *message_alloc(struct impl *impl, uint32_t channel, uint32_t size);
void message_free(struct message *msg, bool dequeue, bool destroy);
int message_get(struct message *m, ...);
int message_put(struct message *m, ...);
int message_dump(enum spa_log_level level, struct message *m);

#endif /* PULSE_SERVER_MESSAGE_H */
