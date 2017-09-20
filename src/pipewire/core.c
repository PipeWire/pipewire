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
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#define spa_debug pw_log_trace

#include <spa/lib/debug.h>
#include <spa/format-utils.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/interfaces.h>
#include <pipewire/protocol.h>
#include <pipewire/core.h>
#include <pipewire/data-loop.h>

#include <spa/graph-scheduler3.h>

/** \cond */
struct resource_data {
	struct spa_hook resource_listener;
};

/** \endcond */

static void registry_bind(void *object, uint32_t id,
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

	if (type != global->type)
		goto wrong_interface;

	pw_log_debug("global %p: bind global id %d, iface %s to %d", global, id,
		     spa_type_map_get_type(core->type.map, type), new_id);

	pw_global_bind(global, client, permissions, version, new_id);

	return;

      no_id:
	pw_log_debug("registry %p: no global with id %u to bind to %u", resource, id, new_id);
	goto exit;
      wrong_interface:
	pw_log_debug("registry %p: global with id %u has no interface %u", resource, id, type);
	goto exit;
      exit:
	/* unmark the new_id the map, the client does not yet know about the failed
	 * bind and will choose the next id, which we would refuse when we don't mark
	 * new_id as 'used and freed' */
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_resource_remove_id(client->core_resource, new_id);
}

static const struct pw_registry_proxy_methods registry_methods = {
	PW_VERSION_REGISTRY_PROXY_METHODS,
	.bind = registry_bind
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

static void core_client_update(void *object, const struct spa_dict *props)
{
	struct pw_resource *resource = object;
	pw_client_update_properties(resource->client, props);
}

static void core_sync(void *object, uint32_t seq)
{
	struct pw_resource *resource = object;

	pw_log_debug("core %p: sync %d from resource %p", resource->core, seq, resource);
	pw_core_resource_done(resource, seq);
}

static void core_get_registry(void *object, uint32_t version, uint32_t new_id)
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
					    this->type.registry,
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

	spa_list_insert(this->registry_resource_list.prev, &registry_resource->link);

	spa_list_for_each(global, &this->global_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, client);
		if (PW_PERM_IS_R(permissions)) {
			pw_registry_resource_global(registry_resource,
						    global->id,
						    global->parent->id,
						    permissions,
						    global->type,
						    global->version);
		}
	}

	return;

      no_mem:
	pw_log_error("can't create registry resource");
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_NO_MEMORY, "no memory");
}

static void
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

	factory = pw_core_find_factory(client->core, factory_name);
	if (factory == NULL)
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
		goto no_mem;

	properties = NULL;

      done:
	return;

      no_factory:
	pw_log_error("can't find node factory %s", factory_name);
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_INVALID_ARGUMENTS, "unknown factory name %s", factory_name);
	goto done;
      wrong_version:
      wrong_type:
	pw_log_error("invalid resource type/version");
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_INCOMPATIBLE_VERSION, "wrong resource type/version");
	goto done;
      no_properties:
	pw_log_error("can't create properties");
	goto no_mem;
      no_mem:
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	goto done;
}

