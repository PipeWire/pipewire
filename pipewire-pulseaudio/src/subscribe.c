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

#include <pulse/subscribe.h>

#include "internal.h"

struct subscribe_data
{
	pa_context_success_cb_t cb;
	void *userdata;
};

SPA_EXPORT
void pa_context_set_subscribe_callback(pa_context *c, pa_context_subscribe_cb_t cb, void *userdata)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_FAILED)
		return;

	c->subscribe_callback = cb;
	c->subscribe_userdata = userdata;
}
