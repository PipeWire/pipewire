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

#include <pulse/scache.h>

#include "internal.h"

SPA_EXPORT
int pa_stream_connect_upload(pa_stream *s, size_t length)
{
	pw_log_warn("Not Implemented");
	return 0;
}

SPA_EXPORT
int pa_stream_finish_upload(pa_stream *s)
{
	pw_log_warn("Not Implemented");
	return 0;
}

SPA_EXPORT
pa_operation* pa_context_remove_sample(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_play_sample(pa_context *c, const char *name, const char *dev,
        pa_volume_t volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_play_sample_with_proplist(pa_context *c, const char *name,
        const char *dev, pa_volume_t volume, pa_proplist *proplist,
        pa_context_play_sample_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}
