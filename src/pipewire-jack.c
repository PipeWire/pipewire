/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include <jack/jack.h>

#include <spa/param/format-utils.h>
#include <spa/param/audio/format-utils.h>

#include <pipewire/pipewire.h>

#include "extensions/client-node.h"

#define JACK_CLIENT_NAME_SIZE		64
#define JACK_PORT_NAME_SIZE		256
#define JACK_PORT_MAX			4096
#define JACK_PORT_TYPE_SIZE             32

#define REAL_JACK_PORT_NAME_SIZE JACK_CLIENT_NAME_SIZE + JACK_PORT_NAME_SIZE

struct type {
	uint32_t client_node;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->client_node = spa_type_map_get_id(map, PW_TYPE_INTERFACE__ClientNode);
        spa_type_media_type_map(map, &type->media_type);
        spa_type_media_subtype_map(map, &type->media_subtype);
        spa_type_format_audio_map(map, &type->format_audio);
        spa_type_audio_format_map(map, &type->audio_format);
}

struct global {
	bool valid;
	uint32_t type;
	void *data;
};

struct node {
	bool valid;
	uint32_t id;
	char name[JACK_CLIENT_NAME_SIZE];
};

struct port {
	bool valid;
	uint32_t id;
	uint32_t parent_id;
	unsigned long flags;
	char name[REAL_JACK_PORT_NAME_SIZE];
	enum spa_direction direction;
	const char *type;
	uint32_t type_id;
};

struct link {
	bool valid;
	uint32_t id;
	uint32_t src;
	uint32_t dst;
};

struct context {
	struct pw_main_loop *loop;
	struct pw_core *core;
	struct pw_type *t;

	struct pw_map globals;
	struct pw_array nodes;
	struct pw_array ports;
	struct pw_array links;
};

struct client {
	struct type type;

	char name[JACK_CLIENT_NAME_SIZE];

	struct context context;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	uint32_t last_sync;
	bool error;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_listener;
        struct spa_hook proxy_listener;

	struct port ports[1024];
	uint32_t n_ports;
};

static uint32_t alloc_port(struct client *client)
{
	int i;

	for (i = 0; i < client->n_ports; i++) {
		if (!client->ports[i].valid)
			break;
	}
	if (i >= 1024)
		return SPA_ID_INVALID;

	client->ports[i].valid = true;
	client->n_ports = SPA_MAX(client->n_ports, i + 1);
	return i;
}

static struct port *find_port(struct client *client, const char *name)
{
	struct port *p;

	pw_array_for_each(p, &client->context.ports) {
		if (!p->valid)
			continue;
		pw_log_debug("%s", p->name);
		if (!strcmp(p->name, name))
			return p;
	}
	return NULL;
}

void jack_get_version(int *major_ptr, int *minor_ptr, int *micro_ptr, int *proto_ptr)
{
	*major_ptr = 0;
	*minor_ptr = 0;
	*micro_ptr = 0;
	*proto_ptr = 0;
}

const char *
jack_get_version_string(void)
{
	return "0.0.0.0";
}

static void on_sync_reply(void *data, uint32_t seq)
{
	struct client *client = data;
	client->last_sync = seq;
	pw_main_loop_quit(client->context.loop);
}

static void on_state_changed(void *data, enum pw_remote_state old,
                             enum pw_remote_state state, const char *error)
{
	struct client *client = data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		client->error = true;
        case PW_REMOTE_STATE_CONNECTED:
		pw_main_loop_quit(client->context.loop);
                break;
        default:
                break;
        }
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.sync_reply = on_sync_reply,
	.state_changed = on_state_changed,
};

static int do_sync(struct client *client)
{
	uint32_t seq = client->last_sync + 1;

	pw_core_proxy_sync(client->core_proxy, seq);

	while (true) {
	        pw_main_loop_run(client->context.loop);

		if (client->error)
			return -1;

		if (client->last_sync == seq)
			break;
	}
	return 0;
}

static void on_node_proxy_destroy(void *data)
{
	struct client *client = data;

	client->node_proxy = NULL;
	spa_hook_remove(&client->proxy_listener);

}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_node_proxy_destroy,
};

static void client_node_add_mem(void *object,
				uint32_t mem_id,
				uint32_t type,
				int memfd,
				uint32_t flags)
{
}

