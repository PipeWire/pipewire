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

#include <spa/type-map.h>
#include <spa/clock.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/node.h>
#include <spa/param-alloc.h>
#include <spa/list.h>
#include <spa/format-builder.h>
#include <lib/props.h>

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
	int size;
	bool do_trace;

	if ((do_trace = (level == SPA_LOG_LEVEL_TRACE && impl->have_source)))
		level++;

	vsnprintf(text, sizeof(text), fmt, args);
	size = snprintf(location, sizeof(location), "[%s][%s:%i %s()] %s\n",
		levels[level], strrchr(file, '/') + 1, line, func, text);

	if (SPA_UNLIKELY(do_trace)) {
		uint32_t index;
		uint64_t count = 1;

		spa_ringbuffer_get_write_index(&impl->trace_rb, &index);
		spa_ringbuffer_write_data(&impl->trace_rb, impl->trace_data,
					  index & impl->trace_rb.mask, location, size);
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

		if (avail > impl->trace_rb.size) {
			index += avail - impl->trace_rb.size;
			avail = impl->trace_rb.size;
		}
		offset = index & impl->trace_rb.mask;
		first = SPA_MIN(avail, impl->trace_rb.size - offset);

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

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct impl *) handle;

	if (interface_id == this->type.log)
		*interface = &this->log;
	else
		return SPA_RESULT_UNKNOWN_INTERFACE;

	return SPA_RESULT_OK;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct impl *) handle;

	if (this->have_source) {
		spa_loop_remove_source(this->source.loop, &this->source);
		close(this->source.fd);
		this->have_source = false;
	}
	return SPA_RESULT_OK;
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

	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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
		return SPA_RESULT_ERROR;
	}
	init_type(&this->type, this->map);

	if (loop) {
		this->source.func = on_trace_event;
		this->source.data = this;
		this->source.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		this->source.mask = SPA_IO_IN;
		this->source.rmask = 0;
		spa_loop_add_source(loop, &this->source);
		this->have_source = true;
	}

	spa_ringbuffer_init(&this->trace_rb, TRACE_BUFFER);

	spa_log_debug(&this->log, NAME " %p: initialized", this);

	return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Log,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	switch (index) {
	case 0:
		*info = &impl_interfaces[index];
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}
	return SPA_RESULT_OK;
}

static const struct spa_handle_factory logger_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	spa_handle_factory_register(&logger_factory);
}
