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

struct impl {
	struct pw_protocol this;
};

struct marshal {
	struct spa_list link;
	const struct pw_protocol_marshal *marshal;
	uint32_t type;
};

struct pw_protocol *pw_protocol_new(struct pw_core *core,
				    const char *name,
				    size_t user_data_size)
{
	struct pw_protocol *protocol;

	protocol = calloc(1, sizeof(struct impl) + user_data_size);
	protocol->core = core;
	protocol->name = strdup(name);

	spa_list_init(&protocol->marshal_list);
	spa_list_init(&protocol->connection_list);
	spa_list_init(&protocol->listener_list);

	pw_signal_init(&protocol->destroy_signal);

	if (user_data_size > 0)
		protocol->user_data = SPA_MEMBER(protocol, sizeof(struct impl), void);

	spa_list_insert(core->protocol_list.prev, &protocol->link);

	pw_log_info("protocol %p: Created protocol %s", protocol, name);

	return protocol;
}

void pw_protocol_destroy(struct pw_protocol *protocol)
{
	struct impl *impl = SPA_CONTAINER_OF(protocol, struct impl, this);
	struct marshal *marshal, *t1;
	struct pw_protocol_listener *listener, *t2;
	struct pw_protocol_connection *connection, *t3;

	pw_log_info("protocol %p: destroy", protocol);
	pw_signal_emit(&protocol->destroy_signal, protocol);

	spa_list_remove(&protocol->link);

	spa_list_for_each_safe(marshal, t1, &protocol->marshal_list, link)
		free(marshal);

	spa_list_for_each_safe(listener, t2, &protocol->listener_list, link)
		pw_protocol_listener_destroy(listener);

	spa_list_for_each_safe(connection, t3, &protocol->connection_list, link)
		pw_protocol_connection_destroy(connection);

	free(protocol->name);

	if (protocol->destroy)
		protocol->destroy(protocol);

	free(impl);
}

void
pw_protocol_add_marshal(struct pw_protocol *protocol,
			const struct pw_protocol_marshal *marshal)
{
	struct marshal *impl;

	impl = calloc(1, sizeof(struct marshal));
	impl->marshal = marshal;
	impl->type = spa_type_map_get_id (protocol->core->type.map, marshal->type);

	spa_list_insert(protocol->marshal_list.prev, &impl->link);

	pw_log_info("Add marshal %s:%d to protocol %s", marshal->type, marshal->version, protocol->name);
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
