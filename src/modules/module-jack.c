/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>

#include "config.h"

#include "pipewire/pipewire.h"
#include "pipewire/log.h"
#include "pipewire/interfaces.h"

#include "pipewire/core.h"
#include "pipewire/node.h"
#include "pipewire/module.h"
#include "pipewire/client.h"
#include "pipewire/resource.h"
#include "pipewire/private.h"
#include "pipewire/link.h"
#include "pipewire/node-factory.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"

#include "modules/module-jack/jack.h"
#include "modules/module-jack/jack-node.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

int segment_num = 0;

typedef bool(*demarshal_func_t) (void *object, void *data, size_t size);

struct socket {
	int fd;
	struct sockaddr_un addr;
	char lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];

	struct pw_loop *loop;
	struct spa_source *source;
	struct spa_list link;
};

struct impl {
	struct pw_core *core;
	struct pw_type *t;
	struct pw_module *module;
	struct spa_list link;

        struct spa_source *timer;

	struct pw_properties *properties;

	struct spa_list socket_list;
	struct spa_list client_list;
	struct spa_list link_list;

	struct spa_loop_control_hooks hooks;

	struct jack_server server;

	struct pw_link *sink_link;

	struct {
		struct spa_list nodes;
	} rt;
};

struct client {
	struct impl *impl;

	struct spa_list link;

	struct pw_client *client;
	struct spa_hook client_listener;

	int fd;
	struct spa_source *source;

	struct spa_list jack_clients;
};

struct port {
	struct impl *impl;
	struct pw_jack_port *port;
	struct spa_hook port_listener;
	struct jack_client *jc;
};

struct link {
	struct impl *impl;
	struct pw_link *link;
	struct pw_jack_port *out_port;
	struct pw_jack_port *in_port;
	struct spa_hook link_listener;
	struct spa_list link_link;
};

static bool init_socket_name(struct sockaddr_un *addr, const char *name, bool promiscuous, int which)
{
	int name_size;
	const char *runtime_dir;

	runtime_dir = JACK_SOCKET_DIR;

	addr->sun_family = AF_UNIX;
	if (promiscuous) {
		name_size = snprintf(addr->sun_path, sizeof(addr->sun_path),
			     "%s/jack_%s_%d", runtime_dir, name, which) + 1;
	} else {
		name_size = snprintf(addr->sun_path, sizeof(addr->sun_path),
			     "%s/jack_%s_%d_%d", runtime_dir, name, getuid(), which) + 1;
	}

	if (name_size > (int) sizeof(addr->sun_path)) {
		pw_log_error("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
			     runtime_dir, name);
		*addr->sun_path = 0;
		return false;
	}
	return true;
}

static int
notify_client(struct jack_client *client, int ref_num, const char *name, int notify,
	      int sync, const char* message, int value1, int value2)
{
	int size, result = 0;
	char _name[JACK_CLIENT_NAME_SIZE+1];
	char _message[JACK_MESSAGE_SIZE+1];

	if (client->fd == -1)
		return 0;

	if (!client->node->control->callback[notify])
		return 0;

	if (name == NULL)
		name = client->node->control->name;

	snprintf(_name, sizeof(_name), "%s", name);
        snprintf(_message, sizeof(_message), "%s", message);

	size = sizeof(int) + sizeof(_name) + 5 * sizeof(int) + sizeof(_message);
	CheckWrite(&size, sizeof(int));
        CheckWrite(_name, sizeof(_name));
	CheckWrite(&ref_num, sizeof(int));
	CheckWrite(&notify, sizeof(int));
	CheckWrite(&value1, sizeof(int));
	CheckWrite(&value2, sizeof(int));
	CheckWrite(&sync, sizeof(int));
        CheckWrite(_message, sizeof(_message));

	if (sync)
		CheckRead(&result, sizeof(int));

	return result;
}

static int
notify_add_client(struct impl *impl, struct jack_client *client, const char *name, int ref_num)
{
	struct jack_server *server = &impl->server;
	int i;

	for (i = 0; i < CLIENT_NUM; i++) {
		struct jack_client *c = server->client_table[i];
		const char *n;

		if (c == NULL || c == client)
			continue;

		n = c->node->control->name;
		if (notify_client(c, ref_num, name, jack_notify_AddClient, false, "", 0, 0) < 0)
			pw_log_warn("module-jack %p: can't notify add client", impl);

		if (notify_client(client, i, n, jack_notify_AddClient, true, "", 0, 0) < 0) {
			pw_log_error("module-jack %p: can't notify add client", impl);
			return -1;
		}
	}
	return 0;
}
static int
notify_remove_client(struct impl *impl, const char *name, int ref_num)
{
	struct jack_server *server = &impl->server;
	int i;

	for (i = 0; i < CLIENT_NUM; i++) {
		struct jack_client *c = server->client_table[i];
		if (c == NULL)
			continue;
                if (notify_client(c, ref_num, name, jack_notify_RemoveClient, false, "", 0, 0) < 0)
			pw_log_warn("module-jack %p: can't notify remove client", impl);
	}
	return 0;
}

