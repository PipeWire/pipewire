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
#include <getopt.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/pod.h>
#include <spa/support/dbus.h>
#include <spa/monitor/device.h>

#include "pipewire/pipewire.h"
#include "extensions/session-manager.h"

#include <dbus/dbus.h>

#include "media-session.h"

#define NAME "media-session"

#define sm_object_emit(o,m,v,...) spa_hook_list_call(&(o)->hooks, struct sm_object_events, m, v, ##__VA_ARGS__)

#define sm_object_emit_update(s)		sm_object_emit(s, update, 0)
#define sm_object_emit_destroy(s)		sm_object_emit(s, destroy, 0)

#define sm_media_session_emit(s,m,v,...) spa_hook_list_call(&(s)->hooks, struct sm_media_session_events, m, v, ##__VA_ARGS__)

#define sm_media_session_emit_info(s,i)			sm_media_session_emit(s, info, 0, i)
#define sm_media_session_emit_create(s,obj)		sm_media_session_emit(s, create, 0, obj)
#define sm_media_session_emit_remove(s,obj)		sm_media_session_emit(s, remove, 0, obj)
#define sm_media_session_emit_rescan(s,seq)		sm_media_session_emit(s, rescan, 0, seq)
#define sm_media_session_emit_destroy(s)		sm_media_session_emit(s, destroy, 0)

int sm_metadata_start(struct sm_media_session *sess);
int sm_alsa_midi_start(struct sm_media_session *sess);
int sm_v4l2_monitor_start(struct sm_media_session *sess);
int sm_bluez5_monitor_start(struct sm_media_session *sess);
int sm_alsa_monitor_start(struct sm_media_session *sess);
int sm_suspend_node_start(struct sm_media_session *sess);

int sm_policy_node_start(struct sm_media_session *sess);

int sm_session_manager_start(struct sm_media_session *sess);

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

	struct pw_main_loop *loop;
	struct spa_dbus *dbus;

	struct pw_core *monitor_core;
	struct spa_hook monitor_listener;
	int monitor_seq;

	struct pw_core *policy_core;
	struct spa_hook policy_listener;
	struct spa_hook proxy_policy_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_map globals;
	struct spa_list global_list;

	struct spa_hook_list hooks;

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

struct object_info {
	const char *type;
	uint32_t version;
	const void *events;
	size_t size;
	int (*init) (void *object);
	void (*destroy) (void *object);
};

static void add_object(struct impl *impl, struct sm_object *obj, uint32_t id)
{
	size_t size = pw_map_get_size(&impl->globals);
	obj->id = id;
	pw_log_debug("add %u %p", obj->id, obj);
	while (obj->id > size)
		pw_map_insert_at(&impl->globals, size++, NULL);
	pw_map_insert_at(&impl->globals, obj->id, obj);
	spa_list_append(&impl->global_list, &obj->link);
	sm_media_session_emit_create(impl, obj);
}

