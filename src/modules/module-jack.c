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
#include <semaphore.h>

#include <jack/jack.h>
#include <jack/session.h>

#include "config.h"

#include "pipewire/pipewire.h"
#include "pipewire/log.h"
#include "pipewire/interfaces.h"

#include "pipewire/core.h"
#include "pipewire/node.h"
#include "pipewire/module.h"
#include "pipewire/client.h"
#include "pipewire/resource.h"
#include "pipewire/link.h"
#include "pipewire/node-factory.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"

#include "modules/module-jack/defs.h"
#include "modules/module-jack/shm.h"
#include "modules/module-jack/shared.h"
#include "modules/module-jack/synchro.h"
#include "modules/module-jack/server.h"

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
	char *core_name;
	struct spa_list link;
};

struct impl {
	struct pw_core *core;
	struct spa_list link;

	struct pw_properties *properties;

	struct spa_list socket_list;
	struct spa_list client_list;

	struct spa_loop_control_hooks hooks;

	struct jack_server server;
};

struct client {
	struct impl *impl;
	struct spa_list link;
	struct pw_client *client;
	int fd;
	struct spa_source *source;
	struct pw_listener busy_changed;
};

static int process_messages(struct client *client);

static void client_destroy(void *data)
{
	struct pw_client *client = data;
	struct client *this = client->user_data;

	pw_loop_destroy_source(this->impl->core->main_loop, this->source);
	spa_list_remove(&this->link);

	close(this->fd);
}