static void
core_create_link(void *object,
		 uint32_t output_node_id,
		 uint32_t output_port_id,
		 uint32_t input_node_id,
		 uint32_t input_port_id,
		 const struct spa_format *filter,
		 const struct spa_dict *props,
		 uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_node *output_node, *input_node;
	struct pw_port *outport, *inport;
	struct pw_core *core = client->core;
	struct pw_global *global;
	struct pw_link *link;
	char *error;
	int res;

	global = pw_core_find_global(core, output_node_id);
	if (global == NULL || global->type != core->type.node)
		goto no_output;

	output_node = global->object;

	global = pw_core_find_global(core, input_node_id);
	if (global == NULL || global->type != core->type.node)
		goto no_input;

	input_node = global->object;

	if (output_port_id == SPA_ID_INVALID)
		outport = pw_node_get_free_port(output_node, SPA_DIRECTION_OUTPUT);
	else
		outport = pw_node_find_port(output_node, SPA_DIRECTION_OUTPUT, output_port_id);
	if (outport == NULL)
		goto no_output_port;

	if (input_port_id == SPA_ID_INVALID)
		inport = pw_node_get_free_port(input_node, SPA_DIRECTION_INPUT);
	else
		inport = pw_node_find_port(input_node, SPA_DIRECTION_INPUT, input_port_id);
	if (inport == NULL)
		goto no_input_port;

	link = pw_link_new(core, outport, inport, NULL, NULL, &error, 0);
	if (link == NULL)
		goto no_link;

	pw_link_register(link, client, pw_client_get_global(client));

	res = pw_global_bind(pw_link_get_global(link), client, PW_PERM_RWX, PW_VERSION_LINK, new_id);
	if (res < 0)
		goto no_bind;

      done:
	return;

      no_output:
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_INVALID_ARGUMENTS, "unknown output node");
	goto done;
      no_input:
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_INVALID_ARGUMENTS, "unknown input node");
	goto done;
      no_output_port:
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_INVALID_ARGUMENTS, "unknown output port");
	goto done;
      no_input_port:
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_INVALID_ARGUMENTS, "unknown input port");
	goto done;
      no_link:
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_ERROR, "can't create link: %s, error");
	free(error);
	goto done;
      no_bind:
	pw_core_resource_error(client->core_resource,
			       resource->id, res, "can't bind link: %d", res);
	goto done;

}

static void core_update_types(void *object, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_resource *resource = object;
	struct pw_core *this = resource->core;
	struct pw_client *client = resource->client;
	int i;

	for (i = 0; i < n_types; i++, first_id++) {
		uint32_t this_id = spa_type_map_get_id(this->type.map, types[i]);
		if (!pw_map_insert_at(&client->types, first_id, PW_MAP_ID_TO_PTR(this_id)))
			pw_log_error("can't add type for client");
	}
}

static const struct pw_core_proxy_methods core_methods = {
	PW_VERSION_CORE_PROXY_METHODS,
	.update_types = core_update_types,
	.sync = core_sync,
	.get_registry = core_get_registry,
	.client_update = core_client_update,
	.create_object = core_create_object,
	.create_link = core_create_link
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
core_bind_func(struct pw_global *global,
	       struct pw_client *client,
	       uint32_t permissions,
	       uint32_t version,
	       uint32_t id)
{
	struct pw_core *this = global->object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &core_resource_events, resource);

	pw_resource_set_implementation(resource, &core_methods, resource);

	spa_list_append(&this->resource_list, &resource->link);

	if (resource->id == 0)
		client->core_resource = resource;

	pw_log_debug("core %p: bound to %d", this, resource->id);

	this->info.change_mask = PW_CORE_CHANGE_MASK_ALL;
	pw_core_resource_info(resource, &this->info);

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create core resource");
	return SPA_RESULT_NO_MEMORY;
}

/** Create a new core object
 *
 * \param main_loop the main loop to use
 * \param properties extra properties for the core, ownership it taken
 * \return a newly allocated core object
 *
 * \memberof pw_core
 */
struct pw_core *pw_core_new(struct pw_loop *main_loop, struct pw_properties *properties)
{
	struct pw_core *this;
	const char *name;

	this = calloc(1, sizeof(struct pw_core));
	if (this == NULL)
		return NULL;

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

	pw_type_init(&this->type);
	pw_map_init(&this->globals, 128, 32);

	spa_graph_init(&this->rt.graph);
	spa_graph_set_callbacks(&this->rt.graph, &spa_graph_impl_default, NULL);

	spa_debug_set_type_map(this->type.map);

