/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/pod.h>
#include <spa/support/dbus.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "extensions/session-manager.h"

#include <dbus/dbus.h>

#include "media-session.h"

#define NAME "media-session"

#define sm_object_emit(o,m,v,...) spa_hook_list_call(&(o)->hooks, struct sm_object_events, m, v, ##__VA_ARGS__)

#define sm_object_emit_update(s)		sm_object_emit(s, update, 0)

#define sm_media_session_emit(s,m,v,...) spa_hook_list_call(&s->hooks, struct sm_media_session_events, m, v, ##__VA_ARGS__)

#define sm_media_session_emit_create(s,obj)		sm_media_session_emit(s, create, 0, obj)
#define sm_media_session_emit_remove(s,obj)		sm_media_session_emit(s, remove, 0, obj)
#define sm_media_session_emit_rescan(s,seq)		sm_media_session_emit(s, rescan, 0, seq)

void * sm_stream_monitor_start(struct sm_media_session *sess);
void * sm_metadata_start(struct sm_media_session *sess);
void * sm_alsa_midi_start(struct sm_media_session *sess);
void * sm_v4l2_monitor_start(struct sm_media_session *sess);
void * sm_bluez5_monitor_start(struct sm_media_session *sess);
void * sm_alsa_monitor_start(struct sm_media_session *sess);
int sm_policy_ep_start(struct sm_media_session *sess);

/** user data to add to an object */
struct data {
	struct spa_list link;
	const char *id;
	size_t size;
};

struct param {
	struct sm_param this;
};

struct sync {
	struct spa_list link;
	int seq;
	void (*callback) (void *data);
	void *data;
};

struct impl {
	struct sm_media_session this;
	uint32_t session_id;

	struct pw_main_loop *loop;
	struct spa_dbus *dbus;

	struct pw_remote *monitor_remote;
	struct spa_hook monitor_listener;

	struct pw_remote *policy_remote;
	struct spa_hook policy_listener;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_map globals;
	struct spa_list global_list;

	struct spa_hook_list hooks;

	struct pw_client_session_proxy *client_session;
	struct spa_hook client_session_listener;

	struct spa_list endpoint_link_list;	/** list of struct endpoint_link */
	struct pw_map endpoint_links;		/** map of endpoint_link */

	struct spa_list sync_list;		/** list of struct sync */
	int rescan_seq;
	int last_seq;
};

struct endpoint_link {
	uint32_t id;

	struct pw_endpoint_link_info info;

	struct impl *impl;

	struct spa_list link;			/**< link in struct impl endpoint_link_list */
	struct spa_list link_list;		/**< list of struct link */
};

struct link {
	struct pw_proxy *proxy;		/**< proxy for link */
	struct spa_hook listener;	/**< proxy listener */

	uint32_t output_node;
	uint32_t output_port;
	uint32_t input_node;
	uint32_t input_port;

	struct endpoint_link *endpoint_link;
	struct spa_list link;		/**< link in struct endpoint_link link_list */
};

static void add_object(struct impl *impl, struct sm_object *obj)
{
	size_t size = pw_map_get_size(&impl->globals);
	while (obj->id > size)
		pw_map_insert_at(&impl->globals, size++, NULL);
	pw_map_insert_at(&impl->globals, obj->id, obj);
	spa_list_append(&impl->global_list, &obj->link);
}

static void remove_object(struct impl *impl, struct sm_object *obj)
{
	pw_map_insert_at(&impl->globals, obj->id, NULL);
	spa_list_remove(&obj->link);
}

static void *find_object(struct impl *impl, uint32_t id)
{
	void *obj;
	if ((obj = pw_map_lookup(&impl->globals, id)) != NULL)
		return obj;
	return NULL;
}

static struct data *object_find_data(struct sm_object *obj, const char *id)
{
	struct data *d;
	spa_list_for_each(d, &obj->data, link) {
		if (strcmp(d->id, id) == 0)
			return d;
	}
	return NULL;
}

void *sm_object_add_data(struct sm_object *obj, const char *id, size_t size)
{
	struct data *d;

	d = object_find_data(obj, id);
	if (d != NULL) {
		if (d->size == size)
			goto done;
		spa_list_remove(&d->link);
		free(d);
	}
	d = calloc(1, sizeof(struct data) + size);
	d->id = id;
	d->size = size;

	spa_list_append(&obj->data, &d->link);
done:
	return SPA_MEMBER(d, sizeof(struct data), void);
}

void *sm_object_get_data(struct sm_object *obj, const char *id)
{
	struct data *d;
	d = object_find_data(obj, id);
	if (d == NULL)
		return NULL;
	return SPA_MEMBER(d, sizeof(struct data), void);
}

int sm_object_remove_data(struct sm_object *obj, const char *id)
{
	struct data *d;
	d = object_find_data(obj, id);
	if (d == NULL)
		return -ENOENT;
	spa_list_remove(&d->link);
	free(d);
	return 0;
}

/**
 * Clients
 */