static void remove_object(struct impl *impl, struct sm_object *obj)
{
	pw_log_debug("remove %u %p", obj->id, obj);
	pw_map_insert_at(&impl->globals, obj->id, NULL);
	spa_list_remove(&obj->link);
	sm_media_session_emit_remove(impl, obj);
	obj->id = SPA_ID_INVALID;
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

int sm_object_destroy(struct sm_object *obj)
{
	pw_log_debug(NAME" %p: object %d", obj->session, obj->id);
	if (obj->proxy) {
		pw_proxy_destroy(obj->proxy);
		if (obj->handle == obj->proxy)
			obj->handle = NULL;
		obj->proxy = NULL;
	}
	if (obj->handle) {
		pw_proxy_destroy(obj->handle);
		obj->handle = NULL;
	}
	return 0;
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


static uint32_t clear_params(struct spa_list *param_list, uint32_t id)
{
	struct param *p, *t;
	uint32_t count = 0;

	spa_list_for_each_safe(p, t, param_list, this.link) {
		if (id == SPA_ID_INVALID || p->this.id == id) {
			spa_list_remove(&p->this.link);
			free(p);
			count++;
		}
	}
	return count;
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

	client->obj.avail |= SM_CLIENT_CHANGE_MASK_INFO;
	client->obj.changed |= SM_CLIENT_CHANGE_MASK_INFO;
	sm_object_sync_update(&client->obj);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
};

static void client_destroy(void *object)
{
	struct sm_client *client = object;
	if (client->info)
		pw_client_info_free(client->info);
}

static const struct object_info client_info = {
	.type = PW_TYPE_INTERFACE_Client,
	.version = PW_VERSION_CLIENT,
	.events = &client_events,
	.size = sizeof(struct sm_client),
	.init = NULL,
	.destroy = client_destroy,
};

/**
 * Device
 */
static void device_event_info(void *object, const struct pw_device_info *info)
{
	struct sm_device *device = object;
	struct impl *impl = SPA_CONTAINER_OF(device->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: device %d info", impl, device->obj.id);
	device->info = pw_device_info_update(device->info, info);

	device->obj.avail |= SM_DEVICE_CHANGE_MASK_INFO;
	device->obj.changed |= SM_DEVICE_CHANGE_MASK_INFO;

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		pw_device_enum_params((struct pw_device*)device->obj.proxy,
				1, SPA_PARAM_Profile, 0, UINT32_MAX, NULL);
	}
	sm_object_sync_update(&device->obj);
}

static void device_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct sm_device *device = object;
	struct impl *impl = SPA_CONTAINER_OF(device->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: device %p param %d index:%d", impl, device, id, index);
	device->n_params -= clear_params(&device->param_list, id);

	if (add_param(&device->param_list, id, param) != NULL)
		device->n_params++;

	device->obj.avail |= SM_DEVICE_CHANGE_MASK_PARAMS;
	device->obj.changed |= SM_DEVICE_CHANGE_MASK_PARAMS;
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static int device_init(void *object)
{
	struct sm_device *device = object;
	spa_list_init(&device->node_list);
	spa_list_init(&device->param_list);
	return 0;
}

static void device_destroy(void *object)
{
	struct sm_device *device = object;
	struct sm_node *node;

	spa_list_consume(node, &device->node_list, link) {
		node->device = NULL;
		spa_list_remove(&node->link);
	}
	clear_params(&device->param_list, SPA_ID_INVALID);
	device->n_params = 0;

	if (device->info)
		pw_device_info_free(device->info);
	device->info = NULL;
}

static const struct object_info device_info = {
	.type = PW_TYPE_INTERFACE_Device,
	.version = PW_VERSION_DEVICE,
	.events = &device_events,
	.size = sizeof(struct sm_device),
	.init = device_init,
	.destroy = device_destroy,
};

static const struct object_info spa_device_info = {
	.type = SPA_TYPE_INTERFACE_Device,
	.version = SPA_VERSION_DEVICE,
	.size = sizeof(struct sm_device),
	.init = device_init,
	.destroy = device_destroy,
};

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

	node->obj.avail |= SM_NODE_CHANGE_MASK_INFO;
	node->obj.changed |= SM_NODE_CHANGE_MASK_INFO;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS &&
	    (node->obj.mask & SM_NODE_CHANGE_MASK_PARAMS) &&
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
			pw_node_subscribe_params((struct pw_node*)node->obj.proxy,
					subscribe, n_subscribe);
			node->subscribe = true;
		}
	}
	node->last_id = SPA_ID_INVALID;
	sm_object_sync_update(&node->obj);
}

static void node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct sm_node *node = object;
	struct impl *impl = SPA_CONTAINER_OF(node->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: node %p param %d index:%d", impl, node, id, index);

	if (node->last_id != id) {
		pw_log_debug(NAME" %p: node %p clear param %d", impl, node, id);
		node->n_params -= clear_params(&node->param_list, id);
		node->last_id = id;
	}

	if (add_param(&node->param_list, id, param) != NULL)
		node->n_params++;

	node->obj.avail |= SM_NODE_CHANGE_MASK_PARAMS;
	node->obj.changed |= SM_NODE_CHANGE_MASK_PARAMS;
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static int node_init(void *object)
{
	struct sm_node *node = object;
	struct impl *impl = SPA_CONTAINER_OF(node->obj.session, struct impl, this);
	struct pw_properties *props = node->obj.props;
	const char *str;

	spa_list_init(&node->port_list);
	spa_list_init(&node->param_list);

	if (props) {
		if ((str = pw_properties_get(props, PW_KEY_DEVICE_ID)) != NULL)
			node->device = find_object(impl, atoi(str));
		pw_log_debug(NAME" %p: node %d parent device %s (%p)", impl,
				node->obj.id, str, node->device);
		if (node->device) {
			spa_list_append(&node->device->node_list, &node->link);
			node->device->obj.avail |= SM_DEVICE_CHANGE_MASK_NODES;
			node->device->obj.changed |= SM_DEVICE_CHANGE_MASK_NODES;
		}
	}
	return 0;
}

static void node_destroy(void *object)
{
	struct sm_node *node = object;
	struct sm_port *port;

	spa_list_consume(port, &node->port_list, link) {
		port->node = NULL;
		spa_list_remove(&port->link);
	}
	clear_params(&node->param_list, SPA_ID_INVALID);
	node->n_params = 0;

	if (node->device) {
		spa_list_remove(&node->link);
		node->device->obj.changed |= SM_DEVICE_CHANGE_MASK_NODES;
	}
	if (node->info) {
		pw_node_info_free(node->info);
		node->info = NULL;
	}
}

static const struct object_info node_info = {
	.type = PW_TYPE_INTERFACE_Node,
	.version = PW_VERSION_NODE,
	.events = &node_events,
	.size = sizeof(struct sm_node),
	.init = node_init,
	.destroy = node_destroy,
};

/**
 * Port
 */
static void port_event_info(void *object, const struct pw_port_info *info)
{
	struct sm_port *port = object;
	struct impl *impl = SPA_CONTAINER_OF(port->obj.session, struct impl, this);

	pw_log_debug(NAME" %p: port %d info", impl, port->obj.id);
	port->info = pw_port_info_update(port->info, info);

	port->obj.avail |= SM_PORT_CHANGE_MASK_INFO;
	port->obj.changed |= SM_PORT_CHANGE_MASK_INFO;
	sm_object_sync_update(&port->obj);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_event_info,
};

static int port_init(void *object)
{
	struct sm_port *port = object;
	struct impl *impl = SPA_CONTAINER_OF(port->obj.session, struct impl, this);
	struct pw_properties *props = port->obj.props;
	const char *str;

	if (props) {
		if ((str = pw_properties_get(props, PW_KEY_PORT_DIRECTION)) != NULL)
			port->direction = strcmp(str, "out") == 0 ?
				PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
		if ((str = pw_properties_get(props, PW_KEY_NODE_ID)) != NULL)
			port->node = find_object(impl, atoi(str));

		pw_log_debug(NAME" %p: port %d parent node %s (%p) direction:%d", impl,
				port->obj.id, str, port->node, port->direction);
		if (port->node) {
			spa_list_append(&port->node->port_list, &port->link);
			port->node->obj.avail |= SM_NODE_CHANGE_MASK_PORTS;
			port->node->obj.changed |= SM_NODE_CHANGE_MASK_PORTS;
		}
	}
	return 0;
}

static void port_destroy(void *object)
{
	struct sm_port *port = object;
	if (port->info)
		pw_port_info_free(port->info);
	if (port->node) {
		spa_list_remove(&port->link);
		port->node->obj.changed |= SM_NODE_CHANGE_MASK_PORTS;
	}
}

static const struct object_info port_info = {
	.type = PW_TYPE_INTERFACE_Port,
	.version = PW_VERSION_PORT,
	.events = &port_events,
	.size = sizeof(struct sm_port),
	.init = port_init,
	.destroy = port_destroy,
};

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

	sess->obj.avail |= SM_SESSION_CHANGE_MASK_INFO;
	sess->obj.changed |= SM_SESSION_CHANGE_MASK_INFO;
	sm_object_sync_update(&sess->obj);
}

