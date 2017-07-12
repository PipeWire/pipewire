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

struct pw_protocol *pw_protocol_new(struct pw_core *core,
				    const char *name,
				    size_t user_data_size)
{
	struct pw_protocol *protocol;

	if (pw_core_find_protocol(core, name) != NULL)
		return NULL;

	protocol = calloc(1, sizeof(struct impl) + user_data_size);
	protocol->core = core;
	protocol->name = strdup(name);

	spa_list_init(&protocol->iface_list);
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
	struct pw_protocol_iface *iface, *t1;
	struct pw_protocol_listener *listener, *t2;
	struct pw_protocol_connection *connection, *t3;

	pw_log_info("protocol %p: destroy", protocol);
	pw_signal_emit(&protocol->destroy_signal, protocol);

	spa_list_remove(&protocol->link);

	spa_list_for_each_safe(iface, t1, &protocol->iface_list, link)
		free(iface);

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
pw_protocol_add_interfaces(struct pw_protocol *protocol,
			   const struct pw_interface *client_iface,
			   const struct pw_interface *server_iface)
{
	struct pw_protocol_iface *iface;
	const char *type;
	uint32_t version;

	iface = calloc(1, sizeof(struct pw_protocol_iface));
	iface->client_iface = client_iface;
	iface->server_iface = server_iface;

	spa_list_insert(protocol->iface_list.prev, &iface->link);

	type = client_iface ? client_iface->type : server_iface->type;
	version = client_iface ? client_iface->version : server_iface->version;

	pw_log_info("Add iface %s:%d to protocol %s", type, version, protocol->name);
}

const struct pw_interface *
pw_protocol_get_interface(struct pw_protocol *protocol,
			  const char *type,
			  bool server)
{
	struct pw_protocol_iface *protocol_iface;

	if (protocol == NULL)
		return NULL;

	spa_list_for_each(protocol_iface, &protocol->iface_list, link) {
		const struct pw_interface *iface = server ? protocol_iface->server_iface :
							    protocol_iface->client_iface;
		if (strcmp(iface->type, type) == 0)
                        return iface;
        }
	return NULL;
}
