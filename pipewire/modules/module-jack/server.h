/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

struct jack_client {
	int ref_num;
	struct client *owner;
	struct jack_client_control *control;
};

struct jack_server {
	pthread_mutex_t lock;

	bool promiscuous;

	struct jack_graph_manager *graph_manager;
	struct jack_engine_control *engine_control;

	struct jack_client* client_table[CLIENT_NUM];
	struct jack_synchro synchro_table[CLIENT_NUM];
};

static inline int
jack_server_allocate_ref_num(struct jack_server *server)
{
	int i;

	for (i = 0; i < CLIENT_NUM; i++)
		if (server->client_table[i] == NULL)
			return i;
	return -1;
}

static inline void
jack_server_free_ref_num(struct jack_server *server, int ref_num)
{
	server->client_table[ref_num] = NULL;
}
