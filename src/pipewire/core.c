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
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#include <pipewire/log.h>

#include <spa/support/dbus.h>
#include <spa/node/utils.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/interfaces.h>
#include <pipewire/protocol.h>
#include <pipewire/core.h>
#include <pipewire/data-loop.h>
#include <pipewire/device.h>
#include <pipewire/map.h>
#include <pipewire/type.h>
#include <pipewire/module.h>
#include <pipewire/version.h>

/** \cond */
struct impl {
	struct pw_core this;
	void *dbus_iface;
};


struct resource_data {
	struct spa_hook resource_listener;
};

/** \endcond */

static int registry_bind(void *object, uint32_t id,
		uint32_t type, uint32_t version, uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_core *core = resource->core;
	struct pw_global *global;
	uint32_t permissions;

	if ((global = pw_core_find_global(core, id)) == NULL)
		goto no_id;

	permissions = pw_global_get_permissions(global, client);

	if (!PW_PERM_IS_R(permissions))
		goto no_id;

	if (global->type != type)
		goto wrong_interface;

	pw_log_debug("global %p: bind global id %d, iface %s/%d to %d", global, id,
		     spa_debug_type_find_name(pw_type_info(), type), version, new_id);

	if (pw_global_bind(global, client, permissions, version, new_id) < 0)
		goto exit;

	return 0;

      no_id:
	pw_log_debug("registry %p: no global with id %u to bind to %u", resource, id, new_id);
	pw_resource_error(resource, -ENOENT, "no such global %u", id);
	goto exit;
      wrong_interface:
	pw_log_debug("registry %p: global with id %u has no interface %u", resource, id, type);
	pw_resource_error(resource, -ENOENT, "no such interface %u", type);
	goto exit;
      exit:
	/* unmark the new_id the map, the client does not yet know about the failed
	 * bind and will choose the next id, which we would refuse when we don't mark
	 * new_id as 'used and freed' */
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_resource_remove_id(client->core_resource, new_id);
	return -EFAULT;
}

static int registry_destroy(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_core *core = resource->core;
	struct pw_global *global;
	uint32_t permissions;

	if ((global = pw_core_find_global(core, id)) == NULL)
		goto no_id;

	permissions = pw_global_get_permissions(global, client);

	if (!PW_PERM_IS_X(permissions))
		goto no_id;

	pw_log_debug("global %p: destroy global id %d", global, id);

	pw_global_destroy(global);
	return 0;

      no_id:
	pw_log_debug("registry %p: no global with id %u to destroy", resource, id);
	goto exit;
      exit:
	return -EFAULT;
}

static const struct pw_registry_proxy_methods registry_methods = {
	PW_VERSION_REGISTRY_PROXY_METHODS,
	.bind = registry_bind,
	.destroy = registry_destroy
};

static void destroy_registry_resource(void *object)
{
	struct pw_resource *resource = object;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = destroy_registry_resource
};

static int destroy_resource(void *object, void *data)
{
	struct pw_resource *resource = object;
	if (resource && resource != resource->client->core_resource) {
		resource->removed = true;
		pw_resource_destroy(resource);
	}
	return 0;
}

static int core_hello(void *object, uint32_t version)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_core *this = resource->core;

	pw_log_debug("core %p: hello %d from resource %p", this, version, resource);
	this->info.change_mask = PW_CORE_CHANGE_MASK_ALL;
	pw_map_for_each(&client->objects, destroy_resource, client);
	pw_core_resource_info(resource, &this->info);
	return 0;
}

static int core_sync(void *object, uint32_t id, int seq)
{
	struct pw_resource *resource = object;
	pw_log_debug("core %p: sync %d for resource %d", resource->core, seq, id);
	pw_core_resource_done(resource, id, seq);
	return 0;
}

static int core_pong(void *object, uint32_t id, int seq)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_resource *r;

	pw_log_debug("core %p: pong %d for resource %d", resource->core, seq, id);

	if ((r = pw_client_find_resource(client, id)) == NULL)
		return -EINVAL;

	pw_resource_emit_pong(r, seq);
	return 0;
}

