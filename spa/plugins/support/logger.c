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

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/system.h>
#include <spa/support/plugin.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/type.h>

#define NAME "logger"

#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_INFO

#define TRACE_BUFFER (16*1024)

struct impl {
	struct spa_handle handle;
	struct spa_log log;

	struct spa_system *system;

	bool colors;
	FILE *file;

	struct spa_ringbuffer trace_rb;
	uint8_t trace_data[TRACE_BUFFER];

	bool have_source;
	struct spa_source source;
};

static void
impl_log_logv(void *object,
	      enum spa_log_level level,
	      const char *file,
	      int line,
	      const char *func,
	      const char *fmt,
	      va_list args)
{
	struct impl *impl = object;
	char text[512], location[1024];
	static const char *levels[] = { "-", "E", "W", "I", "D", "T", "*T*" };
	const char *prefix = "", *suffix = "";
	int size;
	bool do_trace;

	if ((do_trace = (level == SPA_LOG_LEVEL_TRACE && impl->have_source)))
		level++;

	if (impl->colors) {
		if (level <= SPA_LOG_LEVEL_ERROR)
			prefix = "\x1B[1;31m";
		else if (level <= SPA_LOG_LEVEL_WARN)
			prefix = "\x1B[1;33m";
		else if (level <= SPA_LOG_LEVEL_INFO)
			prefix = "\x1B[1;32m";
		if (prefix[0])
			suffix = "\x1B[0m";
	}

	vsnprintf(text, sizeof(text), fmt, args);
	size = snprintf(location, sizeof(location), "%s[%s][%s:%i %s()] %s%s\n",
		prefix, levels[level], strrchr(file, '/') + 1, line, func, text, suffix);

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
	fflush(impl->file);
}


static void
impl_log_log(void *object,
	     enum spa_log_level level,
	     const char *file,
	     int line,
	     const char *func,
	     const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	impl_log_logv(object, level, file, line, func, fmt, args);
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
		fflush(impl->file);
        }
}

static const struct spa_log_methods impl_log = {
	SPA_VERSION_LOG_METHODS,
	.log = impl_log_log,
	.logv = impl_log_logv,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Log)
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

	if (this->have_source) {
		spa_loop_remove_source(this->source.loop, &this->source);
		close(this->source.fd);
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
	uint32_t i;
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

	for (i = 0; i < n_support; i++) {
		switch (support[i].type) {
		case SPA_TYPE_INTERFACE_Loop:
			loop = support[i].data;
			break;
		case SPA_TYPE_INTERFACE_System:
			this->system = support[i].data;
			break;
		}
	}
	if (info) {
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_COLORS)) != NULL)
			this->colors = (strcmp(str, "true") == 0 || atoi(str) == 1);
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_LEVEL)) != NULL)
			this->log.level = atoi(str);
		if ((str = spa_dict_lookup(info, SPA_KEY_LOG_FILE)) != NULL) {
			this->file = fopen(str, "w");
			if (this->file == NULL)
				fprintf(stderr, "failed to open file %s: (%s)", str, strerror(errno));
		}
	}
	if (this->file == NULL)
		this->file = stderr;

	if (loop != NULL && this->system != NULL) {
		this->source.func = on_trace_event;
		this->source.data = this;
		this->source.fd = spa_system_eventfd_create(this->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		this->source.mask = SPA_IO_IN;
		this->source.rmask = 0;
		spa_loop_add_source(loop, &this->source);
		this->have_source = true;
	}

	spa_ringbuffer_init(&this->trace_rb);

	spa_log_debug(&this->log, NAME " %p: initialized", this);

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
	NAME,
	NULL,
	.get_size = impl_get_size,
	.init = impl_init,
	.enum_interface_info = impl_enum_interface_info,
};