static const struct pw_session_events session_events = {
	PW_VERSION_SESSION_EVENTS,
	.info = session_event_info,
};

static int session_init(void *object)
{
	struct sm_session *sess = object;
	struct impl *impl = SPA_CONTAINER_OF(sess->obj.session, struct impl, this);

	if (sess->obj.id == impl->this.session_id)
		impl->this.session = sess;

	spa_list_init(&sess->endpoint_list);
	return 0;
}

static void session_destroy(void *object)
{
	struct sm_session *sess = object;
	struct sm_endpoint *endpoint;
	struct pw_session_info *i = sess->info;

	spa_list_consume(endpoint, &sess->endpoint_list, link) {
		endpoint->session = NULL;
		spa_list_remove(&endpoint->link);
	}
	if (i) {
		if (i->props)
			pw_properties_free ((struct pw_properties *)i->props);
		free(i);
	}

}

static const struct object_info session_info = {
	.type = PW_TYPE_INTERFACE_Session,
	.version = PW_VERSION_SESSION,
	.events = &session_events,
	.size = sizeof(struct sm_session),
	.init = session_init,
	.destroy = session_destroy,
};

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

	endpoint->obj.avail |= SM_ENDPOINT_CHANGE_MASK_INFO;
	endpoint->obj.changed |= SM_ENDPOINT_CHANGE_MASK_INFO;
	sm_object_sync_update(&endpoint->obj);
}

static const struct pw_endpoint_events endpoint_events = {
	PW_VERSION_ENDPOINT_EVENTS,
	.info = endpoint_event_info,
};

static int endpoint_init(void *object)
{
	struct sm_endpoint *endpoint = object;
	struct impl *impl = SPA_CONTAINER_OF(endpoint->obj.session, struct impl, this);
	struct pw_properties *props = endpoint->obj.props;
	const char *str;

	if (props) {
		if ((str = pw_properties_get(props, PW_KEY_SESSION_ID)) != NULL)
			endpoint->session = find_object(impl, atoi(str));
		pw_log_debug(NAME" %p: endpoint %d parent session %s", impl,
				endpoint->obj.id, str);
		if (endpoint->session) {
			spa_list_append(&endpoint->session->endpoint_list, &endpoint->link);
			endpoint->session->obj.avail |= SM_SESSION_CHANGE_MASK_ENDPOINTS;
			endpoint->session->obj.changed |= SM_SESSION_CHANGE_MASK_ENDPOINTS;
		}
	}
	spa_list_init(&endpoint->stream_list);

	return 0;
}

static void endpoint_destroy(void *object)
{
	struct sm_endpoint *endpoint = object;
	struct sm_endpoint_stream *stream;
	struct pw_endpoint_info *i = endpoint->info;

	spa_list_consume(stream, &endpoint->stream_list, link) {
		stream->endpoint = NULL;
		spa_list_remove(&stream->link);
	}
	if (endpoint->session) {
		endpoint->session = NULL;
		spa_list_remove(&endpoint->link);
	}
	if (i) {
		if (i->props)
			pw_properties_free ((struct pw_properties *)i->props);
		free(i->name);
		free(i->media_class);
		free(i);
	}
}

