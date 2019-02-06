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

#include <errno.h>

#include <pipewire/log.h>

#include <pulse/ext-device-manager.h>

#include "internal.h"


SPA_EXPORT
pa_operation *pa_ext_device_manager_test(
        pa_context *c,
        pa_ext_device_manager_test_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_read(
        pa_context *c,
        pa_ext_device_manager_read_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_set_device_description(
        pa_context *c,
        const char* device,
        const char* description,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_delete(
        pa_context *c,
        const char *const s[],
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_enable_role_device_priority_routing(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_reorder_devices_for_role(
        pa_context *c,
        const char* role,
        const char** devices,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_subscribe(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
void pa_ext_device_manager_set_subscribe_cb(
        pa_context *c,
        pa_ext_device_manager_subscribe_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
}
