/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <spa/ringbuffer.h>

#include <pipewire/client/log.h>

#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_ERROR

enum spa_log_level pw_log_level = DEFAULT_LOG_LEVEL;

#define TRACE_BUFFER (16*1024)

struct debug_log {
	struct spa_log log;
	struct spa_ringbuffer trace_rb;
	uint8_t trace_data[TRACE_BUFFER];
	struct spa_source *source;
};

static void
do_logv(struct spa_log *log,
	enum spa_log_level level,
	const char *file, int line, const char *func, const char *fmt, va_list args)
{
	struct debug_log *l = SPA_CONTAINER_OF(log, struct debug_log, log);
	char text[1024], location[1024];
	static const char *levels[] = { "-", "E", "W", "I", "D", "T", "*T*" };
	int size;
	bool do_trace = (level == SPA_LOG_LEVEL_TRACE && l->source);

	vsnprintf(text, sizeof(text), fmt, args);

	if ((do_trace = (level == SPA_LOG_LEVEL_TRACE && l->source)))
		level++;

	size = snprintf(location, sizeof(location), "[%s][%s:%i %s()] %s\n",
			levels[level], strrchr(file, '/') + 1, line, func, text);

	if (SPA_UNLIKELY(do_trace)) {
		uint32_t index;
		uint64_t count = 1;

		spa_ringbuffer_get_write_index(&l->trace_rb, &index);
		spa_ringbuffer_write_data(&l->trace_rb, l->trace_data,
					  index & l->trace_rb.mask, location, size);
		spa_ringbuffer_write_update(&l->trace_rb, index + size);

		write(l->source->fd, &count, sizeof(uint64_t));
	} else
		fputs(location, stdout);
}

static void
do_log(struct spa_log *log,
       enum spa_log_level level, const char *file, int line, const char *func, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	do_logv(log, level, file, line, func, fmt, args);
	va_end(args);
}

static struct debug_log log = {
	{sizeof(struct spa_log),
	 NULL,
	 DEFAULT_LOG_LEVEL,
	 do_log,
	 do_logv,
	 },
	{0, 0, TRACE_BUFFER, TRACE_BUFFER - 1},
};

struct spa_log *pw_log_get(void)
{
	return &log.log;
}

void pw_log_set_level(enum spa_log_level level)
{
	pw_log_level = level;
	log.log.level = level;
}

static void on_trace_event(struct spa_source *source)
{
	int32_t avail;
	uint32_t index;
	uint64_t count;

	if (read(source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
		fprintf(stderr, "failed to read event fd: %s", strerror(errno));

	while ((avail = spa_ringbuffer_get_read_index(&log.trace_rb, &index)) > 0) {
		uint32_t offset, first;

		if (avail > log.trace_rb.size) {
			fprintf(stderr, "\n** trace overflow ** %d\n", avail);
			index += avail - log.trace_rb.size;
			avail = log.trace_rb.size;
		}
		offset = index & log.trace_rb.mask;
		first = SPA_MIN(avail, log.trace_rb.size - offset);

		fwrite(log.trace_data + offset, first, 1, stderr);
		if (SPA_UNLIKELY(avail > first)) {
			fwrite(log.trace_data, avail - first, 1, stderr);
		}
		spa_ringbuffer_read_update(&log.trace_rb, index + avail);
	}
}

void pw_log_set_trace_event(struct spa_source *source)
{
	log.source = source;
	log.source->func = on_trace_event;
	log.source->data = &log;
}

void
pw_log_log(enum spa_log_level level,
	   const char *file, int line, const char *func, const char *fmt, ...)
{
	if (SPA_UNLIKELY(pw_log_level_enabled(level))) {
		va_list args;
		va_start(args, fmt);
		do_logv(&log.log, level, file, line, func, fmt, args);
		va_end(args);
	}
}

void
pw_log_logv(enum spa_log_level level,
	    const char *file, int line, const char *func, const char *fmt, va_list args)
{
	if (SPA_UNLIKELY(pw_log_level_enabled(level))) {
		do_logv(&log.log, level, file, line, func, fmt, args);
	}
}
