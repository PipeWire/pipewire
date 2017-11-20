/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <pipewire/protocol.h>
#include <pipewire/private.h>

/** \cond */
struct impl {
	struct pw_protocol this;
};

struct marshal {
	struct spa_list link;
	const struct pw_protocol_marshal *marshal;
	uint32_t type;
};
/** \endcond */

struct pw_protocol *pw_protocol_new(struct pw_core *core,
				    const char *name,
				    size_t user_data_size)
{
	struct pw_protocol *protocol;

	protocol = calloc(1, sizeof(struct impl) + user_data_size);
	if (protocol == NULL)
		return NULL;

	protocol->core = core;
	protocol->name = strdup(name);

	spa_list_init(&protocol->marshal_list);
	spa_list_init(&protocol->server_list);
	spa_list_init(&protocol->client_list);
	spa_hook_list_init(&protocol->listener_list);

	if (user_data_size > 0)
		protocol->user_data = SPA_MEMBER(protocol, sizeof(struct impl), void);

	spa_list_append(&core->protocol_list, &protocol->link);

	pw_log_info("protocol %p: Created protocol %s", protocol, name);

	return protocol;
}

void *pw_protocol_get_user_data(struct pw_protocol *protocol)
{
	return protocol->user_data;
}

const struct pw_protocol_implementaton *
pw_protocol_get_implementation(struct pw_protocol *protocol)
{
	return protocol->implementation;
}

const void *
pw_protocol_get_extension(struct pw_protocol *protocol)
{
	return protocol->extension;
}

void pw_protocol_destroy(struct pw_protocol *protocol)
{
	struct impl *impl = SPA_CONTAINER_OF(protocol, struct impl, this);
	struct marshal *marshal, *t1;
	struct pw_protocol_server *server, *t2;
	struct pw_protocol_client *client, *t3;

	pw_log_info("protocol %p: destroy", protocol);
	spa_hook_list_call(&protocol->listener_list, struct pw_protocol_events, destroy);

	spa_list_remove(&protocol->link);

	spa_list_for_each_safe(server, t2, &protocol->server_list, link)
		pw_protocol_server_destroy(server);

	spa_list_for_each_safe(client, t3, &protocol->client_list, link)
		pw_protocol_client_destroy(client);

	spa_list_for_each_safe(marshal, t1, &protocol->marshal_list, link)
		free(marshal);

	free(protocol->name);

	free(impl);
}

void pw_protocol_add_listener(struct pw_protocol *protocol,
                              struct spa_hook *listener,
                              const struct pw_protocol_events *events,
                              void *data)
{
	spa_hook_list_append(&protocol->listener_list, listener, events, data);
}

void
pw_protocol_add_marshal(struct pw_protocol *protocol,
			const struct pw_protocol_marshal *marshal)
{
	struct marshal *impl;

	impl = calloc(1, sizeof(struct marshal));
	impl->marshal = marshal;
	impl->type = spa_type_map_get_id (protocol->core->type.map, marshal->type);

	spa_list_append(&protocol->marshal_list, &impl->link);

	pw_log_info("Add marshal %s:%d to protocol %s", marshal->type, marshal->version,
			protocol->name);
}

const struct pw_protocol_marshal *
pw_protocol_get_marshal(struct pw_protocol *protocol, uint32_t type)
{
	struct marshal *impl;

	if (protocol == NULL)
		return NULL;

	spa_list_for_each(impl, &protocol->marshal_list, link) {
		if (impl->type == type)
                        return impl->marshal;
        }
	return NULL;
}

struct pw_protocol *pw_core_find_protocol(struct pw_core *core, const char *name)
{
	struct pw_protocol *protocol;
	spa_list_for_each(protocol, &core->protocol_list, link) {
		if (strcmp(protocol->name, name) == 0)
			return protocol;
	}
	return NULL;
}