static const struct object_info endpoint_info = {
	.type = PW_TYPE_INTERFACE_Endpoint,
	.version = PW_VERSION_ENDPOINT,
	.events = &endpoint_events,
	.size = sizeof(struct sm_endpoint),
	.init = endpoint_init,
	.destroy = endpoint_destroy,
};


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

	stream->obj.avail |= SM_ENDPOINT_CHANGE_MASK_INFO;
	stream->obj.changed |= SM_ENDPOINT_CHANGE_MASK_INFO;
	sm_object_sync_update(&stream->obj);
}

static const struct pw_endpoint_stream_events endpoint_stream_events = {
	PW_VERSION_ENDPOINT_STREAM_EVENTS,
	.info = endpoint_stream_event_info,
};

static int endpoint_stream_init(void *object)
{
	struct sm_endpoint_stream *stream = object;
	struct impl *impl = SPA_CONTAINER_OF(stream->obj.session, struct impl, this);
	struct pw_properties *props = stream->obj.props;
	const char *str;

	if (props) {
		if ((str = pw_properties_get(props, PW_KEY_ENDPOINT_ID)) != NULL)
			stream->endpoint = find_object(impl, atoi(str));
		pw_log_debug(NAME" %p: stream %d parent endpoint %s", impl,
				stream->obj.id, str);
		if (stream->endpoint) {
			spa_list_append(&stream->endpoint->stream_list, &stream->link);
			stream->endpoint->obj.avail |= SM_ENDPOINT_CHANGE_MASK_STREAMS;
			stream->endpoint->obj.changed |= SM_ENDPOINT_CHANGE_MASK_STREAMS;
		}
	}
	spa_list_init(&stream->link_list);

	return 0;
}

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

static const struct object_info endpoint_stream_info = {
	.type = PW_TYPE_INTERFACE_EndpointStream,
	.version = PW_VERSION_ENDPOINT_STREAM,
	.events = &endpoint_stream_events,
	.size = sizeof(struct sm_endpoint_stream),
	.init = endpoint_stream_init,
	.destroy = endpoint_stream_destroy,
};

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

	link->obj.avail |= SM_ENDPOINT_LINK_CHANGE_MASK_INFO;
	link->obj.changed |= SM_ENDPOINT_LINK_CHANGE_MASK_INFO;
	sm_object_sync_update(&link->obj);
}

static const struct pw_endpoint_link_events endpoint_link_events = {
	PW_VERSION_ENDPOINT_LINK_EVENTS,
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

static const struct object_info endpoint_link_info = {
	.type = PW_TYPE_INTERFACE_EndpointLink,
	.version = PW_VERSION_ENDPOINT_LINK,
	.events = &endpoint_link_events,
	.size = sizeof(struct sm_endpoint_link),
	.init = NULL,
	.destroy = endpoint_link_destroy,
};

/**
 * Proxy
 */
static void
destroy_proxy(void *data)
{
	struct sm_object *obj = data;
	struct impl *impl = SPA_CONTAINER_OF(obj->session, struct impl, this);
	struct data *d;

	pw_log_debug("object %p: proxy:%p id:%d", obj, obj->proxy, obj->id);

	spa_hook_remove(&obj->proxy_listener);
	if (SPA_FLAG_IS_SET(obj->mask, SM_OBJECT_CHANGE_MASK_LISTENER))
		spa_hook_remove(&obj->object_listener);

	if (obj->id != SPA_ID_INVALID)
		remove_object(impl, obj);

	sm_object_emit_destroy(obj);

	if (obj->destroy)
		obj->destroy(obj);

	if (obj->props)
		pw_properties_free(obj->props);
	obj->props = NULL;

	spa_list_consume(d, &obj->data, link) {
		spa_list_remove(&d->link);
		free(d);
	}
}

static void done_proxy(void *data, int seq)
{
	struct sm_object *obj = data;

	pw_log_debug("done %p proxy %p avail:%08x update:%08x %d/%d", obj,
			obj->proxy, obj->avail, obj->changed, obj->pending, seq);

	if (obj->pending == seq) {
		obj->pending = SPA_ID_INVALID;
		if (obj->changed)
			sm_object_emit_update(obj);
		obj->changed = 0;
	}
}

static void bound_proxy(void *data, uint32_t id)
{
	struct sm_object *obj = data;
	struct impl *impl = SPA_CONTAINER_OF(obj->session, struct impl, this);

	pw_log_debug("bound %p proxy %p handle %p id:%d->%d",
			obj, obj->proxy, obj->handle, obj->id, id);

	if (obj->id == SPA_ID_INVALID)
		add_object(impl, obj, id);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = destroy_proxy,
	.done = done_proxy,
	.bound = bound_proxy,
};

int sm_object_sync_update(struct sm_object *obj)
{
	obj->pending = pw_proxy_sync(obj->proxy, 1);
	pw_log_debug("sync %p proxy %p %d", obj, obj->proxy, obj->pending);
	return obj->pending;
}

static const struct object_info *get_object_info(struct impl *impl, const char *type)
{
	const struct object_info *info;

	if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0)
		info = &client_info;
	else if (strcmp(type, SPA_TYPE_INTERFACE_Device) == 0)
		info = &spa_device_info;
	else if (strcmp(type, PW_TYPE_INTERFACE_Device) == 0)
		info = &device_info;
	else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
		info = &node_info;
	else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
		info = &port_info;
	else if (strcmp(type, PW_TYPE_INTERFACE_Session) == 0)
		info = &session_info;
	else if (strcmp(type, PW_TYPE_INTERFACE_Endpoint) == 0)
		info = &endpoint_info;
	else if (strcmp(type, PW_TYPE_INTERFACE_EndpointStream) == 0)
		info = &endpoint_stream_info;
	else if (strcmp(type, PW_TYPE_INTERFACE_EndpointLink) == 0)
		info = &endpoint_link_info;
	else
		info = NULL;

	return info;
}

