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

#define _GNU_SOURCE

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sched.h>

#include <spa/support/log.h>
#include <spa/support/cpu.h>
#include <spa/support/plugin.h>
#include <spa/utils/type.h>

#define NAME "cpu"

struct impl {
	struct spa_handle handle;
	struct spa_cpu cpu;

	struct spa_log *log;

	uint32_t flags;
	uint32_t force;
	uint32_t count;
	uint32_t max_align;
};

# if defined (__i386__) || defined (__x86_64__)
#include "cpu-x86.c"
#define init(t)	x86_init(t)
#endif

static uint32_t
impl_cpu_get_flags(struct spa_cpu *cpu)
{
	struct impl *impl = SPA_CONTAINER_OF(cpu, struct impl, cpu);
	if (impl->force != SPA_CPU_FORCE_AUTODETECT)
		return impl->force;
	return impl->flags;
}

static int
impl_cpu_force_flags(struct spa_cpu *cpu, uint32_t flags)
{
	struct impl *impl = SPA_CONTAINER_OF(cpu, struct impl, cpu);
	impl->force = flags;
	return 0;
}

static uint32_t get_count(struct impl *this)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0)
		return CPU_COUNT(&cpuset);
	return 1;
}

static uint32_t
impl_cpu_get_count(struct spa_cpu *cpu)
{
	struct impl *impl = SPA_CONTAINER_OF(cpu, struct impl, cpu);
	return impl->count;
}

static uint32_t
impl_cpu_get_max_align(struct spa_cpu *cpu)
{
	struct impl *impl = SPA_CONTAINER_OF(cpu, struct impl, cpu);
	return impl->max_align;
}

static const struct spa_cpu impl_cpu = {
	SPA_VERSION_CPU,
	NULL,
	impl_cpu_get_flags,
	impl_cpu_force_flags,
	impl_cpu_get_count,
	impl_cpu_get_max_align,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_CPU)
		*interface = &this->cpu;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
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

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->cpu = impl_cpu;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}
	this->flags = 0;
	this->force = SPA_CPU_FORCE_AUTODETECT;
	this->max_align = 16;
	this->count = get_count(this);
	init(this);

	spa_log_debug(this->log, NAME " %p: count:%d align:%d flags:%08x",
			this, this->count, this->max_align, this->flags);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_CPU,},
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

static const struct spa_handle_factory cpu_factory = {
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
	spa_handle_factory_register(&cpu_factory);
}