static int
handle_register_port(struct client *client)
{
	int result = 0;
	int ref_num;
	char name[JACK_PORT_NAME_SIZE + 1];
	char port_type[JACK_PORT_TYPE_SIZE + 1];
	unsigned int flags;
	unsigned int buffer_size;
	static jack_port_id_t port_index = 0;

	CheckSize(kRegisterPort_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(name, sizeof(name));
	CheckRead(port_type, sizeof(port_type));
	CheckRead(&flags, sizeof(unsigned int));
	CheckRead(&buffer_size, sizeof(unsigned int));

	pw_log_error("protocol-jack %p: kRegisterPort %d %s %s %u %u", client->impl,
			ref_num, name, port_type, flags, buffer_size);
	port_index++;

	CheckWrite(&result, sizeof(int));
	CheckWrite(&port_index, sizeof(jack_port_id_t));
	return 0;
}

static int
handle_activate_client(struct client *client)
{
	int result = 0;
	int ref_num;
	int is_real_time;

	CheckSize(kActivateClient_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(&is_real_time, sizeof(int));

	pw_log_error("protocol-jack %p: kActivateClient %d %d", client->impl,
			ref_num, is_real_time);

	CheckWrite(&result, sizeof(int));
	return 0;
}

static int
handle_deactivate_client(struct client *client)
{
	int result = 0;
	int ref_num;

	CheckSize(kDeactivateClient_size);
	CheckRead(&ref_num, sizeof(int));

	pw_log_error("protocol-jack %p: kDeactivateClient %d", client->impl,
			ref_num);

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

	pw_log_error("protocol-jack %p: kClientCheck %s %d %d %d %d", client->impl,
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

static int
handle_client_open(struct client *client)
{
	struct impl *impl = client->impl;
	struct jack_server *server = &impl->server;
	int PID, UUID;
	char name[JACK_CLIENT_NAME_SIZE+1];
	int result, ref_num, shared_engine, shared_client, shared_graph;
	struct jack_client *jc;

	CheckSize(kClientOpen_size);
	CheckRead(&PID, sizeof(int));
	CheckRead(&UUID, sizeof(int));
	CheckRead(name, sizeof(name));

	ref_num = jack_server_allocate_ref_num(server);
	if (ref_num == -1) {
		result = -1;
		goto reply;
	}

	jc = calloc(1,sizeof(struct jack_client));
	jc->owner = client;
	jc->ref_num = ref_num;

	if (jack_synchro_init(&server->synchro_table[ref_num],
			      name,
			      server->engine_control->server_name,
			      0,
			      false,
			      server->promiscuous) < 0) {
		result = -1;
		goto reply;
	}

	jc->control = jack_client_control_alloc(name, client->client->ucred.pid, ref_num, -1);
	if (jc->control == NULL) {
		result = -1;
		goto reply;
	}

	server->client_table[ref_num] = jc;

	result = 0;
	shared_engine = impl->server.engine_control->info.index;
	shared_client = jc->control->info.index;
	shared_graph = impl->server.graph_manager->info.index;

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

static int
handle_connect_name_ports(struct client *client)
{
	int ref_num;
	char src[REAL_JACK_PORT_NAME_SIZE+1];
	char dst[REAL_JACK_PORT_NAME_SIZE+1];
	int result = 0;

	CheckSize(kConnectNamePorts_size);
	CheckRead(&ref_num, sizeof(int));
	CheckRead(src, sizeof(src));
	CheckRead(dst, sizeof(dst));

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
	case jack_request_ConnectPorts:
	case jack_request_DisconnectPorts:
	case jack_request_SetTimeBaseClient:
	case jack_request_ActivateClient:
		res = handle_activate_client(client);
		break;
	case jack_request_DeactivateClient:
		res = handle_deactivate_client(client);
		break;
	case jack_request_DisconnectPort:
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
on_busy_changed(struct pw_listener *listener,
		struct pw_client *client)
{
	struct client *c = SPA_CONTAINER_OF(listener, struct client, busy_changed);
	enum spa_io mask = SPA_IO_ERR | SPA_IO_HUP;

	if (!client->busy)
		mask |= SPA_IO_IN;

	pw_loop_update_io(c->impl->core->main_loop, c->source, mask);

	if (!client->busy)
		process_messages(c);
}

static void
connection_data(struct spa_loop_utils *utils,
		struct spa_source *source, int fd, enum spa_io mask, void *data)
{
	struct client *client = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", client->impl);
		pw_client_destroy(client->client);
		return;
	}

	if (mask & SPA_IO_IN)
		process_messages(client);
}

static struct client *client_new(struct impl *impl, int fd)
{
	struct client *this;
	struct pw_client *client;
	socklen_t len;
	struct ucred ucred, *ucredp;

	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_error("no peercred: %m");
		ucredp = NULL;
	} else {
		ucredp = &ucred;
	}

        client = pw_client_new(impl->core, ucredp, NULL, sizeof(struct client));
	if (client == NULL)
		goto no_client;

	client->destroy = client_destroy;

	this = client->user_data;
	this->impl = impl;
	this->fd = fd;
	this->source = pw_loop_add_io(impl->core->main_loop,
				      this->fd,
				      SPA_IO_ERR | SPA_IO_HUP, false, connection_data, this);
	if (this->source == NULL)
		goto no_source;

	this->client = client;

	spa_list_insert(impl->client_list.prev, &this->link);

	pw_signal_add(&client->busy_changed, &this->busy_changed, on_busy_changed);

	pw_log_error("module-jack %p: added new client", impl);

	return this;

      no_source:
	free(this);
      no_client:
	return NULL;
}

static int
make_int_client(struct impl *impl, struct pw_node *node)
{
	struct jack_server *server = &impl->server;
	struct jack_connection_manager *conn;
	int ref_num;
	struct jack_client *jc;
	jack_port_id_t port_id;

	ref_num = jack_server_allocate_ref_num(server);
	if (ref_num == -1)
		return -1;

	if (jack_synchro_init(&server->synchro_table[ref_num],
			      "system",
			      server->engine_control->server_name,
			      0,
			      false,
			      server->promiscuous) < 0) {
		return -1;
	}

	jc = calloc(1,sizeof(struct jack_client));
	jc->ref_num = ref_num;
	jc->node = node;
	jc->control = jack_client_control_alloc("system", -1, ref_num, -1);
	jc->control->active = true;

	server->client_table[ref_num] = jc;

	impl->server.engine_control->driver_num++;

	conn = jack_graph_manager_next_start(server->graph_manager);

	jack_connection_manager_init_ref_num(conn, ref_num);

	port_id = jack_graph_manager_allocate_port(server->graph_manager,
						   ref_num, "system:playback_1", 2,
						   JackPortIsInput |  JackPortIsPhysical | JackPortIsTerminal);

	jack_connection_manager_add_port(conn, true, ref_num, port_id);

	jack_graph_manager_next_stop(server->graph_manager);

	return 0;
}

static bool init_nodes(struct impl *impl)
{
	struct pw_core *core = impl->core;
	struct pw_node *n;

        spa_list_for_each(n, &core->node_list, link) {
                const char *str;

                if (n->global == NULL)
                        continue;

                if (n->properties == NULL)
                        continue;

                if ((str = pw_properties_get(n->properties, "media.class")) == NULL)
                        continue;

                if (strcmp(str, "Audio/Sink") != 0)
                        continue;

		make_int_client(impl, n);
        }
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

static bool init_socket_name(struct socket *s, const char *name, bool promiscuous, int which)
{
	int name_size;
	const char *runtime_dir;

	runtime_dir = JACK_SOCKET_DIR;

	s->addr.sun_family = AF_UNIX;
	if (promiscuous) {
		name_size = snprintf(s->addr.sun_path, sizeof(s->addr.sun_path),
			     "%s/jack_%s_%d", runtime_dir, name, which) + 1;
	} else {
		name_size = snprintf(s->addr.sun_path, sizeof(s->addr.sun_path),
			     "%s/jack_%s_%d_%d", runtime_dir, name, getuid(), which) + 1;
	}

	s->core_name = (s->addr.sun_path + name_size - 1) - strlen(name);

	if (name_size > (int) sizeof(s->addr.sun_path)) {
		pw_log_error("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
			     runtime_dir, name);
		*s->addr.sun_path = 0;
		return false;
	}
	return true;
}

static void
socket_data(struct spa_loop_utils *utils,
	    struct spa_source *source, int fd, enum spa_io mask, void *data)
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

	pw_loop_update_io(impl->core->main_loop,
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

	s->loop = impl->core->main_loop;
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

	/* graph manager */
	server->graph_manager = jack_graph_manager_alloc(2048);

	/* engine control */
	server->engine_control = jack_engine_control_alloc(name);

	for (i = 0; i < CLIENT_NUM; i++)
		server->synchro_table[i] = JACK_SYNCHRO_INIT;

	if (!init_nodes(impl))
		return -1;

	s = create_socket();

	if (!init_socket_name(s, name, promiscuous, 0))
		goto error;

	if (!add_socket(impl, s))
		goto error;

	return 0;

      error:
	destroy_socket(s);
	return -1;
}


static struct impl *module_new(struct pw_core *core, struct pw_properties *properties)
{
	struct impl *impl;
	const char *name, *str;
	bool promiscuous;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("protocol-jack %p: new", impl);

	impl->core = core;
	impl->properties = properties;

	spa_list_init(&impl->socket_list);
	spa_list_init(&impl->client_list);

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

	return impl;

      error:
	free(impl);
	return NULL;
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
	module_new(module->core, NULL);
	return true;
}