static int core_error(void *object, uint32_t id, int seq, int res, const char *message)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_resource *r;

	pw_log_debug("core %p: error %d for resource %d: %s", resource->core, res, id, message);

	if ((r = pw_client_find_resource(client, id)) == NULL)
		return -EINVAL;

	pw_resource_emit_error(r, seq, res, message);
	return 0;
}

static int core_get_registry(void *object, uint32_t version, uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_core *this = resource->core;
	struct pw_global *global;
	struct pw_resource *registry_resource;
	struct resource_data *data;

	registry_resource = pw_resource_new(client,
					    new_id,
					    PW_PERM_RWX,
					    PW_TYPE_INTERFACE_Registry,
					    version,
					    sizeof(*data));
	if (registry_resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(registry_resource);
	pw_resource_add_listener(registry_resource,
				 &data->resource_listener,
				 &resource_events,
				 registry_resource);

	pw_resource_set_implementation(registry_resource,
				       &registry_methods,
				       registry_resource);

	spa_list_append(&this->registry_resource_list, &registry_resource->link);

	spa_list_for_each(global, &this->global_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, client);
		if (PW_PERM_IS_R(permissions)) {
			pw_registry_resource_global(registry_resource,
						    global->id,
						    global->parent->id,
						    permissions,
						    global->type,
						    global->version,
						    global->properties ?
						        &global->properties->dict : NULL);
		}
	}

	return 0;

      no_mem:
	pw_log_error("can't create registry resource");
	pw_core_resource_error(client->core_resource, new_id,
			client->recv_seq, -ENOMEM, "no memory");
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_resource_remove_id(client->core_resource, new_id);
	return -ENOMEM;
}

static int
core_create_object(void *object,
		   const char *factory_name,
		   uint32_t type,
		   uint32_t version,
		   const struct spa_dict *props,
		   uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_factory *factory;
	void *obj;
	struct pw_properties *properties;
	int res;

	factory = pw_core_find_factory(client->core, factory_name);
	if (factory == NULL || factory->global == NULL)
		goto no_factory;

	if (!PW_PERM_IS_R(pw_global_get_permissions(factory->global, client)))
		goto no_factory;

	if (factory->info.type != type)
		goto wrong_type;

	if (factory->info.version < version)
		goto wrong_version;

	if (props) {
		properties = pw_properties_new_dict(props);
		if (properties == NULL)
			goto no_properties;
	} else
		properties = NULL;

	/* error will be posted */
	obj = pw_factory_create_object(factory, resource, type, version, properties, new_id);
	if (obj == NULL)
		goto create_failed;

	return 0;

      no_factory:
	res = -EINVAL;
	pw_log_error("can't find node factory %s", factory_name);
	pw_resource_error(resource, res, "unknown factory name %s", factory_name);
	goto error;
      wrong_version:
      wrong_type:
	res = -EPROTO;
	pw_log_error("invalid resource type/version");
	pw_resource_error(resource, res, "wrong resource type/version");
	goto error;
      no_properties:
	res = -errno;
	pw_log_error("can't create properties");
	pw_resource_error(resource, res, "no memory");
      create_failed:
	res = -errno;
      error:
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_resource_remove_id(client->core_resource, new_id);
	return res;
}

static int core_destroy(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_resource *r;

	pw_log_debug("core %p: destroy resource %d from client %p", resource->core, id, client);

	if ((r = pw_client_find_resource(client, id)) == NULL)
		goto no_resource;

	pw_resource_destroy(r);
	return 0;

      no_resource:
	pw_log_error("can't find resouce %d", id);
	pw_resource_error(resource, -EINVAL, "unknown resource %d", id);
	return -EINVAL;
}

static const struct pw_core_proxy_methods core_methods = {
	PW_VERSION_CORE_PROXY_METHODS,
	.hello = core_hello,
	.sync = core_sync,
	.pong = core_pong,
	.error = core_error,
	.get_registry = core_get_registry,
	.create_object = core_create_object,
	.destroy = core_destroy,
};

static void core_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	resource->client->core_resource = NULL;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events core_resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = core_unbind_func,
};