static void client_node_transport(void *object,
                           uint32_t node_id,
                           int readfd,
                           int writefd,
                           struct pw_client_node_transport *transport)
{
}


static void client_node_set_param(void *object, uint32_t seq,
                           uint32_t id, uint32_t flags,
                           const struct spa_pod *param)
{
}

static void client_node_event(void *object, const struct spa_event *event)
{
}

static void client_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
}

static void client_node_add_port(void *object,
                          uint32_t seq,
                          enum spa_direction direction,
                          uint32_t port_id)
{
}

static void client_node_remove_port(void *object,
                             uint32_t seq,
                             enum spa_direction direction,
                             uint32_t port_id)
{
}

static void client_node_port_set_param(void *object,
                                uint32_t seq,
                                enum spa_direction direction,
                                uint32_t port_id,
                                uint32_t id, uint32_t flags,
                                const struct spa_pod *param)
{
}
static void client_node_port_use_buffers(void *object,
                                  uint32_t seq,
                                  enum spa_direction direction,
                                  uint32_t port_id,
                                  uint32_t n_buffers,
                                  struct pw_client_node_buffer *buffers)
{
}
static void client_node_port_command(void *object,
                              enum spa_direction direction,
                              uint32_t port_id,
                              const struct spa_command *command)
{
}

static void client_node_port_set_io(void *object,
                             uint32_t seq,
                             enum spa_direction direction,
                             uint32_t port_id,
                             uint32_t id,
                             uint32_t mem_id,
                             uint32_t offset,
                             uint32_t size)
{
}



static const struct pw_client_node_proxy_events client_node_events = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	.add_mem = client_node_add_mem,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_command = client_node_port_command,
	.port_set_io = client_node_port_set_io,
};

static void registry_event_global(void *data, uint32_t id, uint32_t parent_id,
                                  uint32_t permissions, uint32_t type, uint32_t version,
                                  const struct spa_dict *props)
{
	struct client *c = (struct client *) data;
        struct pw_type *t = c->context.t;
	struct global *g, *gt;
	const char *str;
	size_t size;
	struct node *n;
	struct port *p;

	if (props == NULL)
		return;

	pw_log_debug("added: %u", id);

	g = pw_array_add(&c->context.nodes, sizeof(struct global));
	g->valid = true;
	g->type = type;

	if (type == t->node) {
		if ((str = spa_dict_lookup(props, "node.name")) == NULL)
			return;

		n = pw_array_add(&c->context.nodes, sizeof(struct node));
		n->valid = true;
		n->id = id;
		strncpy(n->name, str, sizeof(n->name));

		g->data = n;
	}
	else if (type == t->port) {
		const struct spa_dict_item *item;

		if ((str = spa_dict_lookup(props, "port.name")) == NULL)
			return;

		if ((gt = pw_map_lookup(&c->context.globals, parent_id)) == NULL)
			return;

		n = gt->data;

		p = pw_array_add(&c->context.ports, sizeof(struct port));
		p->valid = true;
		p->id = id;
		p->parent_id = parent_id;
		snprintf(p->name, sizeof(p->name), "%s:%s", n->name, str);

		spa_dict_for_each(item, props) {
	                if (!strcmp(item->key, "port.direction")) {
				if (!strcmp(item->value, "in"))
					p->flags |= JackPortIsInput;
				else if (!strcmp(item->value, "out"))
					p->flags |= JackPortIsOutput;
			}
			else if (!strcmp(item->key, "port.physical")) {
				if (pw_properties_parse_bool(item->value))
					p->flags |= JackPortIsPhysical;
			}
			else if (!strcmp(item->key, "port.terminal")) {
				if (pw_properties_parse_bool(item->value))
					p->flags |= JackPortIsTerminal;
			}
		}

		g->data = n;
	}
	else if (type == t->link) {
	}
	else
		return;

        size = pw_map_get_size(&c->context.globals);
        while (id > size)
                pw_map_insert_at(&c->context.globals, size++, NULL);
        pw_map_insert_at(&c->context.globals, id, g);
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	struct client *c = (struct client *) object;

	pw_log_debug("removed: %u", id);

        pw_map_insert_at(&c->context.globals, id, NULL);
}

static const struct pw_registry_proxy_events registry_events = {
        PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_event_global,
        .global_remove = registry_event_global_remove,
};

