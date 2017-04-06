/* Pinos
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
#include <time.h>

#include <pinos/client/pinos.h>
#include <pinos/client/interfaces.h>
#include <pinos/server/core.h>
#include <pinos/server/data-loop.h>
#include <pinos/server/client-node.h>
#include <spa/lib/debug.h>
#include <spa/format-utils.h>

typedef struct {
  PinosGlobal   this;
  PinosBindFunc bind;
} PinosGlobalImpl;

typedef struct {
  PinosCore  this;

  SpaSupport support[4];

} PinosCoreImpl;

static void
registry_bind (void     *object,
               uint32_t  id,
               uint32_t  new_id)
{
  PinosResource *resource = object;
  PinosClient *client = resource->client;
  PinosCore *core = resource->core;
  PinosGlobal *global;

  spa_list_for_each (global, &core->global_list, link)
    if (global->id == id)
      break;

  if (&global->link == &core->global_list)
    goto no_id;

  pinos_log_debug ("global %p: bind object id %d to %d", global, id, new_id);
  pinos_global_bind (global, client, 0, new_id);

  return;

no_id:
  pinos_log_debug ("registry %p: no global with id %u to bind to %u", resource, id, new_id);
  /* unmark the new_id the map, the client does not yet know about the failed
   * bind and will choose the next id, which we would refuse when we don't mark
   * new_id as 'used and freed' */
  pinos_map_insert_at (&client->objects, new_id, NULL);
  pinos_core_notify_remove_id (client->core_resource, new_id);
  return;
}

static PinosRegistryMethods registry_methods = {
  &registry_bind
};

static void
destroy_registry_resource (void *object)
{
  PinosResource *resource = object;
  spa_list_remove (&resource->link);
}

static void
core_client_update (void          *object,
                    const SpaDict *props)
{
  PinosResource *resource = object;

  pinos_client_update_properties (resource->client, props);
}

static void
core_sync (void    *object,
           uint32_t seq)
{
  PinosResource *resource = object;

  pinos_core_notify_done (resource, seq);
}

static void
core_get_registry (void     *object,
                   uint32_t  new_id)
{
  PinosResource *resource = object;
  PinosClient *client = resource->client;
  PinosCore *this = resource->core;
  PinosGlobal *global;
  PinosResource *registry_resource;

  registry_resource = pinos_resource_new (client,
                                          new_id,
                                          this->type.registry,
                                          this,
                                          destroy_registry_resource);
  if (registry_resource == NULL)
    goto no_mem;

  registry_resource->implementation = &registry_methods;

  spa_list_insert (this->registry_resource_list.prev, &registry_resource->link);

  spa_list_for_each (global, &this->global_list, link)
    pinos_registry_notify_global (registry_resource,
                                  global->id,
                                  spa_type_map_get_type (this->type.map, global->type));

  return;

no_mem:
  pinos_log_error ("can't create registry resource");
  pinos_core_notify_error (client->core_resource,
                           resource->id,
                           SPA_RESULT_NO_MEMORY,
                           "no memory");
}

static void
core_create_node (void          *object,
                  const char    *factory_name,
                  const char    *name,
                  const SpaDict *props,
                  uint32_t       new_id)
{
  PinosResource *resource = object;
  PinosClient *client = resource->client;

  pinos_core_notify_error (client->core_resource,
                           resource->id,
                           SPA_RESULT_NOT_IMPLEMENTED,
                           "not implemented");
}

static void
core_create_client_node (void          *object,
                         const char    *name,
                         const SpaDict *props,
                         uint32_t       new_id)
{
  PinosResource *resource = object;
  PinosClient *client = resource->client;
  PinosClientNode *node;
  SpaResult res;
  int data_fd, i;
  PinosProperties *properties;

  properties = pinos_properties_new (NULL, NULL);
  if (properties == NULL)
    goto no_mem;

  for (i = 0; i < props->n_items; i++) {
    pinos_properties_set (properties, props->items[i].key,
                                      props->items[i].value);
  }

  node = pinos_client_node_new (client,
                                new_id,
                                name,
                                properties);
  if (node == NULL)
    goto no_mem;

  if ((res = pinos_client_node_get_data_socket (node, &data_fd)) < 0) {
    pinos_core_notify_error (client->core_resource,
                             resource->id,
                             SPA_RESULT_ERROR,
                             "can't get data fd");
    return;
  }

  pinos_client_node_notify_done (node->resource,
                                 data_fd);
  return;

no_mem:
  pinos_log_error ("can't create client node");
  pinos_core_notify_error (client->core_resource,
                           resource->id,
                           SPA_RESULT_NO_MEMORY,
                           "no memory");
  return;
}