static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct sm_client *client = object;
	struct impl *impl = SPA_CONTAINER_OF(client->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: client %d info", impl, client->obj.id);
	client->info = pw_client_info_update(client->info, info);

	client->avail |= SM_CLIENT_CHANGE_MASK_INFO;
	client->changed |= SM_CLIENT_CHANGE_MASK_INFO;
	sm_object_emit_update(&client->obj);
	client->changed = 0;
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info,
};

static void client_destroy(void *object)
{
	struct sm_client *client = object;
	if (client->info)
		pw_client_info_free(client->info);
}

static struct param *add_param(struct spa_list *param_list,
		uint32_t id, const struct spa_pod *param)
{
	struct param *p;

	if (param == NULL || !spa_pod_is_object(param)) {
		errno = EINVAL;
		return NULL;
	}
	if (id == SPA_ID_INVALID)
		id = SPA_POD_OBJECT_ID(param);

	p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
	if (p == NULL)
		return NULL;

	p->this.id = id;
	p->this.param = SPA_MEMBER(p, sizeof(struct param), struct spa_pod);
	memcpy(p->this.param, param, SPA_POD_SIZE(param));

	spa_list_append(param_list, &p->this.link);

	return p;
}


static void clear_params(struct spa_list *param_list, uint32_t id)
{
	struct param *p, *t;

	spa_list_for_each_safe(p, t, param_list, this.link) {
		if (id == SPA_ID_INVALID || p->this.id == id) {
			spa_list_remove(&p->this.link);
			free(p);
		}
	}
}

/**
 * Node
 */
static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct sm_node *node = object;
	struct impl *impl = SPA_CONTAINER_OF(node->obj.session, struct impl, this);
	uint32_t i;

	pw_log_debug(NAME" %p: node %d info", impl, node->obj.id);
	node->info = pw_node_info_update(node->info, info);

	if (node->obj.id == SPA_ID_INVALID) {
		node->obj.id = info->id;
		pw_log_debug(NAME" %p: node %d added", impl, node->obj.id);
		add_object(impl, &node->obj);
	}

	node->avail |= SM_NODE_CHANGE_MASK_INFO;
	node->changed |= SM_NODE_CHANGE_MASK_INFO;
	sm_object_emit_update(&node->obj);
	node->changed = 0;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS &&
	    (node->mask & SM_NODE_CHANGE_MASK_PARAMS) &&
	    !node->subscribe) {
		uint32_t subscribe[info->n_params], n_subscribe = 0;

		for (i = 0; i < info->n_params; i++) {
			switch (info->params[i].id) {
			case SPA_PARAM_PropInfo:
			case SPA_PARAM_Props:
			case SPA_PARAM_EnumFormat:
				subscribe[n_subscribe++] = info->params[i].id;
				break;
			default:
				break;
			}
		}
		if (n_subscribe > 0) {
			pw_log_debug(NAME" %p: node %d subscribe %d params", impl,
					node->obj.id, n_subscribe);
			pw_node_proxy_subscribe_params((struct pw_node_proxy*)node->obj.proxy,
					subscribe, n_subscribe);
			node->subscribe = true;
		}
	}
}