jack_client_t * jack_client_open (const char *client_name,
                                  jack_options_t options,
                                  jack_status_t *status, ...)
{
	struct client *client;
	bool busy = true;
	struct spa_dict props;
	struct spa_dict_item items[2];

	pw_log_debug("client open %s %d", client_name, options);

	client = calloc(1, sizeof(struct client));
	if (client == NULL)
		goto init_failed;

	strncpy(client->name, client_name, JACK_CLIENT_NAME_SIZE);
	client->context.loop = pw_main_loop_new(NULL);
        client->context.core = pw_core_new(pw_main_loop_get_loop(client->context.loop), NULL);
        client->context.t = pw_core_get_type(client->context.core);
	init_type(&client->type, client->context.t->map);

	pw_map_init(&client->context.globals, 64, 64);
	pw_array_init(&client->context.nodes, 64);
	pw_array_init(&client->context.ports, 64);
	pw_array_init(&client->context.links, 64);

        client->remote = pw_remote_new(client->context.core,
				pw_properties_new(
					"client.name", client_name,
					NULL),
				0);

        pw_remote_add_listener(client->remote, &client->remote_listener, &remote_events, client);
        pw_remote_connect(client->remote);

	while (busy) {
		const char *error = NULL;

	        pw_main_loop_run(client->context.loop);

		switch (pw_remote_get_state(client->remote, &error)) {
		case PW_REMOTE_STATE_ERROR:
			goto server_failed;

		case PW_REMOTE_STATE_CONNECTED:
			busy = false;
			break;

		default:
			break;
		}
	}
	client->core_proxy = pw_remote_get_core_proxy(client->remote);
	client->registry_proxy = pw_core_proxy_get_registry(client->core_proxy,
						client->context.t->registry,
						PW_VERSION_REGISTRY, 0);
	pw_registry_proxy_add_listener(client->registry_proxy,
                                               &client->registry_listener,
                                               &registry_events, client);


	props = SPA_DICT_INIT(items, 0);
	items[props.n_items++] = SPA_DICT_ITEM_INIT("node.name", client_name);

	client->node_proxy = pw_core_proxy_create_object(client->core_proxy,
				"client-node",
				client->type.client_node,
				PW_VERSION_CLIENT_NODE,
				&props,
				0);
	if (client->node_proxy == NULL)
		goto init_failed;

	pw_client_node_proxy_add_listener(client->node_proxy,
			&client->node_listener, &client_node_events, client);
        pw_proxy_add_listener((struct pw_proxy*)client->node_proxy,
			&client->proxy_listener, &proxy_events, client);

	pw_client_node_proxy_update(client->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
				    PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS,
				    0, 0, 0, NULL);

	if (do_sync(client) < 0)
		goto init_failed;

	*status = 0;

	return (jack_client_t *)client;

      init_failed:
	*status = JackFailure | JackInitFailure;
	return NULL;
      server_failed:
	*status = JackFailure | JackServerFailed;
	return NULL;
}

jack_client_t * jack_client_new (const char *client_name)
{
	jack_options_t options = JackUseExactName;
	jack_status_t status;

        if (getenv("JACK_START_SERVER") == NULL)
            options |= JackNoStartServer;

	return jack_client_open(client_name, options, &status, NULL);
}

int jack_client_close (jack_client_t *client)
{
	struct client *c = (struct client *) client;

	pw_log_debug("client %p: close", client);

	pw_core_destroy(c->context.core);
        pw_main_loop_destroy(c->context.loop);
	free(c);

	return 0;
}

int jack_client_name_size (void)
{
	return JACK_CLIENT_NAME_SIZE;
}

char * jack_get_client_name (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return c->name;
}

char *jack_get_uuid_for_client_name (jack_client_t *client,
                                     const char    *client_name)
{
	return NULL;
}

char *jack_get_client_name_by_uuid (jack_client_t *client,
                                    const char    *client_uuid )
{
	return NULL;
}

int jack_internal_client_new (const char *client_name,
                              const char *load_name,
                              const char *load_init)
{
	return 0;
}

void jack_internal_client_close (const char *client_name)
{
}

int jack_activate (jack_client_t *client)
{
	struct client *c = (struct client *) client;

        pw_client_node_proxy_done(c->node_proxy, 0, 0);
	pw_client_node_proxy_set_active(c->node_proxy, true);

	if (do_sync(c) < 0)
		return -1;

	return 0;
}

int jack_deactivate (jack_client_t *client)
{
	return 0;
}