	this->support[0] = SPA_SUPPORT_INIT(SPA_TYPE__TypeMap, this->type.map);
	this->support[1] = SPA_SUPPORT_INIT(SPA_TYPE_LOOP__DataLoop, this->data_loop->loop);
	this->support[2] = SPA_SUPPORT_INIT(SPA_TYPE_LOOP__MainLoop, this->main_loop->loop);
	this->support[3] = SPA_SUPPORT_INIT(SPA_TYPE__Log, pw_log_get());
	this->n_support = 4;

	pw_data_loop_start(this->data_loop_impl);

	spa_list_init(&this->protocol_list);
	spa_list_init(&this->remote_list);
	spa_list_init(&this->resource_list);
	spa_list_init(&this->registry_resource_list);
	spa_list_init(&this->global_list);
	spa_list_init(&this->module_list);
	spa_list_init(&this->client_list);
	spa_list_init(&this->node_list);
	spa_list_init(&this->factory_list);
	spa_list_init(&this->link_list);
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
	this->info.version = SPA_STRINGIFY(PW_VERSION_CORE);
	srandom(time(NULL));
	this->info.cookie = random();
	this->info.props = &properties->dict;
	this->info.name = name;

	this->global = pw_core_add_global(this,
					  NULL,
					  NULL,
					  this->type.core,
					  PW_VERSION_CORE,
					  core_bind_func,
					  this);
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
void pw_core_destroy(struct pw_core *core)
{
	struct pw_global *global, *t;
	struct pw_module *module, *tm;

	pw_log_debug("core %p: destroy", core);
	spa_hook_list_call(&core->listener_list, struct pw_core_events, destroy);

	spa_list_for_each_safe(module, tm, &core->module_list, link)
		pw_module_destroy(module);

	spa_list_for_each_safe(global, t, &core->global_list, link)
		pw_global_destroy(global);

	spa_hook_list_call(&core->listener_list, struct pw_core_events, free);

	pw_data_loop_destroy(core->data_loop_impl);

	pw_properties_free(core->properties);

	pw_map_clear(&core->globals);

	pw_log_debug("core %p: free", core);
	free(core);
}

const struct pw_core_info *pw_core_get_info(struct pw_core *core)
{
	return &core->info;
}

struct pw_global *pw_core_get_global(struct pw_core *core)
{
	return core->global;
}

void pw_core_add_listener(struct pw_core *core,
			  struct spa_hook *listener,
			  const struct pw_core_events *events,
			  void *data)
{
	spa_hook_list_append(&core->listener_list, listener, events, data);
}

void pw_core_set_permission_callback(struct pw_core *core,
				     pw_permission_func_t callback,
				     void *data)
{
	core->permission_func = callback;
	core->permission_data = data;
}

struct pw_type *pw_core_get_type(struct pw_core *core)
{
	return &core->type;
}

const struct spa_support *pw_core_get_support(struct pw_core *core, uint32_t *n_support)
{
	*n_support = core->n_support;
	return core->support;
}

struct pw_loop *pw_core_get_main_loop(struct pw_core *core)
{
	return core->main_loop;
}

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
void pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	uint32_t i;

	for (i = 0; i < dict->n_items; i++)
		pw_properties_set(core->properties, dict->items[i].key, dict->items[i].value);

	core->info.change_mask = PW_CORE_CHANGE_MASK_PROPS;
	core->info.props = &core->properties->dict;

	spa_hook_list_call(&core->listener_list, struct pw_core_events, info_changed, &core->info);

	spa_list_for_each(resource, &core->resource_list, link)
		pw_core_resource_info(resource, &core->info);

	core->info.change_mask = 0;
}

bool pw_core_for_each_global(struct pw_core *core,
			     bool (*callback) (void *data, struct pw_global *global),
			     void *data)
{
	struct pw_global *g, *t;

	spa_list_for_each_safe(g, t, &core->global_list, link)
		if (!callback(data, g))
			return false;
	return true;
}