static struct sm_object *init_object(struct impl *impl, const struct object_info *info,
		struct pw_proxy *proxy, struct pw_proxy *handle, uint32_t id,
		const struct spa_dict *props)
{
	struct sm_object *obj;

	obj = pw_proxy_get_user_data(handle);
	obj->session = &impl->this;
	obj->id = id;
	obj->type = info->type;
	obj->props = props ? pw_properties_new_dict(props) : pw_properties_new(NULL, NULL);
	obj->proxy = proxy;
	obj->handle = handle;
	obj->destroy = info->destroy;
	obj->mask |= SM_OBJECT_CHANGE_MASK_PROPERTIES | SM_OBJECT_CHANGE_MASK_BIND;
	obj->avail |= obj->mask;
	spa_hook_list_init(&obj->hooks);
	spa_list_init(&obj->data);

	if (proxy) {
		pw_proxy_add_listener(obj->proxy, &obj->proxy_listener, &proxy_events, obj);
		if (info->events != NULL)
			pw_proxy_add_object_listener(obj->proxy, &obj->object_listener, info->events, obj);
		SPA_FLAG_UPDATE(obj->mask, SM_OBJECT_CHANGE_MASK_LISTENER, info->events != NULL);
	}
	if (handle) {
		pw_proxy_add_listener(obj->handle, &obj->handle_listener, &proxy_events, obj);
	}

	if (info->init)
		info->init(obj);

	if (id != SPA_ID_INVALID)
		add_object(impl, obj, id);

	return obj;
}

static struct sm_object *
create_object(struct impl *impl, struct pw_proxy *proxy, struct pw_proxy *handle,
		const struct spa_dict *props)
{
	const char *type;
	const struct object_info *info;
	struct sm_object *obj;

	type = pw_proxy_get_type(handle, NULL);

	info = get_object_info(impl, type);
	if (info == NULL) {
		pw_log_error(NAME" %p: unknown object type %s", impl, type);
		errno = ENOTSUP;
		return NULL;
	}
	obj = init_object(impl, info, proxy, handle, SPA_ID_INVALID, props);

	pw_log_debug(NAME" %p: created new object %p proxy %p", impl, obj, obj->proxy);

	return obj;
}

static struct sm_object *
bind_object(struct impl *impl, const struct object_info *info, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	int res;
	struct pw_proxy *proxy;
	struct sm_object *obj;

	proxy = pw_registry_bind(impl->registry,
			id, type, info->version, info->size);
	if (proxy == NULL) {
		res = -errno;
		goto error;
	}
	obj = init_object(impl, info, proxy, proxy, id, props);

	pw_log_debug(NAME" %p: bound new object %p proxy %p id:%d", impl, obj, obj->proxy, obj->id);

	return obj;

error:
	pw_log_warn(NAME" %p: can't handle global %d: %s", impl, id, spa_strerror(res));
	errno = -res;
	return NULL;
}

static int
update_object(struct impl *impl, const struct object_info *info,
		struct sm_object *obj, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	pw_properties_update(obj->props, props);

	if (obj->proxy != NULL)
		return 0;

	pw_log_debug(NAME" %p: update type:%s", impl, obj->type);

	obj->proxy = pw_registry_bind(impl->registry,
			id, info->type, info->version, 0);
	if (obj->proxy == NULL)
		return -errno;

	obj->type = info->type;

	pw_proxy_add_listener(obj->proxy, &obj->proxy_listener, &proxy_events, obj);
	if (info->events)
		pw_proxy_add_object_listener(obj->proxy, &obj->object_listener, info->events, obj);

	SPA_FLAG_UPDATE(obj->mask, SM_OBJECT_CHANGE_MASK_LISTENER, info->events != NULL);

	sm_media_session_emit_create(impl, obj);

	return 0;
}