int jack_get_client_pid (const char *name)
{
	return 0;
}

jack_native_thread_t jack_client_thread_id (jack_client_t *client)
{
	return 0;
}

int jack_is_realtime (jack_client_t *client)
{
	return 0;
}

jack_nframes_t jack_thread_wait (jack_client_t *client, int status)
{
	return 0;
}

jack_nframes_t jack_cycle_wait (jack_client_t* client)
{
	return 0;
}

void jack_cycle_signal (jack_client_t* client, int status)
{
}

int jack_set_process_thread(jack_client_t* client, JackThreadCallback thread_callback, void *arg)
{
	return 0;
}

int jack_set_thread_init_callback (jack_client_t *client,
                                   JackThreadInitCallback thread_init_callback,
                                   void *arg)
{
	return 0;
}

void jack_on_shutdown (jack_client_t *client,
                       JackShutdownCallback shutdown_callback, void *arg)
{
}

void jack_on_info_shutdown (jack_client_t *client,
                            JackInfoShutdownCallback shutdown_callback, void *arg)
{
}

int jack_set_process_callback (jack_client_t *client,
                               JackProcessCallback process_callback,
                               void *arg)
{
	return 0;
}

int jack_set_freewheel_callback (jack_client_t *client,
                                 JackFreewheelCallback freewheel_callback,
                                 void *arg)
{
	return 0;
}

int jack_set_buffer_size_callback (jack_client_t *client,
                                   JackBufferSizeCallback bufsize_callback,
                                   void *arg)
{
	return 0;
}

int jack_set_sample_rate_callback (jack_client_t *client,
                                   JackSampleRateCallback srate_callback,
                                   void *arg)
{
	return 0;
}

int jack_set_client_registration_callback (jack_client_t *client,
                                            JackClientRegistrationCallback
                                            registration_callback, void *arg)
{
	return 0;
}

int jack_set_port_registration_callback (jack_client_t *client,
                                          JackPortRegistrationCallback
                                          registration_callback, void *arg)
{
	return 0;
}

int jack_set_port_connect_callback (jack_client_t *client,
                                    JackPortConnectCallback
                                    connect_callback, void *arg)
{
	return 0;
}

int jack_set_port_rename_callback (jack_client_t *client,
                                   JackPortRenameCallback
                                   rename_callback, void *arg)
{
	return 0;
}

int jack_set_graph_order_callback (jack_client_t *client,
                                   JackGraphOrderCallback graph_callback,
                                   void *data)
{
	return 0;
}

int jack_set_xrun_callback (jack_client_t *client,
                            JackXRunCallback xrun_callback, void *arg)
{
	return 0;
}

int jack_set_latency_callback (jack_client_t *client,
			       JackLatencyCallback latency_callback,
			       void *data)
{
	return 0;
}

int jack_set_freewheel(jack_client_t* client, int onoff)
{
	return 0;
}

int jack_set_buffer_size (jack_client_t *client, jack_nframes_t nframes)
{
	return 0;
}

jack_nframes_t jack_get_sample_rate (jack_client_t *client)
{
	return 0;
}

jack_nframes_t jack_get_buffer_size (jack_client_t *client)
{
	return 0;
}

int jack_engine_takeover_timebase (jack_client_t *client)
{
	return 0;
}

float jack_cpu_load (jack_client_t *client)
{
	return 0.0;
}

