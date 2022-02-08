/* Spa
 *
 * Copyright © 2018 Wim Taymans
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

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <fnmatch.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/system.h>
#include <spa/support/plugin.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/type.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/ansi.h>

#include "log-patterns.h"

#ifdef __FreeBSD__
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define NAME "logger"

#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_INFO

#define TRACE_BUFFER (16*1024)

struct impl {
	struct spa_handle handle;
	struct spa_log log;

	FILE *file;
	bool close_file;

	struct spa_system *system;
	struct spa_source source;
	struct spa_ringbuffer trace_rb;
	uint8_t trace_data[TRACE_BUFFER];

	unsigned int have_source:1;
	unsigned int colors:1;
	unsigned int timestamp:1;
	unsigned int line:1;

	struct spa_list patterns;
};

static SPA_PRINTF_FUNC(7,0) void
impl_log_logtv(void *object,
	      enum spa_log_level level,
	      const struct spa_log_topic *topic,
	      const char *file,
	      int line,
	      const char *func,
	      const char *fmt,
	      va_list args)
{
#define RESERVED_LENGTH 24

	struct impl *impl = object;
	char timestamp[15] = {0};
	char topicstr[32] = {0};
	char filename[64] = {0};
	char location[1000 + RESERVED_LENGTH], *p, *s;
	static const char * const levels[] = { "-", "E", "W", "I", "D", "T", "*T*" };
	const char *prefix = "", *suffix = "";
	int size, len;
	bool do_trace;

	if ((do_trace = (level == SPA_LOG_LEVEL_TRACE && impl->have_source)))
		level++;

	if (impl->colors) {
		if (level <= SPA_LOG_LEVEL_ERROR)
			prefix = SPA_ANSI_BOLD_RED;
		else if (level <= SPA_LOG_LEVEL_WARN)
			prefix = SPA_ANSI_BOLD_YELLOW;
		else if (level <= SPA_LOG_LEVEL_INFO)
			prefix = SPA_ANSI_BOLD_GREEN;
		if (prefix[0])
			suffix = SPA_ANSI_RESET;
	}

	p = location;
	len = sizeof(location) - RESERVED_LENGTH;

	if (impl->timestamp) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC_RAW, &now);
		spa_scnprintf(timestamp, sizeof(timestamp), "[%05lu.%06lu]",
			(now.tv_sec & 0x1FFFFFFF) % 100000, now.tv_nsec / 1000);
	}

	if (topic && topic->topic)
		spa_scnprintf(topicstr, sizeof(topicstr), " %-12s | ", topic->topic);


	if (impl->line && line != 0) {
		s = strrchr(file, '/');
		spa_scnprintf(filename, sizeof(filename), "[%16.16s:%5i %s()]",
			s ? s + 1 : file, line, func);
	}

	size = spa_scnprintf(p, len, "%s[%s]%s%s%s ", prefix, levels[level],
			     timestamp, topicstr, filename);
	/*
	 * it is assumed that at this point `size` <= `len`,
	 * which is reasonable as long as file names and function names
	 * don't become very long
	 */
	size += spa_vscnprintf(p + size, len - size, fmt, args);

	/*
	 * `RESERVED_LENGTH` bytes are reserved for printing the suffix
	 * (at the moment it's "... (truncated)\x1B[0m\n" at its longest - 21 bytes),
	 * its length must be less than `RESERVED_LENGTH` (including the null byte),
	 * otherwise a stack buffer overrun could ensue
	 */

	/* if the message could not fit entirely... */
	if (size >= len - 1) {
		size = len - 1; /* index of the null byte */
		len = sizeof(location);
		size += spa_scnprintf(p + size, len - size, "... (truncated)");
	}
	else {
		len = sizeof(location);
	}

	size += spa_scnprintf(p + size, len - size, "%s\n", suffix);

	if (SPA_UNLIKELY(do_trace)) {
		uint32_t index;

		spa_ringbuffer_get_write_index(&impl->trace_rb, &index);
		spa_ringbuffer_write_data(&impl->trace_rb, impl->trace_data, TRACE_BUFFER,
					  index & (TRACE_BUFFER - 1), location, size);
		spa_ringbuffer_write_update(&impl->trace_rb, index + size);

		if (spa_system_eventfd_write(impl->system, impl->source.fd, 1) < 0)
			fprintf(impl->file, "error signaling eventfd: %s\n", strerror(errno));
	} else
		fputs(location, impl->file);

#undef RESERVED_LENGTH
}