static void
registry_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
        struct sm_object *obj;
	const struct object_info *info;

	pw_log_debug(NAME " %p: new global '%d' %s/%d", impl, id, type, version);

	info = get_object_info(impl, type);
	if (info == NULL)
		return;

	obj = find_object(impl, id);
	if (obj == NULL) {
		bind_object(impl, info, id, permissions, type, version, props);
	} else {
		pw_log_debug(NAME " %p: our object %d appeared %s/%s",
				impl, id, obj->type, type);
		update_object(impl, info, obj, id, permissions, type, version, props);
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
	if (impl->policy_core)
		impl->rescan_seq = pw_core_sync(impl->policy_core, 0, impl->last_seq);
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
	sync->seq = pw_core_sync(impl->policy_core, 0, impl->last_seq);
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

	if (impl->policy_core == NULL)
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

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
        .global = registry_global,
        .global_remove = registry_global_remove,
};

static void monitor_sync(struct impl *impl)
{
	pw_core_set_paused(impl->policy_core, true);
	impl->monitor_seq = pw_core_sync(impl->monitor_core, 0, impl->monitor_seq);
	pw_log_debug(NAME " %p: monitor sync start %d", impl, impl->monitor_seq);
}

struct pw_proxy *sm_media_session_export(struct sm_media_session *sess,
		const char *type, const struct spa_dict *props,
		void *object, size_t user_data_size)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct pw_proxy *handle;

	pw_log_debug(NAME " %p: object %s %p", impl, type, object);

	handle = pw_core_export(impl->monitor_core, type,
			props, object, user_data_size);

	monitor_sync(impl);

	return handle;
}

struct sm_node *sm_media_session_export_node(struct sm_media_session *sess,
		const struct spa_dict *props, struct pw_impl_node *object)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct sm_node *node;
	struct pw_proxy *handle;

	pw_log_debug(NAME " %p: node %p", impl, object);

	handle = pw_core_export(impl->monitor_core, PW_TYPE_INTERFACE_Node,
			props, object, sizeof(struct sm_node));

	node = (struct sm_node *) create_object(impl, NULL, handle, props);

	monitor_sync(impl);

	return node;
}

struct sm_device *sm_media_session_export_device(struct sm_media_session *sess,
		const struct spa_dict *props, struct spa_device *object)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct sm_device *device;
	struct pw_proxy *handle;

	pw_log_debug(NAME " %p: device %p", impl, object);

	handle = pw_core_export(impl->monitor_core, SPA_TYPE_INTERFACE_Device,
			props, object, sizeof(struct sm_device));

	device = (struct sm_device *) create_object(impl, NULL, handle, props);

	monitor_sync(impl);

	return device;
}

struct pw_proxy *sm_media_session_create_object(struct sm_media_session *sess,
		const char *factory_name, const char *type, uint32_t version,
		const struct spa_dict *props, size_t user_data_size)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	return pw_core_create_object(impl->policy_core,
			factory_name, type, version, props, user_data_size);
}

struct sm_node *sm_media_session_create_node(struct sm_media_session *sess,
		const char *factory_name, const struct spa_dict *props)
{
	struct impl *impl = SPA_CONTAINER_OF(sess, struct impl, this);
	struct sm_node *node;
	struct pw_proxy *proxy;

	pw_log_debug(NAME " %p: node '%s'", impl, factory_name);

	proxy = pw_core_create_object(impl->policy_core,
				factory_name,
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE,
				props,
				sizeof(struct sm_node));

	node = (struct sm_node *)create_object(impl, proxy, proxy, props);

	return node;
}

static void check_endpoint_link(struct endpoint_link *link)
{
	if (!spa_list_is_empty(&link->link_list))
		return;

	if (link->impl) {
		spa_list_remove(&link->link);
		pw_map_remove(&link->impl->endpoint_links, link->id);

		pw_client_session_link_update(link->impl->this.client_session,
				link->id,
				PW_CLIENT_SESSION_LINK_UPDATE_DESTROYED,
				0, NULL, NULL);

		link->impl = NULL;
		free(link);
	}
}

static void proxy_link_destroy(void *data)
{
	struct link *l = data;

	if (l->endpoint_link) {
		spa_list_remove(&l->link);
		check_endpoint_link(l->endpoint_link);
		l->endpoint_link = NULL;
	}
}

