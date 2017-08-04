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

#include <spa/lib/debug.h>
#include <spa/format-utils.h>

#include <pipewire/pipewire.h>
#include <pipewire/callback.h>
#include <pipewire/private.h>
#include <pipewire/interfaces.h>
#include <pipewire/protocol.h>
#include <pipewire/core.h>
#include <pipewire/data-loop.h>

/** \cond */
struct global_impl {
	struct pw_global this;
};

struct resource_data {
	struct pw_callback_info resource_callbacks;
};

/** \endcond */

static inline uint32_t pw_global_permissions(struct pw_global *global,
					     struct pw_client *client)
{
	struct pw_core *core = client->core;

	if (core->permission_func == NULL)
		return PW_PERM_RWX;

	return core->permission_func(global, client, core->permission_data);
}

#define PW_PERM_IS_R(p)	(((p)&PW_PERM_R) == PW_PERM_R)

static struct pw_global *find_global(struct pw_core *core, uint32_t id)
{
	struct pw_global *global;
	spa_list_for_each(global, &core->global_list, link) {
		if (global->id == id)
			return global;
	}
	return NULL;
}

static void registry_bind(void *object, uint32_t id,
			  uint32_t type, uint32_t version, uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_core *core = resource->core;
	struct pw_global *global;
	uint32_t permissions;

	if ((global = find_global(core, id)) == 0)
		goto no_id;

	permissions = pw_global_permissions(global, client);

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
	return;
}

static const struct pw_registry_methods registry_methods = {
	PW_VERSION_REGISTRY_METHODS,
	.bind = registry_bind
};