static SPA_PRINTF_FUNC(6,0) void
impl_log_logv(void *object,
	      enum spa_log_level level,
	      const char *file,
	      int line,
	      const char *func,
	      const char *fmt,
	      va_list args)
{
	impl_log_logtv(object, level, NULL, file, line, func, fmt, args);
}

static SPA_PRINTF_FUNC(7,8) void
impl_log_logt(void *object,
	     enum spa_log_level level,
	     const struct spa_log_topic *topic,
	     const char *file,
	     int line,
	     const char *func,
	     const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	impl_log_logtv(object, level, topic, file, line, func, fmt, args);
	va_end(args);
}

static SPA_PRINTF_FUNC(6,7) void
impl_log_log(void *object,
	     enum spa_log_level level,
	     const char *file,
	     int line,
	     const char *func,
	     const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	impl_log_logtv(object, level, NULL, file, line, func, fmt, args);
	va_end(args);
}

static void on_trace_event(struct spa_source *source)
{
	struct impl *impl = source->data;
	int32_t avail;
	uint32_t index;
	uint64_t count;

	if (spa_system_eventfd_read(impl->system, source->fd, &count) < 0)
		fprintf(impl->file, "failed to read event fd: %s", strerror(errno));

	while ((avail = spa_ringbuffer_get_read_index(&impl->trace_rb, &index)) > 0) {
		int32_t offset, first;

		if (avail > TRACE_BUFFER) {
			index += avail - TRACE_BUFFER;
			avail = TRACE_BUFFER;
		}
		offset = index & (TRACE_BUFFER - 1);
		first = SPA_MIN(avail, TRACE_BUFFER - offset);

		fwrite(impl->trace_data + offset, first, 1, impl->file);
		if (SPA_UNLIKELY(avail > first)) {
			fwrite(impl->trace_data, avail - first, 1, impl->file);
		}
		spa_ringbuffer_read_update(&impl->trace_rb, index + avail);
        }
}

static void
impl_log_topic_init(void *object, struct spa_log_topic *t)
{
	struct impl *impl = object;
	enum spa_log_level level = impl->log.level;

	support_log_topic_init(&impl->patterns, level, t);
}

static const struct spa_log_methods impl_log = {
	SPA_VERSION_LOG_METHODS,
	.log = impl_log_log,
	.logv = impl_log_logv,
	.logt = impl_log_logt,
	.logtv = impl_log_logtv,
	.topic_init = impl_log_topic_init,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Log))
		*interface = &this->log;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	support_log_free_patterns(&this->patterns);

	if (this->close_file && this->file != NULL)
		fclose(this->file);

	if (this->have_source) {
		spa_loop_remove_source(this->source.loop, &this->source);
		spa_system_close(this->system, this->source.fd);
		this->have_source = false;
	}
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	struct spa_loop *loop = NULL;
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Log,
			SPA_VERSION_LOG,
			&impl_log, this);
	this->log.level = DEFAULT_LOG_LEVEL;

	loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	this->system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);
	spa_list_init(&this->patterns);

	if (loop != NULL && this->system != NULL) {
		this->source.func = on_trace_event;
		this->source.data = this;
		this->source.fd = spa_system_eventfd_create(this->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		this->source.mask = SPA_IO_IN;
		this->source.rmask = 0;

		if (this->source.fd < 0) {
			fprintf(stderr, "Warning: failed to create eventfd: %m");
		} else {
			spa_loop_add_source(loop, &this->source);
			this->have_source = true;
		}
	}
	if (info) {
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_TIMESTAMP)) != NULL)
			this->timestamp = spa_atob(str);
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_LINE)) != NULL)
			this->line = spa_atob(str);
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_COLORS)) != NULL)
			this->colors = spa_atob(str);
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_LEVEL)) != NULL)
			this->log.level = atoi(str);
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_FILE)) != NULL) {
			this->file = fopen(str, "w");
			if (this->file == NULL)
				fprintf(stderr, "Warning: failed to open file %s: (%m)", str);
			else
				this->close_file = true;
		}
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_PATTERNS)) != NULL)
			support_log_parse_patterns(&this->patterns, str);
	}
	if (this->file == NULL)
		this->file = stderr;
	if (!isatty(fileno(this->file)))
		this->colors = false;

	spa_ringbuffer_init(&this->trace_rb);

	spa_log_debug(&this->log, NAME " %p: initialized", this);

	setlinebuf(this->file);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Log,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;

	return 1;
}

const struct spa_handle_factory spa_support_logger_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	.name = SPA_NAME_SUPPORT_LOG,
	.info = NULL,
	.get_size = impl_get_size,
	.init = impl_init,
	.enum_interface_info = impl_enum_interface_info,
};