static int
global_bind(void *_data,
	    struct pw_client *client,
	    uint32_t permissions,
	    uint32_t version,
	    uint32_t id)
{
	struct pw_core *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &core_resource_events, resource);

	pw_resource_set_implementation(resource, &core_methods, resource);

	spa_list_append(&global->resource_list, &resource->link);

	if (resource->id == 0)
		client->core_resource = resource;

	pw_log_debug("core %p: bound to %d", this, resource->id);

	return 0;

      no_mem:
	pw_log_error("can't create core resource");
	return -ENOMEM;
}

static void global_destroy(void *object)
{
	struct pw_core *core = object;
	spa_hook_remove(&core->global_listener);
	core->global = NULL;
	pw_core_destroy(core);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

/** Create a new core object
 *
 * \param main_loop the main loop to use
 * \param properties extra properties for the core, ownership it taken
 * \return a newly allocated core object
 *
 * \memberof pw_core
 */
SPA_EXPORT
struct pw_core *pw_core_new(struct pw_loop *main_loop,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_core *this;
	const char *name;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	pw_log_debug("core %p: new", this);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	this->properties = properties;

	this->data_loop_impl = pw_data_loop_new(properties);
	if (this->data_loop_impl == NULL)
		goto no_data_loop;

	this->data_loop = pw_data_loop_get_loop(this->data_loop_impl);
	this->main_loop = main_loop;

	pw_map_init(&this->globals, 128, 32);

	impl->dbus_iface = pw_load_spa_dbus_interface(this->main_loop);

	this->support[0] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, this->data_loop->loop);
	this->support[1] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_MainLoop, this->main_loop->loop);
	this->support[2] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_LoopUtils, this->main_loop->utils);
	this->support[3] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, pw_log_get());
	this->support[4] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DBus, impl->dbus_iface);
	this->support[5] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_CPU,
			pw_get_support_interface(SPA_TYPE_INTERFACE_CPU));
	this->n_support = 6;

	pw_data_loop_start(this->data_loop_impl);

	spa_list_init(&this->protocol_list);
	spa_list_init(&this->remote_list);
	spa_list_init(&this->registry_resource_list);
	spa_list_init(&this->global_list);
	spa_list_init(&this->module_list);
	spa_list_init(&this->device_list);
	spa_list_init(&this->client_list);
	spa_list_init(&this->node_list);
	spa_list_init(&this->factory_list);
	spa_list_init(&this->link_list);
	spa_list_init(&this->control_list[0]);
	spa_list_init(&this->control_list[1]);
	spa_list_init(&this->export_list);
	spa_list_init(&this->driver_list);
	spa_hook_list_init(&this->listener_list);

	if ((name = pw_properties_get(properties, PW_CORE_PROP_NAME)) == NULL) {
		pw_properties_setf(properties,
				   PW_CORE_PROP_NAME, "pipewire-%s-%d",
				   pw_get_user_name(), getpid());
		name = pw_properties_get(properties, PW_CORE_PROP_NAME);
	}

	this->info.change_mask = 0;
	this->info.user_name = pw_get_user_name();
	this->info.host_name = pw_get_host_name();
	this->info.version = pw_get_library_version();
	srandom(time(NULL));
	this->info.cookie = random();
	this->info.props = &properties->dict;
	this->info.name = name;

	this->sc_pagesize = sysconf(_SC_PAGESIZE);

	this->global = pw_global_new(this,
				     PW_TYPE_INTERFACE_Core,
				     PW_VERSION_CORE,
				     pw_properties_new(
					     PW_CORE_PROP_USER_NAME, this->info.user_name,
					     PW_CORE_PROP_HOST_NAME, this->info.host_name,
					     PW_CORE_PROP_NAME, this->info.name,
					     PW_CORE_PROP_VERSION, this->info.version,
					     NULL),
				     global_bind,
				     this);
	if (this->global == NULL)
		goto no_mem;

	pw_global_add_listener(this->global, &this->global_listener, &global_events, this);
	pw_global_register(this->global, NULL, NULL);
	this->info.id = this->global->id;

	return this;

      no_mem:
      no_data_loop:
	free(this);
	return NULL;
}

/** Destroy a core object
 *
 * \param core a core to destroy
 *
 * \memberof pw_core
 */
