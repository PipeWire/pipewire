/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/eventfd.h>

#include <spa/support/type-map.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/utils/ringbuffer.h>

#define NAME "logger"

#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_INFO

#define TRACE_BUFFER (16*1024)

struct type {
	uint32_t log;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->log = spa_type_map_get_id(map, SPA_TYPE__Log);
}

struct impl {
	struct spa_handle handle;
	struct spa_log log;

	struct type type;
	struct spa_type_map *map;

	bool colors;

	struct spa_ringbuffer trace_rb;
	uint8_t trace_data[TRACE_BUFFER];

	bool have_source;
	struct spa_source source;
};

static void
impl_log_logv(struct spa_log *log,
	      enum spa_log_level level,
	      const char *file,
	      int line,
	      const char *func,
	      const char *fmt,
	      va_list args)
{
	struct impl *impl = SPA_CONTAINER_OF(log, struct impl, log);
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
		if (prefix[0])
			suffix = "\x1B[0m";
	}

	vsnprintf(text, sizeof(text), fmt, args);
	size = snprintf(location, sizeof(location), "%s[%s][%s:%i %s()] %s%s\n",
		prefix, levels[level], strrchr(file, '/') + 1, line, func, text, suffix);

	if (SPA_UNLIKELY(do_trace)) {
		uint32_t index;
		uint64_t count = 1;

		spa_ringbuffer_get_write_index(&impl->trace_rb, &index);
		spa_ringbuffer_write_data(&impl->trace_rb, impl->trace_data, TRACE_BUFFER,
					  index & (TRACE_BUFFER - 1), location, size);
		spa_ringbuffer_write_update(&impl->trace_rb, index + size);

		if (write(impl->source.fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
			fprintf(stderr, "error signaling eventfd: %s\n", strerror(errno));
	} else
		fputs(location, stderr);
}


static void
impl_log_log(struct spa_log *log,
	     enum spa_log_level level,
	     const char *file,
	     int line,
	     const char *func,
	     const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	impl_log_logv(log, level, file, line, func, fmt, args);
	va_end(args);
}

static void on_trace_event(struct spa_source *source)
{
	struct impl *impl = source->data;
	int32_t avail;
	uint32_t index;
	uint64_t count;

	if (read(source->fd, &count, sizeof(uint64_t)) != sizeof(uint64_t))
		fprintf(stderr, "failed to read event fd: %s", strerror(errno));

	while ((avail = spa_ringbuffer_get_read_index(&impl->trace_rb, &index)) > 0) {
		uint32_t offset, first;

		if (avail > TRACE_BUFFER) {
			index += avail - TRACE_BUFFER;
			avail = TRACE_BUFFER;
		}
		offset = index & (TRACE_BUFFER - 1);
		first = SPA_MIN(avail, TRACE_BUFFER - offset);

		fwrite(impl->trace_data + offset, first, 1, stderr);
		if (SPA_UNLIKELY(avail > first)) {
			fwrite(impl->trace_data, avail - first, 1, stderr);
		}
		spa_ringbuffer_read_update(&impl->trace_rb, index + avail);
        }
}

static const struct spa_log impl_log = {
	SPA_VERSION_LOG,
	NULL,
	DEFAULT_LOG_LEVEL,
	impl_log_log,
	impl_log_logv,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == this->type.log)
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

	this->log = impl_log;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		if (strcmp(support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
			loop = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(&this->log, "a type-map is needed");
		return -EINVAL;
	}
	init_type(&this->type, this->map);

	if (info && (str = spa_dict_lookup(info, "log.colors")) != NULL)
		this->colors = (strcmp(str, "true") == 0 || atoi(str) == 1);

	if (loop) {
		this->source.func = on_trace_event;
		this->source.data = this;
		this->source.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
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
	{SPA_TYPE__Log,},
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

static const struct spa_handle_factory logger_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

int spa_handle_factory_register(const struct spa_handle_factory *factory);

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	spa_handle_factory_register(&logger_factory);
}