static void node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct sm_node *node = object;
	struct impl *impl = SPA_CONTAINER_OF(node->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: node %p param %d index:%d", impl, node, id, index);
	clear_params(&node->param_list, id);

	add_param(&node->param_list, id, param);

	node->avail |= SM_NODE_CHANGE_MASK_PARAMS;
	node->changed |= SM_NODE_CHANGE_MASK_PARAMS;
	sm_object_emit_update(&node->obj);
	node->changed = 0;
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void node_destroy(void *object)
{
	struct sm_node *node = object;
	struct sm_port *port;

	spa_list_consume(port, &node->port_list, link) {
		port->node = NULL;
		spa_list_remove(&port->link);
	}
	clear_params(&node->param_list, SPA_ID_INVALID);

	if (node->info)
		pw_node_info_free(node->info);
}

/**
 * Port
 */
static void port_event_info(void *object, const struct pw_port_info *info)
{
	struct sm_port *port = object;
	struct impl *impl = SPA_CONTAINER_OF(port->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: port %d info", impl, port->obj.id);
	port->info = pw_port_info_update(port->info, info);

	port->avail |= SM_PORT_CHANGE_MASK_INFO;
	port->changed |= SM_PORT_CHANGE_MASK_INFO;
	sm_object_emit_update(&port->obj);
	port->changed = 0;
}

static const struct pw_port_proxy_events port_events = {
	PW_VERSION_PORT_PROXY_EVENTS,
	.info = port_event_info,
};

static void port_destroy(void *object)
{
	struct sm_port *port = object;
	if (port->info)
		pw_port_info_free(port->info);
	if (port->node) {
		spa_list_remove(&port->link);
		port->node->changed |= SM_NODE_CHANGE_MASK_PORTS;
	}
}

/**
 * Session
 */
static void session_event_info(void *object, const struct pw_session_info *info)
{
	struct sm_session *sess = object;
	struct impl *impl = SPA_CONTAINER_OF(sess->obj.session, struct impl, this);
	struct pw_session_info *i = sess->info;

	pw_log_debug(NAME" %p: session %d info", impl, sess->obj.id);
	if (i == NULL && info) {
		i = sess->info = calloc(1, sizeof(struct pw_session_info));
		i->version = PW_VERSION_SESSION_INFO;
		i->id = info->id;
        }
	i->change_mask = info->change_mask;
	if (info->change_mask & PW_SESSION_CHANGE_MASK_PROPS) {
		if (i->props)
			pw_properties_free ((struct pw_properties *)i->props);
		i->props = (struct spa_dict *) pw_properties_new_dict (info->props);
	}

	sess->avail |= SM_SESSION_CHANGE_MASK_INFO;
	sess->changed |= SM_SESSION_CHANGE_MASK_INFO;
	sm_object_emit_update(&sess->obj);
	sess->changed = 0;
}

static const struct pw_session_proxy_events session_events = {
	PW_VERSION_SESSION_PROXY_EVENTS,
	.info = session_event_info,
};

static void session_destroy(void *object)
{
	struct sm_session *sess = object;
	struct sm_endpoint *endpoint;

	if (sess->info) {
		free(sess->info);
	}

	spa_list_consume(endpoint, &sess->endpoint_list, link) {
		endpoint->session = NULL;
		spa_list_remove(&endpoint->link);
	}
}

/**
 * Endpoint
 */
static void endpoint_event_info(void *object, const struct pw_endpoint_info *info)
{
	struct sm_endpoint *endpoint = object;
	struct impl *impl = SPA_CONTAINER_OF(endpoint->obj.session, struct impl, this);
	struct pw_endpoint_info *i = endpoint->info;
	const char *str;

	pw_log_debug(NAME" %p: endpoint %d info", impl, endpoint->obj.id);
	if (i == NULL && info) {
		i = endpoint->info = calloc(1, sizeof(struct pw_endpoint_info));
		i->id = info->id;
		i->name = info->name ? strdup(info->name) : NULL;
		i->media_class = info->media_class ? strdup(info->media_class) : NULL;
		i->direction = info->direction;
		i->flags = info->flags;
        }
	i->change_mask = info->change_mask;
	if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION) {
		i->session_id = info->session_id;
	}
	if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
		if (i->props)
			pw_properties_free ((struct pw_properties *)i->props);
		i->props = (struct spa_dict *) pw_properties_new_dict (info->props);
		if ((str = spa_dict_lookup(i->props, PW_KEY_PRIORITY_SESSION)) != NULL)
			endpoint->priority = pw_properties_parse_int(str);
	}

	endpoint->avail |= SM_ENDPOINT_CHANGE_MASK_INFO;
	endpoint->changed |= SM_ENDPOINT_CHANGE_MASK_INFO;
	sm_object_emit_update(&endpoint->obj);
	endpoint->changed = 0;
}

static const struct pw_endpoint_proxy_events endpoint_events = {
	PW_VERSION_ENDPOINT_PROXY_EVENTS,
	.info = endpoint_event_info,
};

static void endpoint_destroy(void *object)
{
	struct sm_endpoint *endpoint = object;
	struct sm_endpoint_stream *stream;

	if (endpoint->info) {
		free(endpoint->info->name);
		free(endpoint->info->media_class);
		free(endpoint->info);
	}

	spa_list_consume(stream, &endpoint->stream_list, link) {
		stream->endpoint = NULL;
		spa_list_remove(&stream->link);
	}
	if (endpoint->session) {
		endpoint->session = NULL;
		spa_list_remove(&endpoint->link);
	}
}

/**
 * Endpoint Stream
 */
