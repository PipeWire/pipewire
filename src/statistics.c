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

#include <jack/statistics.h>

SPA_EXPORT
float jack_get_max_delayed_usecs (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	float res = 0.0f;

	if (c->driver_activation)
		res = (float)c->driver_activation->max_delay / SPA_USEC_PER_SEC;

	pw_log_trace(NAME" %p: max delay %f", client, res);
	return res;
}

SPA_EXPORT
float jack_get_xrun_delayed_usecs (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	float res = 0.0f;

	if (c->driver_activation)
		res = (float)c->driver_activation->xrun_delay / SPA_USEC_PER_SEC;

	pw_log_trace(NAME" %p: xrun delay %f", client, res);
	return res;
}

SPA_EXPORT
void jack_reset_max_delayed_usecs (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	if (c->driver_activation)
		c->driver_activation->max_delay = 0;
}