void
notify_clients(struct impl *impl, int notify,
	       int sync, const char* message, int value1, int value2)
{
	struct jack_server *server = &impl->server;
	int i;
	for (i = 0; i < CLIENT_NUM; i++) {
		struct jack_client *c = server->client_table[i];
		if (c == NULL)
			continue;
		notify_client(c, i, NULL, notify, sync, message, value1, value2);
	}
}

static void port_free(void *data)
{
	struct pw_jack_port *port = data;
	struct port *p = port->user_data;
	struct impl *impl = p->impl;
	struct jack_client *jc = p->jc;
	pw_log_debug("port %p: free", port);

	if (jc->node->control->active)
		notify_clients(impl, jack_notify_PortRegistrationOffCallback, false, "", port->port_id, 0);
}

static const struct pw_jack_port_events port_listener = {
	PW_VERSION_JACK_PORT_EVENTS,
	.free = port_free,
};

static int process_messages(struct client *client);

static int
handle_register_port(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	int result = -1;
	int ref_num;
	char name[JACK_PORT_NAME_SIZE + 1];
	char port_type[JACK_PORT_TYPE_SIZE + 1];
	unsigned int flags;
	unsigned int buffer_size;
	static jack_port_id_t port_index = 0;
	struct jack_client *jc;
	struct pw_jack_port *port;
	struct port *p;

	CheckSize(kRegisterPort_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(name, sizeof(name));
	CheckRead(port_type, sizeof(port_type));
	CheckRead(&flags, sizeof(unsigned int));
	CheckRead(&buffer_size, sizeof(unsigned int));

	pw_log_debug("protocol-jack %p: kRegisterPort %d %s %s %u %u", impl,
			ref_num, name, port_type, flags, buffer_size);

	jc = server->client_table[ref_num];

	port = pw_jack_node_add_port(jc->node, name, port_type, flags, sizeof(struct port));
	if (port == NULL) {
		pw_log_error("module-jack %p: can't add port", impl);
		goto reply;
	}
	p = port->user_data;
	p->impl = impl;
	p->jc = jc;
	port_index = port->port_id;

	pw_jack_port_add_listener(port, &p->port_listener, &port_listener, port);

	if (jc->node->control->active)
		notify_clients(impl, jack_notify_PortRegistrationOnCallback, false, "", port_index, 0);

	result = 0;

      reply:
	CheckWrite(&result, sizeof(int));
	CheckWrite(&port_index, sizeof(jack_port_id_t));
	return 0;
}

static int do_add_node(struct spa_loop *loop, bool async, uint32_t seq, size_t size, const void *data,
		     void *user_data)
{
	struct jack_client *jc = user_data;
	struct impl *impl = jc->data;
	spa_list_append(&impl->rt.nodes, &jc->node->graph_link);
	return SPA_RESULT_OK;
}

static int do_remove_node(struct spa_loop *loop, bool async, uint32_t seq, size_t size, const void *data,
		     void *user_data)
{
	struct jack_client *jc = user_data;
	spa_list_remove(&jc->node->graph_link);
	return SPA_RESULT_OK;
}

static int
handle_activate_client(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	struct jack_client *jc;
	int result = 0, ref_num, i;
	int is_real_time;
	jack_int_t input_ports[PORT_NUM_FOR_CLIENT];
        jack_int_t output_ports[PORT_NUM_FOR_CLIENT];

	CheckSize(kActivateClient_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(&is_real_time, sizeof(int));

	pw_log_debug("protocol-jack %p: kActivateClient %d %d", client->impl,
			ref_num, is_real_time);

	conn = jack_graph_manager_next_start(mgr);

	if (is_real_time) {
		jack_connection_manager_direct_connect(conn, server->freewheel_ref_num, ref_num);
		jack_connection_manager_direct_connect(conn, ref_num, server->freewheel_ref_num);
	}

	memcpy (input_ports, jack_connection_manager_get_inputs(conn, ref_num), sizeof(input_ports));
	memcpy (output_ports, jack_connection_manager_get_outputs(conn, ref_num), sizeof(input_ports));

	jack_graph_manager_next_stop(mgr);

	jc = server->client_table[ref_num];
	if (jc) {
		notify_client(jc, ref_num, NULL, jack_notify_ActivateClient, true, "", 0, 0);
		jc->activated = true;
		jc->realtime = is_real_time;
		if (is_real_time)
			pw_loop_invoke(jc->node->node->data_loop,
				do_add_node, 0, 0, NULL, false, jc);
	}

	for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (input_ports[i] != EMPTY); i++)
		notify_clients(impl, jack_notify_PortRegistrationOnCallback, false, "", input_ports[i], 0);
	for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (output_ports[i] != EMPTY); i++)
		notify_clients(impl, jack_notify_PortRegistrationOnCallback, false, "", output_ports[i], 0);

	CheckWrite(&result, sizeof(int));
	return 0;
}