static void endpoint_stream_event_info(void *object, const struct pw_endpoint_stream_info *info)
{
	struct sm_endpoint_stream *stream = object;
	struct impl *impl = SPA_CONTAINER_OF(stream->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: endpoint stream %d info", impl, stream->obj.id);
	if (stream->info == NULL && info) {
		stream->info = calloc(1, sizeof(struct pw_endpoint_stream_info));
		stream->info->version = PW_VERSION_ENDPOINT_STREAM_INFO;
		stream->info->id = info->id;
		stream->info->endpoint_id = info->endpoint_id;
		stream->info->name = info->name ? strdup(info->name) : NULL;
        }
	stream->info->change_mask = info->change_mask;

	stream->avail |= SM_ENDPOINT_CHANGE_MASK_INFO;
	stream->changed |= SM_ENDPOINT_CHANGE_MASK_INFO;
	sm_object_emit_update(&stream->obj);
	stream->changed = 0;
}

static const struct pw_endpoint_stream_proxy_events endpoint_stream_events = {
	PW_VERSION_ENDPOINT_STREAM_PROXY_EVENTS,
	.info = endpoint_stream_event_info,
};

static void endpoint_stream_destroy(void *object)
{
	struct sm_endpoint_stream *stream = object;

	if (stream->info) {
		free(stream->info->name);
		free(stream->info);
	}
	if (stream->endpoint) {
		stream->endpoint = NULL;
		spa_list_remove(&stream->link);
	}
}
/**
 * Endpoint Link
 */
static void endpoint_link_event_info(void *object, const struct pw_endpoint_link_info *info)
{
	struct sm_endpoint_link *link = object;
	struct impl *impl = SPA_CONTAINER_OF(link->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: endpoint link %d info", impl, link->obj.id);
	if (link->info == NULL && info) {
		link->info = calloc(1, sizeof(struct pw_endpoint_link_info));
		link->info->version = PW_VERSION_ENDPOINT_LINK_INFO;
		link->info->id = info->id;
		link->info->session_id = info->session_id;
		link->info->output_endpoint_id = info->output_endpoint_id;
		link->info->output_stream_id = info->output_stream_id;
		link->info->input_endpoint_id = info->input_endpoint_id;
		link->info->input_stream_id = info->input_stream_id;
	}
	link->info->change_mask = info->change_mask;

	link->avail |= SM_ENDPOINT_LINK_CHANGE_MASK_INFO;
	link->changed |= SM_ENDPOINT_LINK_CHANGE_MASK_INFO;
	sm_object_emit_update(&link->obj);
	link->changed = 0;
}

static const struct pw_endpoint_link_proxy_events endpoint_link_events = {
	PW_VERSION_ENDPOINT_LINK_PROXY_EVENTS,
	.info = endpoint_link_event_info,
};

static void endpoint_link_destroy(void *object)
{
	struct sm_endpoint_link *link = object;

	if (link->info) {
		free(link->info->error);
		free(link->info);
	}
	if (link->output) {
		link->output = NULL;
		spa_list_remove(&link->output_link);
	}
	if (link->input) {
		link->input = NULL;
		spa_list_remove(&link->input_link);
	}
}

/**
 * Proxy
 */
static void
destroy_proxy (void *data)
{
	struct sm_object *obj = data;
	struct impl *impl = SPA_CONTAINER_OF(obj->session, struct impl, this);

	sm_media_session_emit_remove(impl, obj);

	if (obj->destroy)
		obj->destroy(obj);
}

static const struct pw_proxy_events proxy_events = {
        PW_VERSION_PROXY_EVENTS,
        .destroy = destroy_proxy,
};

static void
init_object(struct impl *impl, struct sm_object *obj, uint32_t id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	int res;
	const void *events;
        uint32_t client_version;
        pw_destroy_t destroy;
	size_t user_data_size;
	const char *str;
	struct pw_proxy *proxy;

	proxy = obj ? obj->proxy : NULL;

	pw_log_debug(NAME " %p: init '%d' %d", impl, id, type);

	switch (type) {
	case PW_TYPE_INTERFACE_Client:
		events = &client_events;
                client_version = PW_VERSION_CLIENT_PROXY;
                destroy = (pw_destroy_t) client_destroy;
		user_data_size = sizeof(struct sm_client);
		break;

	case PW_TYPE_INTERFACE_Node:
		events = &node_events;
                client_version = PW_VERSION_NODE_PROXY;
                destroy = (pw_destroy_t) node_destroy;
		user_data_size = sizeof(struct sm_node);
		break;

	case PW_TYPE_INTERFACE_Port:
		events = &port_events;
                client_version = PW_VERSION_PORT_PROXY;
                destroy = (pw_destroy_t) port_destroy;
		user_data_size = sizeof(struct sm_port);
		break;

	case PW_TYPE_INTERFACE_Session:
		events = &session_events;
                client_version = PW_VERSION_SESSION_PROXY;
                destroy = (pw_destroy_t) session_destroy;
		user_data_size = sizeof(struct sm_session);
		break;

	case PW_TYPE_INTERFACE_Endpoint:
		events = &endpoint_events;
                client_version = PW_VERSION_ENDPOINT_PROXY;
                destroy = (pw_destroy_t) endpoint_destroy;
		user_data_size = sizeof(struct sm_endpoint);
		break;

	case PW_TYPE_INTERFACE_EndpointStream:
		events = &endpoint_stream_events;
                client_version = PW_VERSION_ENDPOINT_STREAM_PROXY;
                destroy = (pw_destroy_t) endpoint_stream_destroy;
		user_data_size = sizeof(struct sm_endpoint_stream);
		break;

	case PW_TYPE_INTERFACE_EndpointLink:
		events = &endpoint_link_events;
		client_version = PW_VERSION_ENDPOINT_LINK_PROXY;
		destroy = (pw_destroy_t) endpoint_link_destroy;
		user_data_size = sizeof(struct sm_endpoint_link);
		break;

	default:
		return;
	}

	if (proxy == NULL) {
		proxy = pw_registry_proxy_bind(impl->registry_proxy,
				id, type, client_version, user_data_size);
		if (proxy == NULL) {
			res = -errno;
			goto error;
		}
	}
	if (obj == NULL)
		obj = pw_proxy_get_user_data(proxy);
	obj->session = &impl->this;
	obj->id = id;
	obj->type = type;
	obj->props = props ? pw_properties_new_dict(props) : pw_properties_new(NULL, NULL);
	obj->proxy = proxy;
	obj->destroy = destroy;
	obj->mask = SM_OBJECT_CHANGE_MASK_PROPERTIES | SM_OBJECT_CHANGE_MASK_BIND;
	obj->avail = obj->mask;
	spa_hook_list_init(&obj->hooks);
	spa_list_init(&obj->data);
	if (id != SPA_ID_INVALID)
		add_object(impl, obj);

	pw_proxy_add_listener(proxy, &obj->proxy_listener, &proxy_events, obj);

	switch (type) {
	case PW_TYPE_INTERFACE_Node:
	{
		struct sm_node *node = (struct sm_node*) obj;
		spa_list_init(&node->port_list);
		spa_list_init(&node->param_list);
		break;
	}
	case PW_TYPE_INTERFACE_Port:
	{
		struct sm_port *port = (struct sm_port*) obj;

		if (props) {
			if ((str = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) != NULL)
				port->direction = strcmp(str, "out") == 0 ?
					PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
			if ((str = spa_dict_lookup(props, PW_KEY_NODE_ID)) != NULL)
				port->node = find_object(impl, atoi(str));

			pw_log_debug(NAME" %p: port %d parent node %s direction:%d", impl, id, str,
					port->direction);
			if (port->node) {
				spa_list_append(&port->node->port_list, &port->link);
				port->node->changed |= SM_NODE_CHANGE_MASK_PORTS;
			}
		}
		break;
	}
	case PW_TYPE_INTERFACE_Session:
	{
		struct sm_session *sess = (struct sm_session*) obj;
		if (id == impl->session_id)
			impl->this.session = sess;
		spa_list_init(&sess->endpoint_list);
		break;
	}
	case PW_TYPE_INTERFACE_Endpoint:
	{
		struct sm_endpoint *endpoint = (struct sm_endpoint*) obj;
		if (props) {
			if ((str = spa_dict_lookup(props, PW_KEY_SESSION_ID)) != NULL)
				endpoint->session = find_object(impl, atoi(str));
			pw_log_debug(NAME" %p: endpoint %d parent session %s", impl, id, str);
			if (endpoint->session)
				spa_list_append(&endpoint->session->endpoint_list, &endpoint->link);
		}
		spa_list_init(&endpoint->stream_list);
		break;
	}
	case PW_TYPE_INTERFACE_EndpointStream:
	{
		struct sm_endpoint_stream *stream = (struct sm_endpoint_stream*) obj;

		if (props) {
			if ((str = spa_dict_lookup(props, PW_KEY_ENDPOINT_ID)) != NULL)
				stream->endpoint = find_object(impl, atoi(str));
			pw_log_debug(NAME" %p: stream %d parent endpoint %s", impl, id, str);
			if (stream->endpoint) {
				spa_list_append(&stream->endpoint->stream_list, &stream->link);
				stream->endpoint->changed |= SM_ENDPOINT_CHANGE_MASK_STREAMS;
			}
		}
		spa_list_init(&stream->link_list);
		break;
	}
	default:
		break;
	}

	sm_media_session_emit_create(impl, obj);
	pw_proxy_add_object_listener(proxy, &obj->object_listener, events, obj);

	return;

error:
	pw_log_warn(NAME" %p: can't handle global %d: %s", impl, id, spa_strerror(res));
}

static void
registry_global(void *data, uint32_t id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
        struct sm_object *obj;

	pw_log_debug(NAME " %p: new global '%d' %d", impl, id, type);

	obj = find_object(impl, id);
	if (obj == NULL) {
		init_object(impl, obj, id, permissions, type, version, props);
	} else {
		pw_log_debug(NAME " %p: our object %d appeared", impl, id);
	}
}

int sm_object_add_listener(struct sm_object *obj, struct spa_hook *listener,
		const struct sm_object_events *events, void *data)
{
	spa_hook_list_append(&obj->hooks, listener, events, data);
	return 0;
}

int sm_media_session_add_listener(struct sm_media_session *sess, struct spa_hook *listener,
                const struct sm_media_session_events *events, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct spa_hook_list save;
	struct sm_object *obj;

	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	spa_list_for_each(obj, &impl->global_list, link)
		sm_media_session_emit_create(impl, obj);

        spa_hook_list_join(&impl->hooks, &save);

	return 0;
}

struct sm_object *sm_media_session_find_object(struct sm_media_session *sess, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	return find_object(impl, id);
}

int sm_media_session_schedule_rescan(struct sm_media_session *sess)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	if (impl->core_proxy)
		impl->rescan_seq = pw_core_proxy_sync(impl->core_proxy, 0, impl->last_seq);
	return impl->rescan_seq;
}

int sm_media_session_sync(struct sm_media_session *sess,
		void (*callback) (void *data), void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct sync *sync;

	sync = calloc(1, sizeof(struct sync));
	if (sync == NULL)
		return -errno;

	spa_list_append(&impl->sync_list, &sync->link);
	sync->callback = callback;
	sync->data = data;
	sync->seq = pw_core_proxy_sync(impl->core_proxy, 0, impl->last_seq);
	return sync->seq;
}

static void roundtrip_callback(void *data)
{
	int *done = data;
	*done = 1;
}

int sm_media_session_roundtrip(struct sm_media_session *sess)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct pw_loop *loop = impl->this.loop;
	int done, res;

	if (impl->core_proxy == NULL)
		return -EIO;

	done = 0;
	if ((res = sm_media_session_sync(sess, roundtrip_callback, &done)) < 0)
		return res;

	pw_log_debug(NAME" %p: roundtrip %d", impl, res);

	pw_loop_enter(loop);
	while (!done) {
		if ((res = pw_loop_iterate(loop, -1)) < 0) {
			pw_log_warn(NAME" %p: iterate error %d (%s)",
				loop, res, spa_strerror(res));
			break;
		}
	}
        pw_loop_leave(loop);

	pw_log_debug(NAME" %p: roundtrip done", impl);

	return 0;
}

