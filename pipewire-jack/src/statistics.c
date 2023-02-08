/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <jack/statistics.h>

SPA_EXPORT
float jack_get_max_delayed_usecs (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	float res = 0.0f;

	spa_return_val_if_fail(c != NULL, 0.0);

	if (c->driver_activation)
		res = (float)c->driver_activation->max_delay / SPA_USEC_PER_SEC;

	pw_log_trace("%p: max delay %f", client, res);
	return res;
}

SPA_EXPORT
float jack_get_xrun_delayed_usecs (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	float res = 0.0f;

	spa_return_val_if_fail(c != NULL, 0.0);

	if (c->driver_activation)
		res = (float)c->driver_activation->xrun_delay / SPA_USEC_PER_SEC;

	pw_log_trace("%p: xrun delay %f", client, res);
	return res;
}

SPA_EXPORT
void jack_reset_max_delayed_usecs (jack_client_t *client)
{
	struct client *c = (struct client *) client;

	spa_return_if_fail(c != NULL);

	if (c->driver_activation)
		c->driver_activation->max_delay = 0;
}