static int client_deactivate(struct impl *impl, int ref_num)
{
	struct jack_server *server = &impl->server;
	int fw_ref = server->freewheel_ref_num, i, j;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	struct jack_client *jc;
	const jack_int_t *inputs, *outputs;
	jack_int_t conns[CONNECTION_NUM_FOR_PORT];

	jc = server->client_table[ref_num];
	if (jc) {
		jc->activated = false;
		if (jc->realtime)
			pw_loop_invoke(jc->node->node->data_loop,
					do_remove_node, 0, 0, NULL, false, jc);
	}

	conn = jack_graph_manager_next_start(mgr);

	if (jack_connection_manager_is_direct_connection(conn, fw_ref, ref_num))
		jack_connection_manager_direct_disconnect(conn, fw_ref, ref_num);

	if (jack_connection_manager_is_direct_connection(conn, ref_num, fw_ref))
		jack_connection_manager_direct_disconnect(conn, ref_num, fw_ref);

	inputs = jack_connection_manager_get_inputs(conn, ref_num);
	outputs = jack_connection_manager_get_outputs(conn, ref_num);

	for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (inputs[i] != EMPTY); i++) {
		memcpy(conns, jack_connection_manager_get_connections(conn, inputs[i]), sizeof(conns));
		for (j = 0; (j < CONNECTION_NUM_FOR_PORT) && (conns[j] != EMPTY); j++) {
			jack_connection_manager_disconnect_ports(conn, conns[j], inputs[i]);
			notify_clients(impl, jack_notify_PortDisconnectCallback,
					false, "", conns[j], inputs[i]);
		}
	}
	for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (outputs[i] != EMPTY); i++) {
		memcpy(conns, jack_connection_manager_get_connections(conn, outputs[i]), sizeof(conns));
		for (j = 0; (j < CONNECTION_NUM_FOR_PORT) && (conns[j] != EMPTY); j++) {
			jack_connection_manager_disconnect_ports(conn, outputs[i], conns[j]);
			notify_clients(impl, jack_notify_PortDisconnectCallback,
					false, "", outputs[i], conns[j]);
		}
	}

	for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (inputs[i] != EMPTY); i++)
		notify_clients(impl, jack_notify_PortRegistrationOffCallback, false, "", inputs[i], 0);
	for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (outputs[i] != EMPTY); i++)
		notify_clients(impl, jack_notify_PortRegistrationOffCallback, false, "", outputs[i], 0);

	jack_graph_manager_next_stop(mgr);

	return 0;
}