jack_port_t * jack_port_register (jack_client_t *client,
                                  const char *port_name,
                                  const char *port_type,
                                  unsigned long flags,
                                  unsigned long buffer_size)
{
	struct client *c = (struct client *) client;
	enum spa_direction direction;
	struct spa_port_info port_info = { 0, };
	struct spa_dict dict;
	struct spa_dict_item items[10];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct pw_type *t = c->context.t;
	struct spa_pod *params[4];
	uint32_t port_id;
	struct port *p;

	pw_log_debug("client %p: port register \"%s\" \"%s\" %ld %ld",
			c, port_name, port_type, flags, buffer_size);

	if (flags & JackPortIsInput)
		direction = PW_DIRECTION_INPUT;
	else if (flags & JackPortIsOutput)
		direction = PW_DIRECTION_OUTPUT;
	else
		return NULL;

	if ((port_id = alloc_port(c)) == SPA_ID_INVALID)
		return NULL;

	p = &c->ports[port_id];
	p->id = port_id;
	snprintf(p->name, sizeof(p->name), "%s:%s", c->name, port_name);
	p->direction = direction;
	p->type = port_type;

	if (strcmp(port_type, JACK_DEFAULT_AUDIO_TYPE) == 0)
		p->type_id = 0;
	else if (strcmp(port_type, JACK_DEFAULT_MIDI_TYPE) == 0)
		p->type_id = 1;
	else
		return NULL;

	port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
			  SPA_PORT_INFO_FLAG_NO_REF;

	port_info.props = &dict;
	dict = SPA_DICT_INIT(items, 0);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT("port.name", port_name);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT("port.type", port_type);

	params[0] = spa_pod_builder_object(&b,
		t->param.idEnumFormat, t->spa_format,
		"I", c->type.media_type.audio,
		"I", c->type.media_subtype.raw,
                ":", c->type.format_audio.format,   "I", c->type.audio_format.F32,
                ":", c->type.format_audio.channels, "i", 1,
                ":", c->type.format_audio.rate,     "iru", 44100, 2, 1, INT32_MAX);

	params[1] = spa_pod_builder_object(&b,
		t->param.idBuffers, t->param_buffers.Buffers,
		":", t->param_buffers.size,    "isu", 128, 3, 4, INT32_MAX, 4,
		":", t->param_buffers.stride,  "i", 4,
		":", t->param_buffers.buffers, "iru", 1, 2, 1, 2,
		":", t->param_buffers.align,   "i", 16);

	pw_client_node_proxy_port_update(c->node_proxy,
					 direction,
					 port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
					 PW_CLIENT_NODE_PORT_UPDATE_INFO ,
					 2,
					 (const struct spa_pod **) params,
					 &port_info);

	if (do_sync(c) < 0)
		return NULL;

	return (jack_port_t *) &c->ports[port_id];
}

int jack_port_unregister (jack_client_t *client, jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct port *p = (struct port *) port;

	pw_log_debug("client %p: port unregister %p", client, port);

	p->valid = false;

	pw_client_node_proxy_port_update(c->node_proxy,
					 p->direction,
					 p->id,
					 0, 0, NULL, NULL);

	if (do_sync(c) < 0)
		return -1;

	return 0;
}

void * jack_port_get_buffer (jack_port_t *port, jack_nframes_t frames)
{
	return 0;
}

jack_uuid_t jack_port_uuid (const jack_port_t *port)
{
	return 0;
}

const char * jack_port_name (const jack_port_t *port)
{
	struct port *p = (struct port *) port;
	return p->name;
}

const char * jack_port_short_name (const jack_port_t *port)
{
	struct port *p = (struct port *) port;
	return strchr(p->name, ':') + 1;
}

int jack_port_flags (const jack_port_t *port)
{
	struct port *p = (struct port *) port;
	return p->flags;
}

const char * jack_port_type (const jack_port_t *port)
{
	struct port *p = (struct port *) port;
	return p->type;
}

jack_port_type_id_t jack_port_type_id (const jack_port_t *port)
{
	struct port *p = (struct port *) port;
	return p->type_id;
}

int jack_port_is_mine (const jack_client_t *client, const jack_port_t *port)
{
	return 0;
}

int jack_port_connected (const jack_port_t *port)
{
	return 0;
}

int jack_port_connected_to (const jack_port_t *port,
                            const char *port_name)
{
	return 0;
}

const char ** jack_port_get_connections (const jack_port_t *port)
{
	return NULL;
}

const char ** jack_port_get_all_connections (const jack_client_t *client,
                                             const jack_port_t *port)
{
	return NULL;
}

int jack_port_tie (jack_port_t *src, jack_port_t *dst)
{
	return 0;
}

int jack_port_untie (jack_port_t *port)
{
	return 0;
}

int jack_port_set_name (jack_port_t *port, const char *port_name)
{
	return 0;
}

int jack_port_set_alias (jack_port_t *port, const char *alias)
{
	return 0;
}

int jack_port_unset_alias (jack_port_t *port, const char *alias)
{
	return 0;
}

int jack_port_get_aliases (const jack_port_t *port, char* const aliases[2])
{
	return 0;
}

int jack_port_request_monitor (jack_port_t *port, int onoff)
{
	return 0;
}

int jack_port_request_monitor_by_name (jack_client_t *client,
                                       const char *port_name, int onoff)
{
	return 0;
}