static void
registry_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct sm_object *obj;

	pw_log_debug(NAME " %p: remove global '%d'", impl, id);

	if ((obj = find_object(impl, id)) == NULL)
		return;

	remove_object(impl, obj);
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_global,
        .global_remove = registry_global_remove,
};

struct pw_proxy *sm_media_session_export(struct sm_media_session *sess,
		uint32_t type, struct pw_properties *properties,
		void *object, size_t user_data_size)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	return pw_remote_export(impl->monitor_remote, type,
			properties, object, user_data_size);
}

struct pw_proxy *sm_media_session_create_object(struct sm_media_session *sess,
		const char *factory_name, uint32_t type, uint32_t version,
		const struct spa_dict *props, size_t user_data_size)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	return pw_core_proxy_create_object(impl->core_proxy,
			factory_name, type, version, props, user_data_size);
}

struct sm_node *sm_media_session_create_node(struct sm_media_session *sess,
		const char *factory_name, const struct spa_dict *props,
		size_t user_data_size)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct sm_node *node;
	struct pw_proxy *proxy;

	pw_log_debug(NAME " %p: node '%s'", impl, factory_name);

	proxy = pw_core_proxy_create_object(impl->core_proxy,
				factory_name,
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE_PROXY,
				props,
				sizeof(struct sm_node) + user_data_size);

	node = pw_proxy_get_user_data(proxy);
	node->obj.proxy = proxy;
	init_object(impl, &node->obj, SPA_ID_INVALID,
			PW_PERM_RWX, PW_TYPE_INTERFACE_Node,
			PW_VERSION_NODE_PROXY, props);

	return node;
}