static int
handle_deactivate_client(struct client *client)
{
	struct impl *impl = client->impl;
	int result = 0;
	int ref_num;

	CheckSize(kDeactivateClient_size);
	CheckRead(&ref_num, sizeof(int));

	pw_log_debug("protocol-jack %p: kDeactivateClient %d", client->impl,
			ref_num);

	result = client_deactivate(impl, ref_num);

	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_client_check(struct client *client)
{
	char name[JACK_CLIENT_NAME_SIZE+1];
	int protocol;
	int options;
	int UUID;
	int open;
	int result = 0;
	int status;

	CheckSize(kClientCheck_size);
	CheckRead(name, sizeof(name));
	CheckRead(&protocol, sizeof(int));
	CheckRead(&options, sizeof(int));
	CheckRead(&UUID, sizeof(int));
	CheckRead(&open, sizeof(int));

	pw_log_debug("protocol-jack %p: kClientCheck %s %d %d %d %d", client->impl,
			name, protocol, options, UUID, open);

	status = 0;
	if (protocol != JACK_PROTOCOL_VERSION) {
		status |= (JackFailure | JackVersionError);
		pw_log_error("protocol-jack: protocol mismatch (%d vs %d)", protocol, JACK_PROTOCOL_VERSION);
		result = -1;
		goto reply;
	}
	/* TODO check client name and uuid */

      reply:
	CheckWrite(&result, sizeof(int));
	CheckWrite(name, sizeof(name));
	CheckWrite(&status, sizeof(int));

	if (open)
		return process_messages(client);

	return 0;
}

static void node_destroy(void *data)
{
	struct jack_client *jc = data;
	struct impl *impl = jc->data;
	struct jack_server *server = &impl->server;
	int ref_num = jc->node->control->ref_num;

	pw_log_debug("module-jack %p: jack_client %p destroy", impl, jc);

	if (jc->activated) {
		client_deactivate(impl, ref_num);
		if (jc->realtime)
			spa_list_remove(&jc->node->graph_link);
	}
	spa_list_remove(&jc->client_link);

	jack_server_free_ref_num(server, ref_num);
}

static void node_free(void *data)
{
	struct jack_client *jc = data;
	struct impl *impl = jc->data;
	struct jack_server *server = &impl->server;
	int ref_num = jc->node->control->ref_num;

	notify_remove_client(impl, jc->node->control->name, ref_num);

	jack_synchro_close(&server->synchro_table[ref_num]);
	if (jc->fd != -1)
		close(jc->fd);
}

static const struct pw_jack_node_events node_events = {
	PW_VERSION_JACK_NODE_EVENTS,
	.destroy = node_destroy,
	.free = node_free,
};

static int
handle_client_open(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	int PID, UUID;
	char name[JACK_CLIENT_NAME_SIZE+1];
	int result = -1, ref_num, shared_engine, shared_client, shared_graph;
	struct jack_client *jc;
	const struct ucred *ucred;
	struct sockaddr_un addr;
	struct pw_jack_node *node;
	struct pw_properties *properties;

	CheckSize(kClientOpen_size);
	CheckRead(&PID, sizeof(int));
	CheckRead(&UUID, sizeof(int));
	CheckRead(name, sizeof(name));

	ucred = pw_client_get_ucred(client->client);

	properties = pw_properties_new(NULL, NULL);
	pw_properties_setf(properties, "application.jack.PID", "%d", PID);
	pw_properties_setf(properties, "application.jack.UUID", "%d", UUID);

	node = pw_jack_node_new(impl->core,
				pw_client_get_global(client->client),
				server,
				name,
				ucred ? ucred->pid : PID,
				properties,
				sizeof(struct jack_client));
	if (node == NULL) {
		pw_log_error("module-jack %p: can't create node", impl);
		goto reply;
	}

	ref_num = node->control->ref_num;

	jc = node->user_data;
	jc->fd = -1;
	jc->data = impl;
	jc->owner = client;
	jc->node = node;

	pw_jack_node_add_listener(node, &jc->node_listener, &node_events, jc);

	if ((jc->fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		pw_log_error("module-jack %p: can't create socket %s", impl, strerror(errno));
		goto reply;
	}

	if (!init_socket_name(&addr, name, server->promiscuous, 0))
		goto reply;

	if (connect(jc->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		pw_log_error("module-jack %p: can't connect socket %s", impl, strerror(errno));
		goto reply;
	}

	server->client_table[ref_num] = jc;
	pw_log_debug("module-jack %p: Added client %d \"%s\"", impl, ref_num, name);

	spa_list_append(&client->jack_clients, &jc->client_link);

	if (notify_add_client(impl, jc, name, ref_num) < 0) {
		pw_log_error("module-jack %p: can't notify add_client", impl);
		goto reply;
	}

	shared_engine = impl->server.engine_control->info.index;
	shared_client = jc->node->control->info.index;
	shared_graph = impl->server.graph_manager->info.index;

	result = 0;

      reply:
	CheckWrite(&result, sizeof(int));
	CheckWrite(&shared_engine, sizeof(int));
	CheckWrite(&shared_client, sizeof(int));
	CheckWrite(&shared_graph, sizeof(int));

	return 0;
}

static int
handle_client_close(struct client *client)
{
	int ref_num;
	CheckSize(kClientClose_size);
	CheckRead(&ref_num, sizeof(int));
	int result = 0;



	CheckWrite(&result, sizeof(int));
	return 0;
}

static void link_destroy(void *data)
{
	struct link *ld = data;
	struct impl *impl = ld->impl;
	struct pw_jack_port *out_port = ld->out_port, *in_port = ld->in_port;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	jack_port_id_t src_id = out_port->port_id, dst_id = in_port->port_id;

	pw_log_debug("module-jack %p: link %p destroy", impl, ld->link);

	spa_list_remove(&ld->link_link);

	conn = jack_graph_manager_next_start(mgr);
	if (jack_connection_manager_is_connected(conn, src_id, dst_id)) {
		if (jack_connection_manager_disconnect_ports(conn, src_id, dst_id)) {
			pw_log_warn("module-jack %p: ports can't disconnect", impl);
		}
		notify_clients(impl, jack_notify_PortDisconnectCallback, false, "", src_id, dst_id);
	}
	jack_graph_manager_next_stop(mgr);

}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.destroy = link_destroy,
};

static int
handle_connect_name_ports(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	struct jack_client *jc;
	int ref_num;
	char src[REAL_JACK_PORT_NAME_SIZE+1];
	char dst[REAL_JACK_PORT_NAME_SIZE+1];
	int result = -1, in_ref, out_ref;
	jack_port_id_t src_id, dst_id;
	struct jack_port *src_port, *dst_port;
	struct pw_jack_port *out_port, *in_port;
	struct pw_link *link;
	struct link *ld;

	CheckSize(kConnectNamePorts_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(src, sizeof(src));
	CheckRead(dst, sizeof(dst));

	src_id = jack_graph_manager_find_port(mgr, src);
	if (src_id == NO_PORT) {
		pw_log_error("protocol-jack %p: port_name %s does not exist", impl, src);
		goto reply;
	}
	dst_id = jack_graph_manager_find_port(mgr, dst);
	if (dst_id == NO_PORT) {
		pw_log_error("protocol-jack %p: port_name %s does not exist", impl, dst);
		goto reply;
	}

	pw_log_debug("protocol-jack %p: kConnectNamePort %d %s %s %u %u", impl,
			ref_num, src, dst, src_id, dst_id);

	src_port = jack_graph_manager_get_port(mgr, src_id);
	dst_port = jack_graph_manager_get_port(mgr, dst_id);

	if (((src_port->flags & JackPortIsOutput) == 0) ||
	    ((dst_port->flags & JackPortIsInput) == 0)) {
		pw_log_error("protocol-jack %p: ports are not input and output", impl);
		goto reply;
	}

	if (!src_port->in_use || !dst_port->in_use) {
		pw_log_error("protocol-jack %p: ports are not in use", impl);
		goto reply;
	}
	if (src_port->type_id != dst_port->type_id) {
		pw_log_error("protocol-jack %p: ports are not of the same type", impl);
		goto reply;
	}

	conn = jack_graph_manager_next_start(mgr);

	out_ref = jack_connection_manager_get_output_refnum(conn, src_id);
	if (out_ref == -1) {
		pw_log_error("protocol-jack %p: unknown port_id %d", impl, src_id);
		goto reply_stop;
	}
	if ((jc = server->client_table[out_ref]) == NULL) {
		pw_log_error("protocol-jack %p: unknown client %d", impl, out_ref);
		goto reply_stop;
	}
	if (!jc->node->control->active) {
		pw_log_error("protocol-jack %p: can't connect ports of inactive client", impl);
		goto reply_stop;
	}
	out_port = pw_jack_node_find_port(jc->node, PW_DIRECTION_OUTPUT, src_id);

	in_ref = jack_connection_manager_get_input_refnum(conn, dst_id);
	if (in_ref == -1) {
		pw_log_error("protocol-jack %p: unknown port_id %d", impl, dst_id);
		goto reply_stop;
	}
	if ((jc = server->client_table[in_ref]) == NULL) {
		pw_log_error("protocol-jack %p: unknown client %d", impl, in_ref);
		goto reply_stop;
	}
	if (!jc->node->control->active) {
		pw_log_error("protocol-jack %p: can't connect ports of inactive client", impl);
		goto reply_stop;
	}
	in_port = pw_jack_node_find_port(jc->node, PW_DIRECTION_INPUT, dst_id);

	if (jack_connection_manager_connect_ports(conn, src_id, dst_id)) {
		pw_log_error("protocol-jack %p: ports can't connect", impl);
		goto reply_stop;
	}
	pw_log_debug("protocol-jack %p: connected ports %p %p", impl, out_port, in_port);

	link = pw_link_new(impl->core,
			   pw_module_get_global(impl->module),
			   out_port->port,
			   in_port->port,
			   NULL,
			   NULL,
			   NULL,
			   sizeof(struct link));
	ld = pw_link_get_user_data(link);
	ld->impl = impl;
	ld->link = link;
	ld->out_port = out_port;
	ld->in_port = in_port;
	spa_list_append(&impl->link_list, &ld->link_link);
	pw_link_add_listener(link, &ld->link_listener, &link_events, ld);
	pw_link_activate(link);

	notify_clients(impl, jack_notify_PortConnectCallback, false, "", src_id, dst_id);

	result = 0;
    reply_stop:
	jack_graph_manager_next_stop(mgr);

    reply:
	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_disconnect_name_ports(struct client *client)
{
	struct link *link, *t;
	struct impl *impl = client->impl;
	int result = -1;
	int ref_num;
	char src[REAL_JACK_PORT_NAME_SIZE+1];
	char dst[REAL_JACK_PORT_NAME_SIZE+1];

	CheckSize(kDisconnectNamePorts_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(src, sizeof(src));
	CheckRead(dst, sizeof(dst));

	spa_list_for_each_safe(link, t, &impl->link_list, link_link) {
		if ((strcmp(link->out_port->jack_port->name, src) == 0) &&
		    (strcmp(link->in_port->jack_port->name, dst) == 0)) {
			pw_link_destroy(link->link);
			result = 0;
			break;
		}
	}

	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_get_UUID_by_client(struct client *client)
{
	char name[JACK_CLIENT_NAME_SIZE+1];
	char UUID[JACK_UUID_SIZE];
	int result = 0;

	CheckSize(kGetUUIDByClient_size);
	CheckRead(name, sizeof(name));

	CheckWrite(&result, sizeof(int));
	CheckWrite(UUID, sizeof(UUID));

	return 0;
}

static int
process_messages(struct client *client)
{
	struct pw_client *c = client->client;
	int type, res = -1;

	if (read(client->fd, &type, sizeof(enum jack_request_type)) != sizeof(enum jack_request_type)) {
		pw_log_error("protocol-jack %p: failed to read type", client->impl);
		goto error;
	}
	pw_log_info("protocol-jack %p: got type %d", client->impl, type);

	switch(type) {
	case jack_request_RegisterPort:
		res = handle_register_port(client);
		break;
	case jack_request_UnRegisterPort:
		break;
	case jack_request_ConnectPorts:
		break;
	case jack_request_DisconnectPorts:
		break;
	case jack_request_SetTimeBaseClient:
		break;
	case jack_request_ActivateClient:
		res = handle_activate_client(client);
		break;
	case jack_request_DeactivateClient:
		res = handle_deactivate_client(client);
		break;
	case jack_request_DisconnectPort:
		break;
	case jack_request_SetClientCapabilities:
	case jack_request_GetPortConnections:
	case jack_request_GetPortNConnections:
	case jack_request_ReleaseTimebase:
	case jack_request_SetTimebaseCallback:
	case jack_request_SetBufferSize:
	case jack_request_SetFreeWheel:
		break;
	case jack_request_ClientCheck:
		res = handle_client_check(client);
		break;
	case jack_request_ClientOpen:
		res = handle_client_open(client);
		break;
	case jack_request_ClientClose:
		res = handle_client_close(client);
		break;
	case jack_request_ConnectNamePorts:
		res = handle_connect_name_ports(client);
		break;
	case jack_request_DisconnectNamePorts:
		res = handle_disconnect_name_ports(client);
		break;
	case jack_request_GetInternalClientName:
	case jack_request_InternalClientHandle:
	case jack_request_InternalClientLoad:
	case jack_request_InternalClientUnload:
	case jack_request_PortRename:
	case jack_request_Notification:
	case jack_request_SessionNotify:
	case jack_request_SessionReply:
	case jack_request_GetClientByUUID:
	case jack_request_ReserveClientName:
		break;
	case jack_request_GetUUIDByClient:
		res = handle_get_UUID_by_client(client);
		break;
	case jack_request_ClientHasSessionCallback:
	case jack_request_ComputeTotalLatencies:
		break;
	default:
		pw_log_error("protocol-jack %p: invalid type %d", client->impl, type);
		goto error;
	}
	if (res != 0)
		goto error;

	return res;

      error:
	pw_log_error("protocol-jack %p: error handling type %d", client->impl, type);
	pw_client_destroy(c);
	return -1;

}

static void
client_busy_changed(void *data, bool busy)
{
	struct client *c = data;
	enum spa_io mask = SPA_IO_ERR | SPA_IO_HUP;

	if (!busy)
		mask |= SPA_IO_IN;

	pw_loop_update_io(pw_core_get_main_loop(c->impl->core), c->source, mask);

	if (!busy)
		process_messages(c);
}

static void client_killed(struct client *client)
{
	struct jack_client *jc;

	spa_list_for_each(jc, &client->jack_clients, client_link) {
		close(jc->fd);
		jc->fd = -1;
	}
	pw_client_destroy(client->client);
}

static void
connection_data(void *data, int fd, enum spa_io mask)
{
	struct client *client = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", client->impl);
		client_killed(client);
		return;
	}

	if (mask & SPA_IO_IN)
		process_messages(client);
}

static void client_destroy(void *data)
{
	struct client *this = data;
	struct jack_client *jc, *t;

	pw_loop_destroy_source(pw_core_get_main_loop(this->impl->core), this->source);
	spa_list_remove(&this->link);

	spa_list_for_each_safe(jc, t, &this->jack_clients, client_link)
		pw_jack_node_destroy(jc->node);

	close(this->fd);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.destroy = client_destroy,
	.busy_changed = client_busy_changed,
};

static struct client *client_new(struct impl *impl, int fd)
{
	struct client *this;
	struct pw_client *client;
	socklen_t len;
	struct ucred ucred, *ucredp;
	struct pw_properties *properties;

	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_error("no peercred: %m");
		ucredp = NULL;
	} else {
		ucredp = &ucred;
	}

	properties = pw_properties_new("pipewire.protocol", "protocol-jack", NULL);
	if (properties == NULL)
		goto no_props;

	if (ucredp) {
		pw_properties_setf(properties, "application.process.id", "%d", ucredp->pid);
		pw_properties_setf(properties, "application.process.userid", "%d", ucredp->uid);
	}

        client = pw_client_new(impl->core, pw_module_get_global(impl->module),
			       ucredp, properties, sizeof(struct client));
	if (client == NULL)
		goto no_client;

	this = pw_client_get_user_data(client);
	this->client = client;
	this->impl = impl;
	this->fd = fd;
	this->source = pw_loop_add_io(pw_core_get_main_loop(impl->core),
				      this->fd,
				      SPA_IO_ERR | SPA_IO_HUP, false, connection_data, this);
	if (this->source == NULL)
		goto no_source;

	spa_list_init(&this->jack_clients);
	spa_list_insert(impl->client_list.prev, &this->link);

	pw_client_add_listener(client, &this->client_listener, &client_events, this);

	pw_log_debug("module-jack %p: added new client", impl);

	return this;

      no_source:
	free(this);
      no_props:
      no_client:
	return NULL;
}

static int do_graph_order_changed(struct spa_loop *loop,
		     bool async,
		     uint32_t seq,
		     size_t size,
		     const void *data,
		     void *user_data)
{
	struct impl *impl = user_data;
	struct jack_server *server = &impl->server;
	int i;

	for (i = 0; i < CLIENT_NUM; i++) {
		struct jack_client *c = server->client_table[i];
		if (c == NULL)
			continue;
                if (notify_client(c, i, NULL, jack_notify_LatencyCallback, true, "", 0, 0) < 0)
			pw_log_warn("module-jack %p: can't notify capture latency", impl);
	}
	for (i = 0; i < CLIENT_NUM; i++) {
		struct jack_client *c = server->client_table[i];
		if (c == NULL)
			continue;
                if (notify_client(c, i, NULL, jack_notify_LatencyCallback, true, "", 1, 0) < 0)
			pw_log_warn("module-jack %p: can't notify playback latency", impl);
	}

	notify_clients(impl, jack_notify_GraphOrderCallback, false, "", 0, 0);
	return SPA_RESULT_OK;
}

static void jack_node_pull(void *data)
{
	struct jack_client *jc = data;
	struct impl *impl = jc->data;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct spa_graph_node *n = &jc->node->node->rt.node, *pn;
	struct spa_graph_port *p, *pp;
	bool res;

	jack_graph_manager_try_switch(mgr, &res);
	if (res) {
		pw_loop_invoke(pw_core_get_main_loop(impl->core),
                       do_graph_order_changed, 0, 0, NULL, false, impl);
	}

	/* mix all input */
	spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
		if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
			continue;
		pn->state = pn->callbacks->process_input(pn->callbacks_data);
	}
}

static void jack_node_push(void *data)
{
	struct jack_client *jc = data;
	struct impl *impl = jc->data;
	struct jack_server *server = &impl->server;
	struct jack_graph_manager *mgr = server->graph_manager;
	struct jack_connection_manager *conn;
	int activation;
	struct pw_jack_node *node;
	struct spa_graph_node *n = &jc->node->node->rt.node, *pn;
	struct spa_graph_port *p, *pp;

	conn = jack_graph_manager_get_current(mgr);

	activation = jack_connection_manager_get_activation(conn, server->freewheel_ref_num);
	if (activation != 0)
		pw_log_warn("resume %d, some client did not complete", activation);

	jack_connection_manager_reset(conn, mgr->client_timing);

        jack_activation_count_signal(&conn->input_counter[server->freewheel_ref_num],
                                    &server->synchro_table[server->freewheel_ref_num]);

	spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
		if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
			continue;
		pn->state = pn->callbacks->process_output(pn->callbacks_data);
	}

	spa_list_for_each(node, &impl->rt.nodes, graph_link) {
		n = &node->node->rt.node;

		spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
			if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
				continue;
			pn->state = pn->callbacks->process_output(pn->callbacks_data);
		}
		n->state = n->callbacks->process_output(n->callbacks_data);

		/* mix inputs */
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
			if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
				continue;
			pn->state = pn->callbacks->process_output(pn->callbacks_data);
			pn->state = pn->callbacks->process_input(pn->callbacks_data);
		}

		n->state = n->callbacks->process_input(n->callbacks_data);

		/* tee outputs */
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
			if ((pp = p->peer) == NULL || ((pn = pp->node) == NULL))
				continue;
			pn->state = pn->callbacks->process_input(pn->callbacks_data);
		}
	}


