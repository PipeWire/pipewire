/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#include <pipewire/log.h>

#include <pulse/ext-device-restore.h>

#include "internal.h"

pa_operation *pa_ext_device_restore_test(
        pa_context *c,
        pa_ext_device_restore_test_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation *pa_ext_device_restore_subscribe(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

void pa_ext_device_restore_set_subscribe_cb(
        pa_context *c,
        pa_ext_device_restore_subscribe_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
}

pa_operation *pa_ext_device_restore_read_formats_all(
        pa_context *c,
        pa_ext_device_restore_read_device_formats_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation *pa_ext_device_restore_read_formats(
        pa_context *c,
        pa_device_type_t type,
        uint32_t idx,
        pa_ext_device_restore_read_device_formats_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation *pa_ext_device_restore_save_formats(
        pa_context *c,
        pa_device_type_t type,
        uint32_t idx,
        uint8_t n_formats,
        pa_format_info **formats,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}