static void check_endpoint_link(struct endpoint_link *link)
{
	if (!spa_list_is_empty(&link->link_list))
		return;

	if (link->impl) {
		spa_list_remove(&link->link);
		pw_map_remove(&link->impl->endpoint_links, link->id);

		pw_client_session_proxy_link_update(link->impl->client_session,
				link->id,
				PW_CLIENT_SESSION_LINK_UPDATE_DESTROYED,
				0, NULL, NULL);

		link->impl = NULL;
		free(link);
	}
}

static void link_proxy_destroy(void *data)
{
	struct link *l = data;

	if (l->endpoint_link) {
		spa_list_remove(&l->link);
		check_endpoint_link(l->endpoint_link);
		l->endpoint_link = NULL;
	}
}

static const struct pw_proxy_events link_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = link_proxy_destroy
};

static int link_nodes(struct impl *impl, struct endpoint_link *link,
		struct sm_node *outnode, struct sm_node *innode)
{
	struct pw_properties *props;
	struct sm_port *outport, *inport;

	pw_log_debug(NAME" %p: linking %d -> %d", impl, outnode->obj.id, innode->obj.id);

	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", outnode->obj.id);
	pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", innode->obj.id);

	for (outport = spa_list_first(&outnode->port_list, struct sm_port, link),
	    inport = spa_list_first(&innode->port_list, struct sm_port, link);
	    !spa_list_is_end(outport, &outnode->port_list, link) &&
	    !spa_list_is_end(inport, &innode->port_list, link);) {

		pw_log_debug(NAME" %p: port %d:%d -> %d:%d", impl,
				outport->direction, outport->obj.id,
				inport->direction, inport->obj.id);

		if (outport->direction == PW_DIRECTION_OUTPUT &&
		    inport->direction == PW_DIRECTION_INPUT) {
			struct link *l;
			struct pw_proxy *p;

			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%d", outport->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%d", inport->obj.id);

			p = pw_core_proxy_create_object(impl->core_proxy,
						"link-factory",
						PW_TYPE_INTERFACE_Link,
						PW_VERSION_LINK_PROXY,
						&props->dict, sizeof(struct link));
			if (p == NULL)
				return -errno;

			l = pw_proxy_get_user_data(p);
			l->proxy = p;
			l->output_node = outnode->obj.id;
			l->output_port = outport->obj.id;
			l->input_node = innode->obj.id;
			l->input_port = inport->obj.id;
			pw_proxy_add_listener(p, &l->listener, &link_proxy_events, l);

			if (link) {
				l->endpoint_link = link;
				spa_list_append(&link->link_list, &l->link);
			}

			outport = spa_list_next(outport, link);
			inport = spa_list_next(inport, link);
		} else {
			if (outport->direction != PW_DIRECTION_OUTPUT)
				outport = spa_list_next(outport, link);
			if (inport->direction != PW_DIRECTION_INPUT)
				inport = spa_list_next(inport, link);
		}
	}
	pw_properties_free(props);

	return 0;
}


