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

#include <pipewire/pipewire.h>

#include <pulse/ext-stream-restore.h>

#include "internal.h"

/** Test if this extension module is available in the server. \since 0.9.12 */
pa_operation *pa_ext_stream_restore_test(
        pa_context *c,
        pa_ext_stream_restore_test_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

/** Read all entries from the stream database. \since 0.9.12 */
pa_operation *pa_ext_stream_restore_read(
        pa_context *c,
        pa_ext_stream_restore_read_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

/** Store entries in the stream database. \since 0.9.12 */
pa_operation *pa_ext_stream_restore_write(
        pa_context *c,
        pa_update_mode_t mode,
        const pa_ext_stream_restore_info data[],
        unsigned n,
        int apply_immediately,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

/** Delete entries from the stream database. \since 0.9.12 */
pa_operation *pa_ext_stream_restore_delete(
        pa_context *c,
        const char *const s[],
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

/** Subscribe to changes in the stream database. \since 0.9.12 */
pa_operation *pa_ext_stream_restore_subscribe(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

/** Set the subscription callback that is called when
 * pa_ext_stream_restore_subscribe() was called. \since 0.9.12 */
void pa_ext_stream_restore_set_subscribe_cb(
        pa_context *c,
        pa_ext_stream_restore_subscribe_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
}
