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
#include <pinos/server/core.h>
#include <pinos/server/data-loop.h>
#include <pinos/server/client-node.h>
#include <spa/lib/debug.h>

typedef struct {
  PinosGlobal   this;
  PinosBindFunc bind;
} PinosGlobalImpl;

typedef struct {
  PinosCore  this;

  SpaSupport support[4];

} PinosCoreImpl;

static SpaResult
registry_dispatch_func (void             *object,
                        PinosMessageType  type,
                        void             *message,
                        void             *data)
{
  SpaResult res;
  PinosResource *resource = object;
  PinosClient *client = resource->client;
  PinosCore *this = data;

  switch (type) {
    case PINOS_MESSAGE_BIND:
    {
      PinosMessageBind *m = message;
      PinosGlobal *global;

      spa_list_for_each (global, &this->global_list, link)
        if (global->id == m->id)
          break;

      if (&global->link == &this->global_list) {
        pinos_client_send_error (client,
                                 resource,
                                 SPA_RESULT_INVALID_OBJECT_ID,
                                 "unknown object id %u", m->id);
        return SPA_RESULT_ERROR;
      }
      pinos_log_debug ("global %p: bind object id %d to %d", global, m->id, m->new_id);
      res = pinos_global_bind (global, client, 0, m->new_id);
      break;
    }
    default:
      pinos_log_error ("unhandled message %d", type);
      res = SPA_RESULT_NOT_IMPLEMENTED;
      break;
  }
  return res;
}

static void
destroy_registry_resource (void *object)
{
  PinosResource *resource = object;
  spa_list_remove (&resource->link);
}