int jack_port_ensure_monitor (jack_port_t *port, int onoff)
{
	return 0;
}

int jack_port_monitoring_input (jack_port_t *port)
{
	return 0;
}

int jack_connect (jack_client_t *client,
                  const char *source_port,
                  const char *destination_port)
{
	struct client *c = (struct client *) client;
	struct port *src, *dst;
	struct pw_properties *props;

	pw_log_debug("client %p: connect %s %s", client, source_port, destination_port);

	src = find_port(c, source_port);
	dst = find_port(c, destination_port);

	if (src == NULL || dst == NULL)
		return -1;

	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_LINK_OUTPUT_NODE_ID, "%d", src->parent_id);
	pw_properties_setf(props, PW_LINK_OUTPUT_PORT_ID, "%d", src->id);
	pw_properties_setf(props, PW_LINK_INPUT_NODE_ID, "%d", dst->parent_id);
	pw_properties_setf(props, PW_LINK_INPUT_PORT_ID, "%d", dst->id);

	pw_core_proxy_create_object(c->core_proxy,
					  "link-factory",
					  c->context.t->link,
					  PW_VERSION_LINK,
					  &props->dict,
					  0);

	pw_properties_free(props);

	if (do_sync(c) < 0)
		return -1;

	return 0;
}

int jack_disconnect (jack_client_t *client,
                     const char *source_port,
                     const char *destination_port)
{
	pw_log_debug("client %p: connect %s %s", client, source_port, destination_port);

	return 0;
}

int jack_port_disconnect (jack_client_t *client, jack_port_t *port)
{
	return 0;
}

int jack_port_name_size(void)
{
	return REAL_JACK_PORT_NAME_SIZE;
}

int jack_port_type_size(void)
{
	return JACK_PORT_TYPE_SIZE;
}

size_t jack_port_type_get_buffer_size (jack_client_t *client, const char *port_type)
{
	return 0;
}

void jack_port_set_latency (jack_port_t *port, jack_nframes_t frames)
{
}

void jack_port_get_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
}

void jack_port_set_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
}

int jack_recompute_total_latencies (jack_client_t *client)
{
	return 0;
}

jack_nframes_t jack_port_get_latency (jack_port_t *port)
{
	return 0;
}

jack_nframes_t jack_port_get_total_latency (jack_client_t *client,
					    jack_port_t *port)
{
	return 0;
}

int jack_recompute_total_latency (jack_client_t *client, jack_port_t* port)
{
	return 0;
}

const char ** jack_get_ports (jack_client_t *client,
                              const char *port_name_pattern,
                              const char *type_name_pattern,
                              unsigned long flags)
{
	struct client *c = (struct client *) client;
	const char **res = malloc(sizeof(char*) * JACK_PORT_MAX);
	int count = 0;
	struct port *p;

	pw_array_for_each(p, &c->context.ports) {
		if (!p->valid)
			continue;
		if (!SPA_FLAG_CHECK(p->flags, flags))
			continue;

		res[count++] = p->name;
	}
	res[count] = NULL;

	return res;
}

jack_port_t * jack_port_by_name (jack_client_t *client, const char *port_name)
{
	return NULL;
}

jack_port_t * jack_port_by_id (jack_client_t *client,
                               jack_port_id_t port_id)
{
	return NULL;
}

jack_nframes_t jack_frames_since_cycle_start (const jack_client_t *client)
{
	return 0;
}

jack_nframes_t jack_frame_time (const jack_client_t *client)
{
	return 0;
}

jack_nframes_t jack_last_frame_time (const jack_client_t *client)
{
	return 0;
}

int jack_get_cycle_times(const jack_client_t *client,
                        jack_nframes_t *current_frames,
                        jack_time_t    *current_usecs,
                        jack_time_t    *next_usecs,
                        float          *period_usecs)
{
	return 0;
}

jack_time_t jack_frames_to_time(const jack_client_t *client, jack_nframes_t frames)
{
	return 0;
}

jack_nframes_t jack_time_to_frames(const jack_client_t *client, jack_time_t usecs)
{
	return 0;
}

jack_time_t jack_get_time()
{
	return 0;
}

void jack_set_error_function (void (*func)(const char *))
{
}

void jack_set_info_function (void (*func)(const char *))
{
}

void jack_free(void* ptr)
{
}

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	pw_init(NULL, NULL);
}