static void
core_update_types (void          *object,
                   uint32_t       first_id,
                   uint32_t       n_types,
                   const char   **types)
{
  PinosResource *resource = object;
  PinosCore *this = resource->core;
  PinosClient *client = resource->client;
  int i;

  for (i = 0; i < n_types; i++, first_id++) {
    uint32_t this_id = spa_type_map_get_id (this->type.map, types[i]);
    if (!pinos_map_insert_at (&client->types, first_id, SPA_UINT32_TO_PTR (this_id)))
      pinos_log_error ("can't add type for client");
  }
}

static PinosCoreMethods core_methods = {
  &core_client_update,
  &core_sync,
  &core_get_registry,
  &core_create_node,
  &core_create_client_node,
  &core_update_types
};

static void
core_unbind_func (void *data)
{
  PinosResource *resource = data;
  resource->client->core_resource = NULL;
  spa_list_remove (&resource->link);
}

static SpaResult
core_bind_func (PinosGlobal *global,
                PinosClient *client,
                uint32_t     version,
                uint32_t     id)
{
  PinosCore *this = global->object;
  PinosResource *resource;

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 core_unbind_func);
  if (resource == NULL)
    goto no_mem;

  resource->implementation = &core_methods;

  spa_list_insert (this->resource_list.prev, &resource->link);
  client->core_resource = resource;

  pinos_log_debug ("core %p: bound to %d", global->object, resource->id);

  this->info.change_mask = PINOS_CORE_CHANGE_MASK_ALL;
  pinos_core_notify_info (resource, &this->info);

  return SPA_RESULT_OK;

no_mem:
  pinos_log_error ("can't create core resource");
  return SPA_RESULT_NO_MEMORY;
}