static const struct pw_proxy_events proxy_link_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = proxy_link_destroy
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

			p = pw_core_create_object(impl->policy_core,
						"link-factory",
						PW_TYPE_INTERFACE_Link,
						PW_VERSION_LINK,
						&props->dict, sizeof(struct link));
			if (p == NULL)
				return -errno;

			l = pw_proxy_get_user_data(p);
			l->proxy = p;
			l->output_node = outnode->obj.id;
			l->output_port = outport->obj.id;
			l->input_node = innode->obj.id;
			l->input_port = inport->obj.id;
			pw_proxy_add_listener(p, &l->listener, &proxy_link_events, l);

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
	    strcmp(obj->type, PW_TYPE_INTERFACE_Node) == 0) {
		outnode = (struct sm_node*)obj;
	}

	/* find input node */
	if ((str = spa_dict_lookup(dict, PW_KEY_LINK_INPUT_NODE)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    strcmp(obj->type, PW_TYPE_INTERFACE_Node) == 0) {
		innode = (struct sm_node*)obj;
	}

	/* find endpoints and streams */
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_OUTPUT_ENDPOINT)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    strcmp(obj->type, PW_TYPE_INTERFACE_Endpoint) == 0) {
		outendpoint = (struct sm_endpoint*)obj;
	}
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_OUTPUT_STREAM)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    strcmp(obj->type, PW_TYPE_INTERFACE_EndpointStream) == 0) {
		outstream = (struct sm_endpoint_stream*)obj;
	}
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_INPUT_ENDPOINT)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    strcmp(obj->type, PW_TYPE_INTERFACE_Endpoint) == 0) {
		inendpoint = (struct sm_endpoint*)obj;
	}
	if ((str = spa_dict_lookup(dict, PW_KEY_ENDPOINT_LINK_INPUT_STREAM)) != NULL &&
	    (obj = find_object(impl, atoi(str))) != NULL &&
	    strcmp(obj->type, PW_TYPE_INTERFACE_EndpointStream) == 0) {
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
		pw_client_session_link_update(impl->this.client_session,
				link->id,
				PW_CLIENT_SESSION_UPDATE_INFO,
				0, NULL,
				&link->info);
	}
	return res;
}

static void monitor_core_done(void *data, uint32_t id, int seq)
{
	struct impl *impl = data;

	if (seq == impl->monitor_seq) {
		pw_log_debug(NAME " %p: monitor sync stop %d", impl, seq);
		pw_core_set_paused(impl->policy_core, false);
	}
}

static const struct pw_core_events monitor_core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = monitor_core_done,
};

static int start_session(struct impl *impl)
{
	impl->monitor_core = pw_context_connect(impl->this.context, NULL, 0);
	if (impl->monitor_core == NULL) {
		pw_log_error("can't start monitor: %m");
		return -errno;
	}

	pw_core_add_listener(impl->monitor_core,
			&impl->monitor_listener,
			&monitor_core_events, impl);

	return 0;
}

static void core_info(void *data, const struct pw_core_info *info)
{
	struct impl *impl = data;
	pw_log_debug(NAME" %p: info", impl);
	impl->this.info = pw_core_info_update(impl->this.info, info);

	if (impl->this.info->change_mask != 0)
		sm_media_session_emit_info(impl, impl->this.info);
	impl->this.info->change_mask = 0;
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
		struct sm_object *obj, *to;

		pw_log_trace(NAME" %p: rescan %u %d", impl, id, seq);
		sm_media_session_emit_rescan(impl, seq);

		spa_list_for_each_safe(obj, to, &impl->global_list, link) {
			pw_log_trace(NAME" %p: obj %p %08x", impl, obj, obj->changed);
			if (obj->changed)
				sm_object_emit_update(obj);
			obj->changed = 0;
		}
	}
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == 0) {
		if (res == -EPIPE)
			pw_main_loop_quit(impl->loop);
	}
}


static const struct pw_core_events policy_core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = core_info,
	.done = core_done,
	.error = core_error
};

static void policy_core_destroy(void *data)
{
	struct impl *impl = data;
	pw_log_debug(NAME" %p: policy core destroy", impl);
	impl->policy_core = NULL;
}

static const struct pw_proxy_events proxy_core_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = policy_core_destroy,
};