SPA_EXPORT
void pw_core_destroy(struct pw_core *core)
{
	struct impl *impl = SPA_CONTAINER_OF(core, struct impl, this);
	struct pw_global *global;
	struct pw_module *module;
	struct pw_device *device;
	struct pw_remote *remote;
	struct pw_node *node;

	pw_log_debug("core %p: destroy", core);
	pw_core_emit_destroy(core);

	spa_hook_remove(&core->global_listener);

	spa_list_consume(remote, &core->remote_list, link)
		pw_remote_destroy(remote);

	spa_list_consume(module, &core->module_list, link)
		pw_module_destroy(module);

	spa_list_consume(device, &core->device_list, link)
		pw_device_destroy(device);

	spa_list_consume(node, &core->node_list, link)
		pw_node_destroy(node);

	spa_list_consume(global, &core->global_list, link)
		pw_global_destroy(global);

	pw_core_emit_free(core);

	pw_data_loop_destroy(core->data_loop_impl);

	pw_properties_free(core->properties);

	pw_map_clear(&core->globals);

	pw_unload_spa_interface(impl->dbus_iface);

	pw_log_debug("core %p: free", core);
	free(core);
}

SPA_EXPORT
void *pw_core_get_user_data(struct pw_core *core)
{
	return core->user_data;
}

SPA_EXPORT
const struct pw_core_info *pw_core_get_info(struct pw_core *core)
{
	return &core->info;
}

SPA_EXPORT
struct pw_global *pw_core_get_global(struct pw_core *core)
{
	return core->global;
}

SPA_EXPORT
void pw_core_add_listener(struct pw_core *core,
			  struct spa_hook *listener,
			  const struct pw_core_events *events,
			  void *data)
{
	spa_hook_list_append(&core->listener_list, listener, events, data);
}

SPA_EXPORT
const struct spa_support *pw_core_get_support(struct pw_core *core, uint32_t *n_support)
{
	*n_support = core->n_support;
	return core->support;
}

SPA_EXPORT
struct pw_loop *pw_core_get_main_loop(struct pw_core *core)
{
	return core->main_loop;
}

SPA_EXPORT
const struct pw_properties *pw_core_get_properties(struct pw_core *core)
{
	return core->properties;
}

/** Update core properties
 *
 * \param core a core
 * \param dict properties to update
 *
 * Update the core object with the given properties
 *
 * \memberof pw_core
 */
SPA_EXPORT
int pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	int changed;

	changed = pw_properties_update(core->properties, dict);

	pw_log_debug("core %p: updated %d properties", core, changed);

	if (!changed)
		return 0;

	core->info.change_mask = PW_CORE_CHANGE_MASK_PROPS;
	core->info.props = &core->properties->dict;

	pw_core_emit_info_changed(core, &core->info);

	if (core->global)
		spa_list_for_each(resource, &core->global->resource_list, link)
			pw_core_resource_info(resource, &core->info);

	core->info.change_mask = 0;

	return changed;
}

SPA_EXPORT
int pw_core_for_each_global(struct pw_core *core,
			    int (*callback) (void *data, struct pw_global *global),
			    void *data)
{
	struct pw_global *g, *t;
	int res;

	spa_list_for_each_safe(g, t, &core->global_list, link) {
		if (core->current_client &&
		    !PW_PERM_IS_R(pw_global_get_permissions(g, core->current_client)))
			continue;
		if ((res = callback(data, g)) != 0)
			return res;
	}
	return 0;
}

SPA_EXPORT
struct pw_global *pw_core_find_global(struct pw_core *core, uint32_t id)
{
	struct pw_global *global;

	global = pw_map_lookup(&core->globals, id);
	if (global == NULL)
		return NULL;

	if (core->current_client &&
	    !PW_PERM_IS_R(pw_global_get_permissions(global, core->current_client)))
		return NULL;

	return global;
}

/** Find a port to link with
 *
 * \param core a core
 * \param other_port a port to find a link with
 * \param id the id of a port or SPA_ID_INVALID
 * \param props extra properties
 * \param n_format_filters number of filters
 * \param format_filters array of format filters
 * \param[out] error an error when something is wrong
 * \return a port that can be used to link to \a otherport or NULL on error
 *
 * \memberof pw_core
 */
