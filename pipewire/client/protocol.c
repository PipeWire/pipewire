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

#include <pipewire/client/protocol.h>

static struct impl {
	bool init;
	struct spa_list protocol_list;
} protocols;

static struct impl *get_impl(void)
{
	if (!protocols.init) {
		spa_list_init(&protocols.protocol_list);
		protocols.init = true;
	}
	return &protocols;
}

struct pw_protocol *pw_protocol_get(const char *name)
{
	struct impl *impl = get_impl();
	struct pw_protocol *protocol;

	spa_list_for_each(protocol, &impl->protocol_list, link) {
		if (strcmp(protocol->name, name) == 0)
                        return protocol;
        }

	protocol = calloc(1, sizeof(struct pw_protocol));
	protocol->name = name;
	spa_list_init(&protocol->iface_list);
	spa_list_insert(impl->protocol_list.prev, &protocol->link);

	pw_log_info("Created protocol %s", name);

	return protocol;
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