#if 0
	jack_connection_manager_resume_ref_num(conn,
					       client->control,
					       server->synchro_table,
					       mgr->client_timing);

	if (server->engine_control->sync_mode) {
		pw_log_trace("suspend");
		jack_connection_manager_suspend_ref_num(conn,
						        client->control,
						        server->synchro_table,
						        mgr->client_timing);
	}
#endif
}

static const struct pw_jack_node_events jack_node_events = {
	PW_VERSION_JACK_NODE_EVENTS,
	.pull = jack_node_pull,
	.push = jack_node_push,
};

static int
make_audio_client(struct impl *impl)
{
	struct jack_server *server = &impl->server;
	int ref_num;
	struct jack_client *jc;
	struct pw_jack_node *node;

	node = pw_jack_driver_new(impl->core,
				  pw_module_get_global(impl->module),
				  server,
				  "system",
				  0, 2,
				  NULL,
				  sizeof(struct jack_client));
	if (node == NULL) {
		pw_log_error("module-jack %p: can't create driver node", impl);
		return -1;
	}

	ref_num = node->control->ref_num;

	jc = node->user_data;
	jc->fd = -1;
	jc->data = impl;
	jc->node = node;
	pw_jack_node_add_listener(node, &jc->node_listener, &jack_node_events, jc);

	server->client_table[ref_num] = jc;

	server->audio_ref_num = ref_num;
	server->audio_node = node;

	pw_log_debug("module-jack %p: Added audio driver %d", impl, ref_num);

	return 0;
}

