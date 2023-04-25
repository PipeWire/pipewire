/* Spa JACK Client */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include "jack-client.h"

static int jack_process(jack_nframes_t nframes, void *arg)
{
	struct spa_jack_client *client = arg;

	jack_get_cycle_times(client->client,
			&client->current_frames, &client->current_usecs,
			&client->next_usecs, &client->period_usecs);

	jack_transport_query (client->client, &client->pos);

	client->buffer_size = nframes;

	spa_log_trace_fp(client->log, "frames %u", nframes);

	spa_jack_client_emit_process(client);

	return 0;
}

static void jack_shutdown(void* arg)
{
	struct spa_jack_client *client = arg;

	spa_log_warn(client->log, "%p", client);

	spa_jack_client_emit_shutdown(client);

	spa_hook_list_init(&client->listener_list);
	client->client = NULL;
}

static int status_to_result(jack_status_t status)
{
	int res;

	if (status & JackInvalidOption)
		res = -EINVAL;
	else if (status & JackServerFailed)
		res = -ECONNREFUSED;
	else if (status & JackVersionError)
		res = -EPROTO;
	else if (status & JackInitFailure)
		res = -EIO;
	else
		res = -EFAULT;

	return res;
}

int spa_jack_client_open(struct spa_jack_client *client,
		const char *client_name, const char *server_name)
{
	jack_status_t status;

	if (client->client)
		return 0;

	client->client = jack_client_open(client_name,
			JackNoStartServer, &status, NULL);

	if (client->client == NULL)
		return status_to_result(status);

	spa_hook_list_init(&client->listener_list);

	spa_log_info(client->log, "%p: %s", client, client_name);

	jack_set_process_callback(client->client, jack_process, client);
	jack_on_shutdown(client->client, jack_shutdown, client);
	client->frame_rate = jack_get_sample_rate(client->client);
	client->buffer_size = jack_get_buffer_size(client->client);

	return 0;
}


int spa_jack_client_close(struct spa_jack_client *client)
{
	if (client->client == NULL)
		return 0;

	spa_log_info(client->log, "%p:", client);

	spa_jack_client_emit_destroy(client);

	if (jack_client_close(client->client) != 0)
		return -EIO;

	spa_hook_list_init(&client->listener_list);
	client->client = NULL;

	return 0;
}