int sm_media_session_create_links(struct sm_media_session *sess,
		const struct spa_dict *dict)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct sm_object *obj;
	struct sm_node *outnode = NULL, *innode = NULL;
	struct sm_endpoint *outendpoint = NULL, *inendpoint = NULL;
	struct sm_endpoint_stream *outstream = NULL, *instream = NULL;
	struct endpoint_link *link = NULL;
	const char *str;
	int res;

	sm_media_session_roundtrip(sess);

	/* find output node */
	if ((str = spa_dict_lookup(dict, PW_KEY_LINK_OUTPUT_NODE)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    obj->type == PW_TYPE_INTERFACE_Node) {
		outnode = (struct sm_node*)obj;
	}

	/* find input node */
	if ((str = spa_dict_lookup(dict, PW_KEY_LINK_INPUT_NODE)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    obj->type == PW_TYPE_INTERFACE_Node) {
		innode = (struct sm_node*)obj;
	}

	/* find endpoints and streams */
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_OUTPUT_ENDPOINT)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    obj->type == PW_TYPE_INTERFACE_Endpoint) {
		outendpoint = (struct sm_endpoint*)obj;
	}
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_OUTPUT_STREAM)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    obj->type == PW_TYPE_INTERFACE_EndpointStream) {
		outstream = (struct sm_endpoint_stream*)obj;
	}
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_INPUT_ENDPOINT)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    obj->type == PW_TYPE_INTERFACE_Endpoint) {
		inendpoint = (struct sm_endpoint*)obj;
	}
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_INPUT_STREAM)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    obj->type == PW_TYPE_INTERFACE_EndpointStream) {
		instream = (struct sm_endpoint_stream*)obj;
	}

	if (outendpoint != NULL && inendpoint != NULL) {
		link = calloc(1, sizeof(struct endpoint_link));
		if (link == NULL)
			return -errno;

		link->id = pw_map_insert_new(&impl->endpoint_links, link);
		link->impl = impl;
		spa_list_init(&link->link_list);
		spa_list_append(&impl->endpoint_link_list, &link->link);

		link->info.version = PW_VERSION_ENDPOINT_LINK_INFO;
		link->info.id = link->id;
		link->info.session_id = impl->this.session->obj.id;
		link->info.output_endpoint_id = outendpoint->info->id;
		link->info.output_stream_id = outstream ? outstream->info->id : SPA_ID_INVALID;
		link->info.input_endpoint_id = inendpoint->info->id;
		link->info.input_stream_id = instream ?  instream->info->id : SPA_ID_INVALID;
		link->info.change_mask =
			PW_ENDPOINT_LINK_CHANGE_MASK_STATE |
			PW_ENDPOINT_LINK_CHANGE_MASK_PROPS;
		link->info.state = PW_ENDPOINT_LINK_STATE_ACTIVE;
		link->info.props = (struct spa_dict*) dict;
	}

	/* link the nodes, record the link proxies in the endpoint_link */
	if (outnode != NULL && innode != NULL)
		res = link_nodes(impl, link, outnode, innode);
	else
		res = 0;

	if (link != NULL) {
		/* now create the endpoint link */
		pw_client_session_proxy_link_update(impl->client_session,
				link->id,
				PW_CLIENT_SESSION_UPDATE_INFO,
				0, NULL,
				&link->info);
	}
	return res;
}

/**
 * Session implementation
 */
static int client_session_set_id(void *object, uint32_t id)
{
	struct impl *impl = object;
	struct pw_session_info info;

	impl->session_id = id;

	spa_zero(info);
	info.version = PW_VERSION_SESSION_INFO;
	info.id = id;

	pw_log_debug("got sesssion id:%d", id);

	pw_client_session_proxy_update(impl->client_session,
			PW_CLIENT_SESSION_UPDATE_INFO,
			0, NULL,
			&info);

	/* start monitors */
	sm_metadata_start(&impl->this);
	sm_alsa_midi_start(&impl->this);
	sm_bluez5_monitor_start(&impl->this);
	sm_alsa_monitor_start(&impl->this);
	sm_v4l2_monitor_start(&impl->this);
	sm_stream_monitor_start(&impl->this);
	return 0;
}

static int client_session_set_param(void *object, uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	struct impl *impl = object;
	pw_proxy_error((struct pw_proxy*)impl->client_session,
			-ENOTSUP, "Session:SetParam not supported");
	return -ENOTSUP;
}

static int client_session_link_set_param(void *object, uint32_t link_id, uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	struct impl *impl = object;
	pw_proxy_error((struct pw_proxy*)impl->client_session,
			-ENOTSUP, "Session:LinkSetParam not supported");
	return -ENOTSUP;
}

static int client_session_link_request_state(void *object, uint32_t link_id, uint32_t state)
{
	return -ENOTSUP;
}

static const struct pw_client_session_proxy_events client_session_events = {
	PW_VERSION_CLIENT_SESSION_PROXY_METHODS,
	.set_id = client_session_set_id,
	.set_param = client_session_set_param,
	.link_set_param = client_session_link_set_param,
	.link_request_state = client_session_link_request_state,
};