PinosCore *
pinos_core_new (PinosMainLoop   *main_loop,
                PinosProperties *properties)
{
  PinosCoreImpl *impl;
  PinosCore *this;

  impl = calloc (1, sizeof (PinosCoreImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  this->data_loop = pinos_data_loop_new ();
  if (this->data_loop == NULL)
    goto no_data_loop;

  this->main_loop = main_loop;
  this->properties = properties;

  pinos_type_init (&this->type);
  pinos_access_init (&this->access);
  pinos_map_init (&this->objects, 128, 32);

  impl->support[0].type = SPA_TYPE__TypeMap;
  impl->support[0].data = this->type.map;
  impl->support[1].type = SPA_TYPE__Log;
  impl->support[1].data = pinos_log_get ();
  impl->support[2].type = SPA_TYPE_LOOP__DataLoop;
  impl->support[2].data = this->data_loop->loop->loop;
  impl->support[3].type = SPA_TYPE_LOOP__MainLoop;
  impl->support[3].data = this->main_loop->loop->loop;
  this->support = impl->support;
  this->n_support = 4;

  pinos_data_loop_start (this->data_loop);

  spa_list_init (&this->resource_list);
  spa_list_init (&this->registry_resource_list);
  spa_list_init (&this->global_list);
  spa_list_init (&this->client_list);
  spa_list_init (&this->node_list);
  spa_list_init (&this->node_factory_list);
  spa_list_init (&this->link_list);
  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->global_added);
  pinos_signal_init (&this->global_removed);

  this->global = pinos_core_add_global (this,
                                        NULL,
                                        this->type.core,
                                        0,
                                        this,
                                        core_bind_func);

  this->info.id = this->global->id;
  this->info.change_mask = 0;
  this->info.user_name = pinos_get_user_name ();
  this->info.host_name = pinos_get_host_name ();
  this->info.version = "0";
  this->info.name = "pinos-0";
  srandom (time (NULL));
  this->info.cookie = random ();
  this->info.props = this->properties ? &this->properties->dict : NULL;

  return this;

no_data_loop:
  free (impl);
  return NULL;
}

void
pinos_core_destroy (PinosCore *core)
{
  PinosCoreImpl *impl = SPA_CONTAINER_OF (core, PinosCoreImpl, this);

  pinos_log_debug ("core %p: destroy", core);
  pinos_signal_emit (&core->destroy_signal, core);

  pinos_data_loop_destroy (core->data_loop);

  pinos_map_clear (&core->objects);

  pinos_log_debug ("core %p: free", core);
  free (impl);
}

PinosGlobal *
pinos_core_add_global (PinosCore     *core,
                       PinosClient   *owner,
                       uint32_t       type,
                       uint32_t       version,
                       void          *object,
                       PinosBindFunc  bind)
{
  PinosGlobalImpl *impl;
  PinosGlobal *this;
  PinosResource *registry;
  const char *type_name;

  impl = calloc (1, sizeof (PinosGlobalImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  impl->bind = bind;

  this->core = core;
  this->owner = owner;
  this->type = type;
  this->version = version;
  this->object = object;

  pinos_signal_init (&this->destroy_signal);

  this->id = pinos_map_insert_new (&core->objects, this);

  spa_list_insert (core->global_list.prev, &this->link);
  pinos_signal_emit (&core->global_added, core, this);

  type_name = spa_type_map_get_type (core->type.map, this->type);
  pinos_log_debug ("global %p: new %u %s", this, this->id, type_name);

  spa_list_for_each (registry, &core->registry_resource_list, link)
    pinos_registry_notify_global (registry,
                                  this->id,
                                  type_name);

  return this;
}

SpaResult
pinos_global_bind (PinosGlobal   *global,
                   PinosClient   *client,
                   uint32_t       version,
                   uint32_t       id)
{
  SpaResult res;
  PinosGlobalImpl *impl = SPA_CONTAINER_OF (global, PinosGlobalImpl, this);

  if (impl->bind) {
    res = impl->bind (global, client, version, id);
  } else {
    res = SPA_RESULT_NOT_IMPLEMENTED;
    pinos_core_notify_error (client->core_resource,
                             client->core_resource->id,
                             res,
                             "can't bind object id %d", id);
  }
  return res;
}

void
pinos_global_destroy (PinosGlobal *global)
{
  PinosCore *core = global->core;
  PinosResource *registry;

  pinos_log_debug ("global %p: destroy %u", global, global->id);
  pinos_signal_emit (&global->destroy_signal, global);

  spa_list_for_each (registry, &core->registry_resource_list, link)
    pinos_registry_notify_global_remove (registry, global->id);

  pinos_map_remove (&core->objects, global->id);

  spa_list_remove (&global->link);
  pinos_signal_emit (&core->global_removed, core, global);

  pinos_log_debug ("global %p: free", global);
  free (global);
}

void
pinos_core_update_properties (PinosCore     *core,
                              const SpaDict *dict)
{
  PinosResource *resource;

  if (core->properties == NULL) {
    if (dict)
      core->properties = pinos_properties_new_dict (dict);
  } else if (dict != &core->properties->dict) {
    uint32_t i;

    for (i = 0; i < dict->n_items; i++)
      pinos_properties_set (core->properties,
                            dict->items[i].key,
                            dict->items[i].value);
  }

  core->info.change_mask = PINOS_CORE_CHANGE_MASK_PROPS;
  core->info.props = core->properties ? &core->properties->dict : NULL;

  spa_list_for_each (resource, &core->resource_list, link) {
    pinos_core_notify_info (resource, &core->info);
  }
}

PinosPort *
pinos_core_find_port (PinosCore       *core,
                      PinosPort       *other_port,
                      uint32_t         id,
                      PinosProperties *props,
                      uint32_t         n_format_filters,
                      SpaFormat      **format_filters,
                      char           **error)
{
  PinosPort *best = NULL;
  bool have_id;
  PinosNode *n;

  have_id = id != SPA_ID_INVALID;

  pinos_log_debug ("id \"%u\", %d", id, have_id);

  spa_list_for_each (n, &core->node_list, link) {
    if (n->global == NULL)
      continue;

    pinos_log_debug ("node id \"%d\"", n->global->id);

    if (have_id) {
      if (n->global->id == id) {
        pinos_log_debug ("id \"%u\" matches node %p", id, n);

        best = pinos_node_get_free_port (n, pinos_direction_reverse (other_port->direction));
        if (best)
          break;
      }
    } else {
      PinosPort *p, *pin, *pout;

      p = pinos_node_get_free_port (n, pinos_direction_reverse (other_port->direction));
      if (p == NULL)
        continue;

      if (p->direction == PINOS_DIRECTION_OUTPUT) {
        pin = other_port;
        pout = p;
      } else {
        pin = p;
        pout = other_port;
      }

      if (pinos_core_find_format (core,
                                  pout,
                                  pin,
                                  props,
                                  n_format_filters,
                                  format_filters,
                                  error) == NULL)
        continue;

      best = p;
    }
  }
  if (best == NULL) {
    asprintf (error, "No matching Node found");
  }
  return best;
}

SpaFormat *
pinos_core_find_format (PinosCore       *core,
                        PinosPort       *output,
                        PinosPort       *input,
                        PinosProperties *props,
                        uint32_t         n_format_filters,
                        SpaFormat      **format_filterss,
                        char           **error)
{
  SpaNodeState out_state, in_state;
  SpaResult res;
  SpaFormat *filter = NULL, *format;
  uint32_t iidx = 0, oidx = 0;

  out_state = output->node->node->state;
  in_state = input->node->node->state;

  if (out_state > SPA_NODE_STATE_CONFIGURE && output->node->state == PINOS_NODE_STATE_IDLE)
    out_state = SPA_NODE_STATE_CONFIGURE;
  if (in_state > SPA_NODE_STATE_CONFIGURE && input->node->state == PINOS_NODE_STATE_IDLE)
    in_state = SPA_NODE_STATE_CONFIGURE;

  if (in_state == SPA_NODE_STATE_CONFIGURE && out_state > SPA_NODE_STATE_CONFIGURE) {
    /* only input needs format */
    if ((res = spa_node_port_get_format (output->node->node,
                                         SPA_DIRECTION_OUTPUT,
                                         output->port_id,
                                         (const SpaFormat **)&format)) < 0) {
      asprintf (error, "error get output format: %d", res);
      goto error;
    }
  } else if (out_state == SPA_NODE_STATE_CONFIGURE && in_state > SPA_NODE_STATE_CONFIGURE) {
    /* only output needs format */
    if ((res = spa_node_port_get_format (input->node->node,
                                         SPA_DIRECTION_INPUT,
                                         input->port_id,
                                         (const SpaFormat **)&format)) < 0) {
      asprintf (error, "error get input format: %d", res);
      goto error;
    }
  } else if (in_state == SPA_NODE_STATE_CONFIGURE && out_state == SPA_NODE_STATE_CONFIGURE) {
again:
    /* both ports need a format */
    pinos_log_debug ("core %p: finding best format", core);
    if ((res = spa_node_port_enum_formats (input->node->node,
                                           SPA_DIRECTION_INPUT,
                                           input->port_id,
                                           &filter,
                                           NULL,
                                           iidx)) < 0) {
      if (res == SPA_RESULT_ENUM_END && iidx != 0) {
        asprintf (error, "error input enum formats: %d", res);
        goto error;
      }
    }
    pinos_log_debug ("Try filter: %p", filter);
    if (pinos_log_level_enabled (SPA_LOG_LEVEL_DEBUG))
      spa_debug_format (filter, core->type.map);

    if ((res = spa_node_port_enum_formats (output->node->node,
                                           SPA_DIRECTION_OUTPUT,
                                           output->port_id,
                                           &format,
                                           filter,
                                           oidx)) < 0) {
      if (res == SPA_RESULT_ENUM_END) {
        oidx = 0;
        iidx++;
        goto again;
      }
      asprintf (error, "error output enum formats: %d", res);
      goto error;
    }
    pinos_log_debug ("Got filtered:");
    if (pinos_log_level_enabled (SPA_LOG_LEVEL_DEBUG))
      spa_debug_format (format, core->type.map);

    spa_format_fixate (format);
  } else {
    asprintf (error, "error node state");
    goto error;
  }
  if (format == NULL) {
    asprintf (error, "error get format");
    goto error;
  }
  return format;

error:
  return NULL;
}

PinosNodeFactory *
pinos_core_find_node_factory (PinosCore  *core,
                              const char *name)
{
  PinosNodeFactory *factory;

  spa_list_for_each (factory, &core->node_factory_list, link) {
    if (strcmp (factory->name, name) == 0)
      return factory;
  }
  return NULL;
}