static SpaResult
core_dispatch_func (void             *object,
                    PinosMessageType  type,
                    void             *message,
                    void             *data)
{
  PinosResource *resource = object;
  PinosClient *client = resource->client;
  PinosCore *this = data;

  switch (type) {
    case PINOS_MESSAGE_CLIENT_UPDATE:
    {
      PinosMessageClientUpdate *m = message;

      pinos_client_update_properties (client,
                                      m->props);
      break;
    }
    case PINOS_MESSAGE_SYNC:
    {
      PinosMessageSync *m = message;
      PinosMessageNotifyDone r;

      r.seq = m->seq;
      pinos_client_send_message (client,
                                 resource,
                                 PINOS_MESSAGE_NOTIFY_DONE,
                                 &r,
                                 true);
      break;
    }

    case PINOS_MESSAGE_GET_REGISTRY:
    {
      PinosMessageGetRegistry *m = message;
      PinosGlobal *global;
      PinosMessageNotifyDone nd;
      PinosResource *registry_resource;

      registry_resource = pinos_resource_new (resource->client,
                                              m->new_id,
                                              this->uri.registry,
                                              this,
                                              destroy_registry_resource);
      if (registry_resource == NULL)
        goto no_mem;

      pinos_resource_set_dispatch (registry_resource,
                                   registry_dispatch_func,
                                   this);

      spa_list_insert (this->registry_resource_list.prev, &registry_resource->link);

      spa_list_for_each (global, &this->global_list, link) {
        PinosMessageNotifyGlobal ng;

        ng.id = global->id;
        ng.type = spa_id_map_get_uri (this->uri.map, global->type);
        pinos_client_send_message (client,
                                   registry_resource,
                                   PINOS_MESSAGE_NOTIFY_GLOBAL,
                                   &ng,
                                   false);
      }
      nd.seq = m->seq;
      pinos_client_send_message (client,
                                 client->core_resource,
                                 PINOS_MESSAGE_NOTIFY_DONE,
                                 &nd,
                                 true);
      break;
    }
    case PINOS_MESSAGE_CREATE_CLIENT_NODE:
    {
      PinosMessageCreateClientNode *m = message;
      PinosClientNode *node;
      SpaResult res;
      int data_fd, i;
      PinosMessageCreateClientNodeDone r;
      PinosProperties *props;

      props = pinos_properties_new (NULL, NULL);
      if (props == NULL)
        goto no_mem;

      for (i = 0; i < m->props->n_items; i++) {
        pinos_properties_set (props, m->props->items[i].key,
                                     m->props->items[i].value);
      }

      node = pinos_client_node_new (client,
                                    m->new_id,
                                    m->name,
                                    props);
      if (node == NULL)
        goto no_mem;

      if ((res = pinos_client_node_get_data_socket (node, &data_fd)) < 0) {
        pinos_client_send_error (client,
                                 resource,
                                 SPA_RESULT_ERROR,
                                 "can't get data fd");
        break;
      }

      r.seq = m->seq;
      r.datafd = data_fd;
      pinos_client_send_message (client,
                                 node->resource,
                                 PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE,
                                 &r,
                                 true);
      break;
    }
    default:
      pinos_log_error ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;

no_mem:
  pinos_client_send_error (client,
                           resource,
                           SPA_RESULT_NO_MEMORY,
                           "no memory");
  return SPA_RESULT_NO_MEMORY;
}

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
  PinosMessageCoreInfo m;
  PinosCoreInfo info;

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 core_unbind_func);
  if (resource == NULL)
    goto no_mem;

  pinos_resource_set_dispatch (resource,
                               core_dispatch_func,
                               this);

  spa_list_insert (this->resource_list.prev, &resource->link);
  client->core_resource = resource;

  pinos_log_debug ("core %p: bound to %d", global->object, resource->id);

  m.info = &info;
  info.id = global->id;
  info.change_mask = PINOS_CORE_CHANGE_MASK_ALL;
  info.user_name = pinos_get_user_name ();
  info.host_name = pinos_get_host_name ();
  info.version = "0";
  info.name = "pinos-0";
  srandom (time (NULL));
  info.cookie = random ();
  info.props = this->properties ? &this->properties->dict : NULL;

  return pinos_client_send_message (resource->client,
                                    resource,
                                    PINOS_MESSAGE_CORE_INFO,
                                    &m,
                                    true);
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

  pinos_uri_init (&this->uri);
  pinos_access_init (&this->access);
  pinos_map_init (&this->objects, 512);

  impl->support[0].uri = SPA_ID_MAP_URI;
  impl->support[0].data = this->uri.map;
  impl->support[1].uri = SPA_LOG_URI;
  impl->support[1].data = pinos_log_get ();
  impl->support[2].uri = SPA_LOOP__DataLoop;
  impl->support[2].data = this->data_loop->loop->loop;
  impl->support[3].uri = SPA_LOOP__MainLoop;
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
                                        this->uri.core,
                                        0,
                                        this,
                                        core_bind_func);

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
  PinosMessageNotifyGlobal ng;

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

  ng.id = this->id;
  ng.type = spa_id_map_get_uri (core->uri.map, this->type);

  pinos_log_debug ("global %p: new %u %s", this, ng.id, ng.type);

  spa_list_for_each (registry, &core->registry_resource_list, link) {
    pinos_client_send_message (registry->client,
                               registry,
                               PINOS_MESSAGE_NOTIFY_GLOBAL,
                               &ng,
                               true);
  }
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
    pinos_client_send_error (client,
                             client->core_resource,
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
  PinosMessageNotifyGlobalRemove ng;

  pinos_log_debug ("global %p: destroy %u", global, global->id);
  pinos_signal_emit (&global->destroy_signal, global);

  ng.id = global->id;
  spa_list_for_each (registry, &core->registry_resource_list, link) {
    pinos_client_send_message (registry->client,
                               registry,
                               PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE,
                               &ng,
                               true);
  }

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
  PinosMessageCoreInfo m;
  PinosCoreInfo info;
  PinosResource *resource;

  if (core->properties == NULL) {
    if (dict)
      core->properties = pinos_properties_new_dict (dict);
  } else if (dict != &core->properties->dict) {
    unsigned int i;

    for (i = 0; i < dict->n_items; i++)
      pinos_properties_set (core->properties,
                            dict->items[i].key,
                            dict->items[i].value);
  }

  m.info = &info;
  info.id = core->global->id;
  info.change_mask = PINOS_CORE_CHANGE_MASK_PROPS;
  info.props = core->properties ? &core->properties->dict : NULL;

  spa_list_for_each (resource, &core->resource_list, link) {
    pinos_client_send_message (resource->client,
                               resource,
                               PINOS_MESSAGE_CORE_INFO,
                               &m,
                               true);
  }
}

PinosPort *
pinos_core_find_port (PinosCore       *core,
                      PinosPort       *other_port,
                      uint32_t         id,
                      PinosProperties *props,
                      unsigned int     n_format_filters,
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
                        unsigned int     n_format_filters,
                        SpaFormat      **format_filterss,
                        char           **error)
{
  SpaNodeState out_state, in_state;
  SpaResult res;
  SpaFormat *filter = NULL, *format;
  unsigned int iidx = 0, oidx = 0;

  out_state = output->node->node->state;
  in_state = input->node->node->state;

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
      spa_debug_format (filter);

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
      spa_debug_format (format);

    spa_format_fixate (format);
  } else {
    asprintf (error, "error node state");
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