static int start_session(struct impl *impl)
{
	impl->client_session = pw_core_proxy_create_object(impl->core_proxy,
                                            "client-session",
                                            PW_TYPE_INTERFACE_ClientSession,
                                            PW_VERSION_CLIENT_SESSION_PROXY,
                                            NULL, 0);

	pw_client_session_proxy_add_listener(impl->client_session,
			&impl->client_session_listener,
			&client_session_events,
			impl);

	return 0;
}

static int start_policy(struct impl *impl)
{
	return sm_policy_ep_start(&impl->this);
}

static void core_done(void *data, uint32_t id, int seq)
{
	struct impl *impl = data;
	struct sync *s, *t;
	impl->last_seq = seq;

	spa_list_for_each_safe(s, t, &impl->sync_list, link) {
		if (s->seq == seq) {
			spa_list_remove(&s->link);
			s->callback(s->data);
			free(s);
		}
	}
	if (impl->rescan_seq == seq) {
		pw_log_trace(NAME" %p: rescan %u %d", impl, id, seq);
		sm_media_session_emit_rescan(impl, seq);
	}
}

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done
};

static void on_monitor_state_changed(void *_data, enum pw_remote_state old,
		enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		pw_log_error(NAME" %p: remote error: %s", impl, error);
		pw_main_loop_quit(impl->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		pw_log_info(NAME" %p: connected", impl);
		impl->core_proxy = pw_remote_get_core_proxy(impl->monitor_remote);
		pw_core_proxy_add_listener(impl->core_proxy,
					   &impl->core_listener,
					   &core_events, impl);
		impl->registry_proxy = pw_core_proxy_get_registry(impl->core_proxy,
                                                PW_VERSION_REGISTRY_PROXY, 0);
		pw_registry_proxy_add_listener(impl->registry_proxy,
                                               &impl->registry_listener,
                                               &registry_events, impl);
		start_session(impl);
		break;

	case PW_REMOTE_STATE_UNCONNECTED:
		pw_log_info(NAME" %p: disconnected", impl);
		pw_main_loop_quit(impl->loop);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events monitor_remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_monitor_state_changed,
};

static void on_policy_state_changed(void *_data, enum pw_remote_state old,
		enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		pw_log_error(NAME" %p: remote error: %s", impl, error);
		pw_main_loop_quit(impl->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		pw_log_info(NAME" %p: connected", impl);
		start_policy(impl);
		break;

	case PW_REMOTE_STATE_UNCONNECTED:
		pw_log_info(NAME" %p: disconnected", impl);
		pw_main_loop_quit(impl->loop);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events policy_remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_policy_state_changed,
};

int main(int argc, char *argv[])
{
	struct impl impl = { 0, };
	const struct spa_support *support;
	uint32_t n_support;
	int res;

	pw_init(&argc, &argv);

	impl.loop = pw_main_loop_new(NULL);
	impl.this.loop = pw_main_loop_get_loop(impl.loop);
	impl.this.core = pw_core_new(impl.this.loop, NULL, 0);

	pw_core_add_spa_lib(impl.this.core, "api.bluez5.*", "bluez5/libspa-bluez5");
	pw_core_add_spa_lib(impl.this.core, "api.alsa.*", "alsa/libspa-alsa");
	pw_core_add_spa_lib(impl.this.core, "api.v4l2.*", "v4l2/libspa-v4l2");

	impl.monitor_remote = pw_remote_new(impl.this.core, NULL, 0);
	pw_remote_add_listener(impl.monitor_remote, &impl.monitor_listener, &monitor_remote_events, &impl);

	impl.policy_remote = pw_remote_new(impl.this.core, NULL, 0);
	pw_remote_add_listener(impl.policy_remote, &impl.policy_listener, &policy_remote_events, &impl);

	pw_module_load(impl.this.core, "libpipewire-module-client-device", NULL, NULL);
	pw_module_load(impl.this.core, "libpipewire-module-adapter", NULL, NULL);
	pw_module_load(impl.this.core, "libpipewire-module-metadata", NULL, NULL);
	pw_module_load(impl.this.core, "libpipewire-module-session-manager", NULL, NULL);

	pw_map_init(&impl.globals, 64, 64);
	spa_list_init(&impl.global_list);
	pw_map_init(&impl.endpoint_links, 64, 64);
	spa_list_init(&impl.endpoint_link_list);
	spa_list_init(&impl.sync_list);
	spa_hook_list_init(&impl.hooks);

	support = pw_core_get_support(impl.this.core, &n_support);

	impl.dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	if (impl.dbus)
		impl.this.dbus_connection = spa_dbus_get_connection(impl.dbus, DBUS_BUS_SESSION);
	if (impl.this.dbus_connection == NULL)
		pw_log_warn("no dbus connection");
	else
		pw_log_debug("got dbus connection %p", impl.this.dbus_connection);

	if ((res = pw_remote_connect(impl.monitor_remote)) < 0)
		return res;
	if ((res = pw_remote_connect(impl.policy_remote)) < 0)
		return res;

	pw_main_loop_run(impl.loop);

	pw_core_destroy(impl.this.core);
	pw_main_loop_destroy(impl.loop);

	return 0;
}