static int
make_freewheel_client(struct impl *impl)
{
	struct jack_server *server = &impl->server;
	int ref_num;
	struct jack_client *jc;
	struct pw_jack_node *node;

	node = pw_jack_driver_new(impl->core,
				  pw_module_get_global(impl->module),
				  server,
				  "freewheel",
				  0, 0,
				  NULL,
				  sizeof(struct jack_client));
	if (node == NULL) {
		pw_log_error("module-jack %p: can't create driver node", impl);
		return -1;
	}

	ref_num = node->control->ref_num;

	jc = node->user_data;
	jc->fd = -1;
	jc->data = impl;
	jc->node = node;

	server->client_table[ref_num] = jc;

	server->freewheel_ref_num = ref_num;
	pw_log_debug("module-jack %p: Added freewheel driver %d", impl, ref_num);

	return 0;
}

static bool on_global(void *data, struct pw_global *global)

{
	struct impl *impl = data;
	struct pw_node *node;
	const struct pw_properties *properties;
	const char *str;
	struct pw_port *in_port, *out_port;

	if (pw_global_get_type(global) != impl->t->node)
		return true;

	node = pw_global_get_object(global);

	properties = pw_node_get_properties(node);
	if ((str = pw_properties_get(properties, "media.class")) == NULL)
		return true;

	if (strcmp(str, "Audio/Sink") != 0)
		return true;

	out_port = pw_node_get_free_port(impl->server.audio_node->node, PW_DIRECTION_OUTPUT);
	in_port = pw_node_get_free_port(node, PW_DIRECTION_INPUT);
	if (out_port == NULL || in_port == NULL)
		return true;

	impl->sink_link = pw_link_new(impl->core, pw_module_get_global(impl->module),
		    out_port,
		    in_port,
		    NULL,
		    NULL,
		    NULL,
		    0);
	pw_link_inc_idle(impl->sink_link);

	return false;
}

