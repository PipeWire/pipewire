/* Spa JACK Client
 *
 * Copyright © 2019 Wim Taymans
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

#include <errno.h>

#include "jack-client.h"

static int jack_process(jack_nframes_t nframes, void *arg)
{
	struct spa_jack_client *client = arg;

	client->buffer_size = nframes;
	spa_jack_client_emit_process(client);

	return 0;
}

static void jack_shutdown(void* arg)
{
	struct spa_jack_client *client = arg;

	spa_jack_client_emit_shutdown(client);
}

static int jack_buffer_size(jack_nframes_t nframes, void *arg)
{
	return 0;
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

	jack_set_process_callback(client->client, jack_process, client);
	jack_on_shutdown(client->client, jack_shutdown, client);
	jack_set_buffer_size_callback(client->client, jack_buffer_size, client);

	return 0;
}


int spa_jack_client_close(struct spa_jack_client *client)
{
	if (client->client == NULL)
		return 0;

	spa_jack_client_emit_destroy(client);

	if (jack_client_close(client->client) != 0)
		return -EIO;

	spa_hook_list_init(&client->listener_list);
	client->client = NULL;

	return 0;
}