struct pw_port *pw_core_find_port(struct pw_core *core,
				  struct pw_port *other_port,
				  uint32_t id,
				  struct pw_properties *props,
				  uint32_t n_format_filters,
				  struct spa_pod **format_filters,
				  char **error)
{
	struct pw_port *best = NULL;
	bool have_id;
	struct pw_node *n;

	have_id = id != SPA_ID_INVALID;

	pw_log_debug("id \"%u\", %d", id, have_id);

	spa_list_for_each(n, &core->node_list, link) {
		if (n->global == NULL)
			continue;

		if (other_port->node == n)
			continue;

		if (core->current_client &&
		    !PW_PERM_IS_R(pw_global_get_permissions(n->global, core->current_client)))
			continue;

		pw_log_debug("node id \"%d\"", n->global->id);

		if (have_id) {
			if (n->global->id == id) {
				pw_log_debug("id \"%u\" matches node %p", id, n);

				best =
				    pw_node_find_port(n,
						pw_direction_reverse(other_port->direction),
						SPA_ID_INVALID);
				if (best)
					break;
			}
		} else {
			struct pw_port *p, *pin, *pout;
			uint8_t buf[4096];
			struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
			struct spa_pod *dummy;

			p = pw_node_find_port(n,
					pw_direction_reverse(other_port->direction),
					SPA_ID_INVALID);
			if (p == NULL)
				continue;

			if (p->direction == PW_DIRECTION_OUTPUT) {
				pin = other_port;
				pout = p;
			} else {
				pin = p;
				pout = other_port;
			}

			if (pw_core_find_format(core,
						pout,
						pin,
						props,
						n_format_filters,
						format_filters,
						&dummy,
						&b,
						error) < 0) {
				free(*error);
				continue;
			}
			best = p;
			break;
		}
	}
	if (best == NULL) {
		asprintf(error, "No matching Node found");
	}
	return best;
}

/** Find a common format between two ports
 *
 * \param core a core object
 * \param output an output port
 * \param input an input port
 * \param props extra properties
 * \param n_format_filters number of format filters
 * \param format_filters array of format filters
 * \param[out] error an error when something is wrong
 * \return a common format of NULL on error
 *
 * Find a common format between the given ports. The format will
 * be restricted to a subset given with the format filters.
 *
 * \memberof pw_core
 */
int pw_core_find_format(struct pw_core *core,
			struct pw_port *output,
			struct pw_port *input,
			struct pw_properties *props,
			uint32_t n_format_filters,
			struct spa_pod **format_filters,
			struct spa_pod **format,
			struct spa_pod_builder *builder,
			char **error)
{
	uint32_t out_state, in_state;
	int res;
	uint32_t iidx = 0, oidx = 0;

	out_state = output->state;
	in_state = input->state;

	pw_log_debug("core %p: finding best format %d %d", core, out_state, in_state);

	/* when a port is configured but the node is idle, we can reconfigure with a different format */
	if (out_state > PW_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE)
		out_state = PW_PORT_STATE_CONFIGURE;
	if (in_state > PW_PORT_STATE_CONFIGURE && input->node->info.state == PW_NODE_STATE_IDLE)
		in_state = PW_PORT_STATE_CONFIGURE;

	pw_log_debug("core %p: states %d %d", core, out_state, in_state);

	if (in_state == PW_PORT_STATE_CONFIGURE && out_state > PW_PORT_STATE_CONFIGURE) {
		/* only input needs format */
		if ((res = spa_node_port_enum_params_sync(output->node->node,
						     output->direction, output->port_id,
						     SPA_PARAM_Format, &oidx,
						     NULL, format, builder)) != 1) {
			if (res == 0)
				res = -EBADF;
			asprintf(error, "error get output format: %s", spa_strerror(res));
			goto error;
		}
		pw_log_debug("Got output format:");
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, *format);
	} else if (out_state >= PW_PORT_STATE_CONFIGURE && in_state > PW_PORT_STATE_CONFIGURE) {
		/* only output needs format */
		if ((res = spa_node_port_enum_params_sync(input->node->node,
						     input->direction, input->port_id,
						     SPA_PARAM_Format, &iidx,
						     NULL, format, builder)) != 1) {
			if (res == 0)
				res = -EBADF;
			asprintf(error, "error get input format: %s", spa_strerror(res));
			goto error;
		}
		pw_log_debug("Got input format:");
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, *format);
	} else if (in_state == PW_PORT_STATE_CONFIGURE && out_state == PW_PORT_STATE_CONFIGURE) {
		struct spa_pod_builder fb = { 0 };
		uint8_t fbuf[4096];
		struct spa_pod *filter;
	      again:
		/* both ports need a format */
		pw_log_debug("core %p: do enum input %d", core, iidx);
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(input->node->node,
						     input->direction, input->port_id,
						     SPA_PARAM_EnumFormat, &iidx,
						     NULL, &filter, &fb)) != 1) {
			if (res == 0 && iidx == 0) {
				asprintf(error, "error input enum formats: %s", spa_strerror(res));
				goto error;
			}
			asprintf(error, "no more input formats");
			goto error;
		}
		pw_log_debug("enum output %d with filter: %p", oidx, filter);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, filter);

		if ((res = spa_node_port_enum_params_sync(output->node->node,
						     output->direction, output->port_id,
						     SPA_PARAM_EnumFormat, &oidx,
						     filter, format, builder)) != 1) {
			if (res == 0) {
				oidx = 0;
				goto again;
			}
			asprintf(error, "error output enum formats: %d", res);
			goto error;
		}

		pw_log_debug("Got filtered:");
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, *format);
	} else {
		res = -EBADF;
		asprintf(error, "error node state");
		goto error;
	}
	return res;

      error:
	if (res == 0)
		res = -EBADF;
	return res;
}