static int start_policy(struct impl *impl)
{
	impl->policy_core = pw_context_connect(impl->this.context, NULL, 0);
	if (impl->policy_core == NULL) {
		pw_log_error("can't start policy: %m");
		return -errno;
	}

	pw_core_add_listener(impl->policy_core,
			&impl->policy_listener,
			&policy_core_events, impl);
	pw_proxy_add_listener((struct pw_proxy*)impl->policy_core,
			&impl->proxy_policy_listener,
			&proxy_core_events, impl);

	impl->registry = pw_core_get_registry(impl->policy_core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(impl->registry,
			&impl->registry_listener,
			&registry_events, impl);

	return 0;
}

static void session_shutdown(struct impl *impl)
{
	struct sm_object *obj;

	spa_list_for_each(obj, &impl->global_list, link)
		sm_media_session_emit_remove(impl, obj);

	sm_media_session_emit_destroy(impl);

	if (impl->registry)
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
	if (impl->policy_core)
		pw_core_disconnect(impl->policy_core);
	if (impl->monitor_core)
		pw_core_disconnect(impl->monitor_core);
	if (impl->this.info)
		pw_core_info_free(impl->this.info);
}

#define DEFAULT_ENABLED		"alsa-pcm,alsa-seq,v4l2,bluez5,metadata,suspend-node,policy-node"
#define DEFAULT_DISABLED	""

static const struct {
	const char *name;
	const char *desc;
	int (*start)(struct sm_media_session *sess);

} modules[] = {
	{ "alsa-seq", "alsa seq midi support", sm_alsa_midi_start },
	{ "alsa-pcm", "alsa pcm udev detection", sm_alsa_monitor_start },
	{ "v4l2", "video for linux udev detection", sm_v4l2_monitor_start },
	{ "bluez5", "bluetooth support", sm_bluez5_monitor_start },
	{ "metadata", "export metadata API", sm_metadata_start },
	{ "suspend-node", "suspend inactive nodes", sm_suspend_node_start },
	{ "policy-node", "configure and link nodes", sm_policy_node_start },
};

static int opt_contains(const char *opt, const char *val)
{
	const char *s, *state = NULL;
	size_t len;
	while((s = pw_split_walk(opt, ",", &len, &state)) != NULL) {
		if (strncmp(val, s, len) == 0)
			return 1;
	}
	return 0;
}

static void show_help(const char *name)
{
	size_t i;

        fprintf(stdout, "%s [options]\n"
             "  -h, --help                            Show this help\n"
             "  -v, --version                         Show version\n"
             "  -e, --enabled                         Enabled options (default '%s')\n"
             "  -d, --disabled                        Disabled options (default '%s')\n"
             "  -p, --properties                      Extra properties as 'key=value { key=value }'\n",
	     name, DEFAULT_ENABLED, DEFAULT_DISABLED);

        fprintf(stdout,
             "\noptions:\n");
	for (i = 0; i < SPA_N_ELEMENTS(modules); i++) {
		fprintf(stdout, "\t%-15.15s: %s\n", modules[i].name, modules[i].desc);
	}
}

int main(int argc, char *argv[])
{
	struct impl impl = { 0, };
	const struct spa_support *support;
	uint32_t n_support;
	int res = 0, c;
	const char *opt_enabled = DEFAULT_ENABLED;
	const char *opt_disabled = DEFAULT_DISABLED;
	const char *opt_properties = NULL;
	static const struct option long_options[] = {
		{"help",	0, NULL, 'h'},
		{"version",	0, NULL, 'v'},
		{"enabled",	1, NULL, 'e'},
		{"disabled",	1, NULL, 'd'},
		{"properties",	1, NULL, 'p'},
		{NULL,		0, NULL, 0}
	};
        size_t i;
	const struct spa_dict_item *item;

	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hve:d:p:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0]);
			return 0;
		case 'v':
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'e':
			opt_enabled = optarg;
			break;
		case 'd':
			opt_disabled = optarg;
			break;
		case 'p':
			opt_properties = optarg;
			break;
		default:
			return -1;
		}
	}

	impl.this.props = pw_properties_new_string(opt_properties ? opt_properties : "");
	if (impl.this.props == NULL)
		return -1;

	spa_dict_for_each(item, &impl.this.props->dict) {
		pw_log_info("  '%s' = '%s'", item->key, item->value);
	}

	impl.loop = pw_main_loop_new(NULL);
	if (impl.loop == NULL)
		return -1;
	impl.this.loop = pw_main_loop_get_loop(impl.loop);
	impl.this.context = pw_context_new(impl.this.loop, NULL, 0);
	if (impl.this.context == NULL)
		return -1;

	pw_context_add_spa_lib(impl.this.context, "api.bluez5.*", "bluez5/libspa-bluez5");
	pw_context_add_spa_lib(impl.this.context, "api.alsa.*", "alsa/libspa-alsa");
	pw_context_add_spa_lib(impl.this.context, "api.v4l2.*", "v4l2/libspa-v4l2");

	pw_context_set_object(impl.this.context, SM_TYPE_MEDIA_SESSION, &impl);

	pw_map_init(&impl.globals, 64, 64);
	spa_list_init(&impl.global_list);
	pw_map_init(&impl.endpoint_links, 64, 64);
	spa_list_init(&impl.endpoint_link_list);
	spa_list_init(&impl.sync_list);
	spa_hook_list_init(&impl.hooks);

	support = pw_context_get_support(impl.this.context, &n_support);

	impl.dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	if (impl.dbus)
		impl.this.dbus_connection = spa_dbus_get_connection(impl.dbus, DBUS_BUS_SESSION);
	if (impl.this.dbus_connection == NULL)
		pw_log_warn("no dbus connection");
	else
		pw_log_debug("got dbus connection %p", impl.this.dbus_connection);

	if ((res = start_session(&impl)) < 0)
		goto exit;
	if ((res = start_policy(&impl)) < 0)
		goto exit;

	for (i = 0; i < SPA_N_ELEMENTS(modules); i++) {
		const char *name = modules[i].name;
		if (opt_contains(opt_enabled, name) &&
		    !opt_contains(opt_disabled, name)) {
			pw_log_info("enable: %s", name);
			modules[i].start(&impl.this);
		}
	}

//	sm_session_manager_start(&impl.this);

	pw_main_loop_run(impl.loop);

exit:
	session_shutdown(&impl);

	pw_context_destroy(impl.this.context);
	pw_main_loop_destroy(impl.loop);

	pw_map_clear(&impl.endpoint_links);
	pw_map_clear(&impl.globals);

	return res;
}