static bool init_nodes(struct impl *impl)
{
	struct pw_core *core = impl->core;

	make_audio_client(impl);
	make_freewheel_client(impl);

	pw_core_for_each_global(core, on_global, impl);

	return true;
}

static struct socket *create_socket(void)
{
	struct socket *s;

	if ((s = calloc(1, sizeof(struct socket))) == NULL)
		return NULL;

	s->fd = -1;
	return s;
}

static void destroy_socket(struct socket *s)
{
	if (s->source)
		pw_loop_destroy_source(s->loop, s->source);
	if (s->addr.sun_path[0])
		unlink(s->addr.sun_path);
	if (s->fd >= 0)
		close(s->fd);
	if (s->lock_addr[0])
		unlink(s->lock_addr);
	free(s);
}

static void
socket_data(void *data, int fd, enum spa_io mask)
{
	struct impl *impl = data;
	struct client *client;
	struct sockaddr_un name;
	socklen_t length;
	int client_fd;

	length = sizeof(name);
	client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
	if (client_fd < 0) {
		pw_log_error("failed to accept: %m");
		return;
	}

	client = client_new(impl, client_fd);
	if (client == NULL) {
		pw_log_error("failed to create client");
		close(client_fd);
		return;
	}

	pw_loop_update_io(pw_core_get_main_loop(impl->core),
			  client->source, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
}

static bool add_socket(struct impl *impl, struct socket *s)
{
	socklen_t size;

	if ((s->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
		return false;

	size = offsetof(struct sockaddr_un, sun_path) + strlen(s->addr.sun_path);
	if (bind(s->fd, (struct sockaddr *) &s->addr, size) < 0) {
		pw_log_error("bind() failed with error: %m");
		return false;
	}

	if (listen(s->fd, 100) < 0) {
		pw_log_error("listen() failed with error: %m");
		return false;
	}

	s->loop = pw_core_get_main_loop(impl->core);
	s->source = pw_loop_add_io(s->loop, s->fd, SPA_IO_IN, false, socket_data, impl);
	if (s->source == NULL)
		return false;

	spa_list_insert(impl->socket_list.prev, &s->link);

	return true;
}

static int init_server(struct impl *impl, const char *name, bool promiscuous)
{
	struct jack_server *server = &impl->server;
	int i;
	struct socket *s;

	pthread_mutex_init(&server->lock, NULL);

	if (jack_register_server(name, 1) != 0)
		return -1;

	jack_cleanup_shm();

	server->promiscuous = promiscuous;

	/* graph manager */
	server->graph_manager = jack_graph_manager_alloc(2048);

	/* engine control */
	server->engine_control = jack_engine_control_alloc(name);

	for (i = 0; i < CLIENT_NUM; i++)
		server->synchro_table[i] = JACK_SYNCHRO_INIT;

	if (!init_nodes(impl))
		return -1;

	s = create_socket();

	if (!init_socket_name(&s->addr, name, promiscuous, 0))
		goto error;

	if (!add_socket(impl, s))
		goto error;

	return 0;

      error:
	destroy_socket(s);
	return -1;
}


static bool module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;
	const char *name, *str;
	bool promiscuous;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("protocol-jack %p: new", impl);

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->module = module;
	impl->properties = properties;

	spa_list_init(&impl->socket_list);
	spa_list_init(&impl->client_list);
	spa_list_init(&impl->link_list);
	spa_list_init(&impl->rt.nodes);

	str = NULL;
	if (impl->properties)
		str = pw_properties_get(impl->properties, "jack.default.server");
	if (str == NULL)
		str = getenv("JACK_DEFAULT_SERVER");

	name = str ? str : JACK_DEFAULT_SERVER_NAME;

	str = NULL;
	if (impl->properties)
		str = pw_properties_get(impl->properties, "jack.promiscuous.server");
	if (str == NULL)
		str = getenv("JACK_PROMISCUOUS_SERVER");

	promiscuous = str ? atoi(str) != 0 : false;

	if (init_server(impl, name, promiscuous) < 0)
		goto error;

	return true;

      error:
	free(impl);
	return false;
}

#if 0
static void module_destroy(struct impl *impl)
{
	struct impl *object, *tmp;

	pw_log_debug("module %p: destroy", impl);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