struct pw_global *pw_core_find_global(struct pw_core *core, uint32_t id)
{
	return pw_map_lookup(&core->globals, id);
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
				  struct spa_format **format_filters,
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

		pw_log_debug("node id \"%d\"", n->global->id);

		if (have_id) {
			if (n->global->id == id) {
				pw_log_debug("id \"%u\" matches node %p", id, n);

				best =
				    pw_node_get_free_port(n,
							  pw_direction_reverse(other_port->
									       direction));
				if (best)
					break;
			}
		} else {
			struct pw_port *p, *pin, *pout;

			p = pw_node_get_free_port(n, pw_direction_reverse(other_port->direction));
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
						n_format_filters, format_filters, error) == NULL) {
				free(*error);
				continue;
			}

			best = p;
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
struct spa_format *pw_core_find_format(struct pw_core *core,
				       struct pw_port *output,
				       struct pw_port *input,
				       struct pw_properties *props,
				       uint32_t n_format_filters,
				       struct spa_format **format_filters,
				       char **error)
{
	uint32_t out_state, in_state;
	int res;
	struct spa_format *filter = NULL, *format;
	uint32_t iidx = 0, oidx = 0;

	out_state = output->state;
	in_state = input->state;

	pw_log_debug("core %p: finding best format %d %d", core, out_state, in_state);

	/* when a port is configured but the node is idle, we can reconfigure with a different format */
	if (out_state > PW_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE)
		out_state = PW_PORT_STATE_CONFIGURE;
	if (in_state > PW_PORT_STATE_CONFIGURE && input->node->info.state == PW_NODE_STATE_IDLE)
		in_state = PW_PORT_STATE_CONFIGURE;

	if (in_state == PW_PORT_STATE_CONFIGURE && out_state > PW_PORT_STATE_CONFIGURE) {
		/* only input needs format */
		if ((res = spa_node_port_get_format(output->node->node, output->direction, output->port_id,
						    (const struct spa_format **) &format)) < 0) {
			asprintf(error, "error get output format: %d", res);
			goto error;
		}
	} else if (out_state == PW_PORT_STATE_CONFIGURE && in_state > PW_PORT_STATE_CONFIGURE) {
		/* only output needs format */
		if ((res = spa_node_port_get_format(input->node->node, input->direction, input->port_id,
						    (const struct spa_format **) &format)) < 0) {
			asprintf(error, "error get input format: %d", res);
			goto error;
		}
	} else if (in_state == PW_PORT_STATE_CONFIGURE && out_state == PW_PORT_STATE_CONFIGURE) {
	      again:
		/* both ports need a format */
		pw_log_debug("core %p: do enum input %d", core, iidx);
		if ((res = spa_node_port_enum_formats(input->node->node, input->direction, input->port_id,
						      &filter, NULL, iidx)) < 0) {
			if (res == SPA_RESULT_ENUM_END && iidx != 0) {
				asprintf(error, "error input enum formats: %d", res);
				goto error;
			}
		}
		pw_log_debug("enum output %d with filter: %p", oidx, filter);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(filter);

		if ((res = spa_node_port_enum_formats(output->node->node, output->direction, output->port_id,
						      &format, filter, oidx)) < 0) {
			if (res == SPA_RESULT_ENUM_END) {
				oidx = 0;
				iidx++;
				goto again;
			}
			asprintf(error, "error output enum formats: %d", res);
			goto error;
		}
		pw_log_debug("Got filtered:");
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(format);

		spa_format_fixate(format);
	} else {
		asprintf(error, "error node state");
		goto error;
	}
	if (format == NULL) {
		asprintf(error, "error get format");
		goto error;
	}
	return format;

      error:
	return NULL;
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
struct pw_factory *pw_core_find_factory(struct pw_core *core, const char *name)
{
	struct pw_factory *factory;

	spa_list_for_each(factory, &core->factory_list, link) {
		if (strcmp(factory->info.name, name) == 0)
			return factory;
	}
	return NULL;
}