/** Find a factory by name
 *
 * \param core the core object
 * \param name the name of the factory to find
 *
 * Find in the list of factories registered in \a core for one with
 * the given \a name.
 *
 * \memberof pw_core
 */
SPA_EXPORT
struct pw_factory *pw_core_find_factory(struct pw_core *core,
					const char *name)
{
	struct pw_factory *factory;

	spa_list_for_each(factory, &core->factory_list, link) {
		if (strcmp(factory->info.name, name) == 0)
			return factory;
	}
	return NULL;
}

static int collect_nodes(struct pw_node *driver)
{
	struct spa_list queue;
	struct pw_node *n, *t;
	struct pw_port *p;
	struct pw_link *l;
	uint32_t quantum = DEFAULT_QUANTUM;

	spa_list_init(&driver->slave_list);

	pw_log_info("driver %p: '%s'", driver, driver->info.name);
	spa_list_init(&queue);
	spa_list_append(&queue, &driver->sort_link);
	driver->visited = true;

	spa_list_consume(n, &queue, sort_link) {
		spa_list_remove(&n->sort_link);
		spa_list_append(&driver->slave_list, &n->slave_link);

		pw_node_set_driver(n, driver);

		if (n->quantum_size > 0 && n->quantum_size < quantum)
			quantum = n->quantum_size;

		spa_list_for_each(p, &n->input_ports, link) {
			spa_list_for_each(l, &p->links, input_link) {
				t = l->output->node;
				if (!t->visited && t->active) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
		spa_list_for_each(p, &n->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				t = l->input->node;
				if (!t->visited && t->active) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
	}
	quantum = SPA_MAX(quantum, MIN_QUANTUM);

	if (driver->rt.position && quantum != driver->rt.position->size) {
		driver->rt.position->size = quantum;
	}

	return 0;
}

int pw_core_recalc_graph(struct pw_core *core)
{
	struct pw_node *n, *s;

	spa_list_for_each(n, &core->driver_list, driver_link) {
		if (!n->visited)
			collect_nodes(n);
	}

	spa_list_for_each(n, &core->node_list, link) {
		if (!n->visited) {
			pw_log_info("unassigned node %p: '%s' %d", n, n->info.name, n->active);
			pw_node_set_driver(n, NULL);
		}
		n->visited = false;
	}

	spa_list_for_each(n, &core->driver_list, driver_link) {
		if (!n->master)
			continue;
		pw_log_info("master %p: quantum:%d '%s'", n, n->rt.position->size, n->info.name);
		spa_list_for_each(s, &n->slave_list, slave_link)
			pw_log_info("slave %p: active:%d '%s'", s, s->active, s->info.name);
	}
	return 0;
}