static void destroy_registry_resource(void *object)
{
	struct pw_resource *resource = object;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_callbacks resource_callbacks = {
	PW_VERSION_RESOURCE_CALLBACKS,
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
	pw_resource_add_callbacks(registry_resource,
				  &data->resource_callbacks,
				  &resource_callbacks,
				  registry_resource);

	pw_resource_set_implementation(registry_resource,
				       &registry_methods,
				       registry_resource);

	spa_list_insert(this->registry_resource_list.prev, &registry_resource->link);

	spa_list_for_each(global, &this->global_list, link) {
		uint32_t permissions = pw_global_permissions(global, client);
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
core_create_node(void *object,
		 const char *factory_name,
		 const char *name,
		 uint32_t type,
		 uint32_t version,
		 const struct spa_dict *props,
		 uint32_t new_id)
{
	struct pw_resource *resource = object;
	struct pw_resource *node_resource;
	struct pw_client *client = resource->client;
	struct pw_node_factory *factory;
	struct pw_properties *properties;

	factory = pw_core_find_node_factory(client->core, factory_name);
	if (factory == NULL)
		goto no_factory;

	node_resource = pw_resource_new(client, new_id, PW_PERM_RWX, type, version, 0);
	if (node_resource == NULL)
		goto no_resource;

	if (props) {
		properties = pw_properties_new_dict(props);
		if (properties == NULL)
			goto no_properties;
	} else
		properties = NULL;

	/* error will be posted */
	pw_node_factory_create_node(factory, node_resource, name, properties);
	properties = NULL;

      done:
	return;

      no_factory:
	pw_log_error("can't find node factory");
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_INVALID_ARGUMENTS, "unknown factory name");
	goto done;

      no_resource:
	pw_log_error("can't create resource");
	goto no_mem;
      no_properties:
	pw_log_error("can't create properties");
	pw_resource_destroy(node_resource);
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

	pw_log_error("can't create link");
	pw_core_resource_error(client->core_resource,
			       resource->id, SPA_RESULT_NOT_IMPLEMENTED, "not implemented");
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

static const struct pw_core_methods core_methods = {
	PW_VERSION_CORE_METHODS,
	&core_update_types,
	&core_sync,
	&core_get_registry,
	&core_client_update,
	&core_create_node,
	&core_create_link
};

static void core_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	resource->client->core_resource = NULL;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_callbacks core_resource_callbacks = {
	PW_VERSION_RESOURCE_CALLBACKS,
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
	pw_resource_add_callbacks(resource, &data->resource_callbacks, &core_resource_callbacks, resource);

	pw_resource_set_implementation(resource, &core_methods, resource);

	spa_list_insert(this->resource_list.prev, &resource->link);
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

	this->data_loop_impl = pw_data_loop_new();
	if (this->data_loop_impl == NULL)
		goto no_data_loop;

	this->data_loop = pw_data_loop_get_loop(this->data_loop_impl);
	this->main_loop = main_loop;

	pw_type_init(&this->type);
	pw_map_init(&this->globals, 128, 32);

	spa_graph_init(&this->rt.graph);
	spa_graph_scheduler_init(&this->rt.sched, &this->rt.graph);

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
	spa_list_init(&this->node_factory_list);
	spa_list_init(&this->link_list);
	pw_callback_init(&this->callback_list);

	this->info.change_mask = 0;
	this->info.user_name = pw_get_user_name();
	this->info.host_name = pw_get_host_name();
	this->info.version = SPA_STRINGIFY(PW_VERSION_CORE);
	srandom(time(NULL));
	this->info.cookie = random();
	this->info.props = this->properties ? &this->properties->dict : NULL;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if ((name = pw_properties_get(properties, "pipewire.core.name")) == NULL) {
		pw_properties_setf(properties,
				   "pipewire.core.name", "pipewire-%s-%d",
				   pw_get_user_name(), getpid());
	}
	this->info.name = pw_properties_get(properties, "pipewire.core.name");
	this->properties = properties;

	this->global = pw_core_add_global(this, NULL, NULL, this->type.core, PW_VERSION_CORE,
			   core_bind_func, this);

	return this;

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
	pw_log_debug("core %p: destroy", core);
	pw_callback_emit(&core->callback_list, struct pw_core_callbacks, destroy, core);

	pw_data_loop_destroy(core->data_loop_impl);

	pw_map_clear(&core->globals);

	pw_log_debug("core %p: free", core);
	free(core);
}

/** Create and add a new global to the core
 *
 * \param core a core
 * \param owner an optional owner of the global
 * \param type the type of the global
 * \param n_ifaces number of interfaces
 * \param ifaces interface information
 * \param object the associated object
 * \param bind a function to bind to this global
 * \param[out] global a result global
 * \return true on success
 *
 * \memberof pw_core
 */
struct pw_global *
pw_core_add_global(struct pw_core *core,
	           struct pw_client *owner,
	           struct pw_global *parent,
		   uint32_t type,
		   uint32_t version,
		   pw_bind_func_t bind,
		   void *object)
{
	struct global_impl *impl;
	struct pw_global *this;
	struct pw_resource *registry;

	impl = calloc(1, sizeof(struct global_impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	this->core = core;
	this->owner = owner;
	this->type = type;
	this->version = version;
	this->bind = bind;
	this->object = object;

	this->id = pw_map_insert_new(&core->globals, this);

	if (owner)
		parent = owner->global;
	if (parent == NULL)
		parent = core->global;
	if (parent == NULL)
		parent = this;
	this->parent = parent;

	spa_list_insert(core->global_list.prev, &this->link);

	pw_callback_emit(&core->callback_list, struct pw_core_callbacks, global_added, this);

	pw_log_debug("global %p: new %u %s, owner %p", this, this->id,
			spa_type_map_get_type(core->type.map, this->type), owner);

	spa_list_for_each(registry, &core->registry_resource_list, link) {
		uint32_t permissions = pw_global_permissions(this, registry->client);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global(registry,
						    this->id,
						    this->parent->id,
						    permissions,
						    this->type,
						    this->version);
	}
	return this;
}

uint32_t pw_global_get_type(struct pw_global *global)
{
	return global->type;
}

uint32_t pw_global_get_version(struct pw_global *global)
{
	return global->version;
}

void * pw_global_get_object(struct pw_global *global)
{
	return global->object;
}

/** Bind to a global
 *
 * \param global the global to bind to
 * \param client the client that binds
 * \param version the version
 * \param id the id
 *
 * Let \a client bind to \a global with the given version and id.
 * After binding, the client and the global object will be able to
 * exchange messages.
 *
 * \memberof pw_global
 */
int
pw_global_bind(struct pw_global *global, struct pw_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	int res;

	if (global->bind == NULL)
		goto no_bind;

	if (global->version < version)
		goto wrong_version;

	res = global->bind(global, client, permissions, version, id);

	return res;

     wrong_version:
	res = SPA_RESULT_INCOMPATIBLE_VERSION;
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id,
			     res, "id %d: interface version %d < %d",
			     id, global->version, version);
	return res;
     no_bind:
	res = SPA_RESULT_NOT_IMPLEMENTED;
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id,
			     res, "can't bind object id %d to interface", id);
	return res;
}

/** Destroy a global
 *
 * \param global a global to destroy
 *
 * \memberof pw_global
 */
void pw_global_destroy(struct pw_global *global)
{
	struct pw_core *core = global->core;
	struct pw_resource *registry;

	pw_log_debug("global %p: destroy %u", global, global->id);

	spa_list_for_each(registry, &core->registry_resource_list, link) {
		uint32_t permissions = pw_global_permissions(global, registry->client);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global_remove(registry, global->id);
	}

	pw_map_remove(&core->globals, global->id);

	spa_list_remove(&global->link);
	pw_callback_emit(&core->callback_list, struct pw_core_callbacks, global_removed, global);

	pw_log_debug("global %p: free", global);
	free(global);
}

void pw_core_add_callbacks(struct pw_core *core,
			   struct pw_callback_info *info,
			   const struct pw_core_callbacks *callbacks,
			   void *data)
{
	pw_callback_add(&core->callback_list, info, callbacks, data);
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

const struct spa_dict *pw_core_get_properties(struct pw_core *core)
{
	return &core->properties->dict;
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

	if (core->properties == NULL) {
		if (dict)
			core->properties = pw_properties_new_dict(dict);
	} else if (dict != &core->properties->dict) {
		uint32_t i;

		for (i = 0; i < dict->n_items; i++)
			pw_properties_set(core->properties,
					  dict->items[i].key, dict->items[i].value);
	}

	core->info.change_mask = PW_CORE_CHANGE_MASK_PROPS;
	core->info.props = core->properties ? &core->properties->dict : NULL;

	pw_callback_emit(&core->callback_list, struct pw_core_callbacks, info_changed, &core->info);

	spa_list_for_each(resource, &core->resource_list, link) {
		pw_core_resource_info(resource, &core->info);
	}
	core->info.change_mask = 0;
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
						n_format_filters, format_filters, error) == NULL)
				continue;

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
		if ((res = pw_port_get_format(output, (const struct spa_format **) &format)) < 0) {
			asprintf(error, "error get output format: %d", res);
			goto error;
		}
	} else if (out_state == PW_PORT_STATE_CONFIGURE && in_state > PW_PORT_STATE_CONFIGURE) {
		/* only output needs format */
		if ((res = pw_port_get_format(input, (const struct spa_format **) &format)) < 0) {
			asprintf(error, "error get input format: %d", res);
			goto error;
		}
	} else if (in_state == PW_PORT_STATE_CONFIGURE && out_state == PW_PORT_STATE_CONFIGURE) {
	      again:
		/* both ports need a format */
		pw_log_debug("core %p: finding best format", core);
		if ((res = pw_port_enum_formats(input, &filter, NULL, iidx)) < 0) {
			if (res == SPA_RESULT_ENUM_END && iidx != 0) {
				asprintf(error, "error input enum formats: %d", res);
				goto error;
			}
		}
		pw_log_debug("Try filter: %p", filter);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(filter);

		if ((res = pw_port_enum_formats(output, &format, filter, oidx)) < 0) {
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

/** Find a node by name
 *
 * \param core the core object
 * \param name the name of the node to find
 *
 * Find in the list of nodes registered in \a core for one with
 * the given \a name.
 *
 * \memberof pw_core
 */
struct pw_node_factory *pw_core_find_node_factory(struct pw_core *core, const char *name)
{
	struct pw_node_factory *factory;

	spa_list_for_each(factory, &core->node_factory_list, link) {
		if (strcmp(factory->name, name) == 0)
			return factory;
	}
	return NULL;
}
