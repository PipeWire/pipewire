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
        pinos_resource_send_error (resource,
                                   SPA_RESULT_INVALID_OBJECT_ID,
                                   "unknown object id %u", m->id);
        return SPA_RESULT_ERROR;
      }
      if (global->bind == NULL) {
        pinos_resource_send_error (resource,
                                   SPA_RESULT_NOT_IMPLEMENTED,
                                   "can't bind object id %d", m->id);
        return SPA_RESULT_ERROR;
      }

      pinos_log_debug ("global %p: bind object id %d", global, m->id);
      global->bind (global, client, 0, m->id);
      break;
    }
    default:
      pinos_log_error ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
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

    case PINOS_MESSAGE_GET_REGISTRY:
    {
      PinosMessageGetRegistry *m = message;
      PinosGlobal *global;
      PinosMessageNotifyDone nd;
      PinosResource *registry_resource;

      registry_resource = pinos_resource_new (resource->client,
                                              SPA_ID_INVALID,
                                              this->uri.registry,
                                              this,
                                              destroy_registry_resource);
      if (registry_resource == NULL)
        goto no_mem;

      registry_resource->dispatch_func = registry_dispatch_func;
      registry_resource->dispatch_data = this;

      spa_list_insert (this->registry_resource_list.prev, &registry_resource->link);

      spa_list_for_each (global, &this->global_list, link) {
        PinosMessageNotifyGlobal ng;

        ng.id = global->id;
        ng.type = spa_id_map_get_uri (this->uri.map, global->type);
        pinos_resource_send_message (registry_resource,
                                     PINOS_MESSAGE_NOTIFY_GLOBAL,
                                     &ng,
                                     false);
      }
      nd.seq = m->seq;
      pinos_resource_send_message (client->core_resource,
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
        pinos_resource_send_error (resource,
                                   SPA_RESULT_ERROR,
                                   "can't get data fd");
        break;
      }

      r.seq = m->seq;
      r.datafd = data_fd;
      pinos_resource_send_message (node->resource,
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
  pinos_resource_send_error (resource,
                             SPA_RESULT_NO_MEMORY,
                             "no memory");
  return SPA_RESULT_NO_MEMORY;
}

static void
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
                                 global->core->uri.core,
                                 global->object,
                                 NULL);
  if (resource == NULL)
    goto no_mem;

  resource->dispatch_func = core_dispatch_func;
  resource->dispatch_data = this;

  client->core_resource = resource;

  pinos_log_debug ("core %p: bound to %d", global->object, resource->id);

  m.info = &info;
  info.id = global->id;
  info.change_mask = ~0;
  info.user_name = pinos_get_user_name ();
  info.host_name = pinos_get_host_name ();
  info.version = "0";
  info.name = "pinos-0";
  srandom (time (NULL));
  info.cookie = random ();
  info.props = NULL;

  pinos_resource_send_message (resource,
                               PINOS_MESSAGE_CORE_INFO,
                               &m,
                               true);
  return;

no_mem:
  pinos_log_error ("can't create core resource");
}

PinosCore *
pinos_core_new (PinosMainLoop *main_loop)
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

  pinos_uri_init (&this->uri);
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

  spa_list_init (&this->registry_resource_list);
  spa_list_init (&this->global_list);
  spa_list_init (&this->client_list);
  spa_list_init (&this->node_list);
  spa_list_init (&this->node_factory_list);
  spa_list_init (&this->link_list);
  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->global_added);
  pinos_signal_init (&this->global_removed);
  pinos_signal_init (&this->node_state_request);
  pinos_signal_init (&this->node_state_changed);
  pinos_signal_init (&this->port_added);
  pinos_signal_init (&this->port_removed);
  pinos_signal_init (&this->port_unlinked);
  pinos_signal_init (&this->link_state_changed);
  pinos_signal_init (&this->node_unlink);
  pinos_signal_init (&this->node_unlink_done);

  this->global = pinos_core_add_global (this,
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

  pinos_signal_emit (&core->destroy_signal, core);

  pinos_data_loop_destroy (core->data_loop);

  free (impl);
}

PinosGlobal *
pinos_core_add_global (PinosCore           *core,
                       uint32_t             type,
                       uint32_t             version,
                       void                *object,
                       PinosBindFunc        bind)
{
  PinosGlobal *global;
  PinosResource *registry;
  PinosMessageNotifyGlobal ng;

  global = calloc (1, sizeof (PinosGlobal));
  if (global == NULL)
    return NULL;

  global->core = core;
  global->type = type;
  global->version = version;
  global->object = object;
  global->bind = bind;

  pinos_signal_init (&global->destroy_signal);

  global->id = pinos_map_insert_new (&core->objects, global);

  spa_list_insert (core->global_list.prev, &global->link);
  pinos_signal_emit (&core->global_added, core, global);

  pinos_log_debug ("global %p: new %u", global, global->id);

  ng.id = global->id;
  ng.type = spa_id_map_get_uri (core->uri.map, global->type);
  spa_list_for_each (registry, &core->registry_resource_list, link) {
    pinos_resource_send_message (registry,
                                 PINOS_MESSAGE_NOTIFY_GLOBAL,
                                 &ng,
                                 true);
  }
  return global;
}

static void
sync_destroy (void      *object,
              void      *data,
              SpaResult  res,
              uint32_t   id)
{
  PinosGlobal *global = object;

  pinos_log_debug ("global %p: sync destroy", global);
  free (global);
}

void
pinos_global_destroy (PinosGlobal *global)
{
  PinosCore *core = global->core;
  PinosResource *registry;
  PinosMessageNotifyGlobalRemove ng;

  pinos_log_debug ("global %p: destroy %u", global, global->id);
  pinos_signal_emit (&global->destroy_signal, global);

  pinos_map_remove (&core->objects, global->id);

  spa_list_remove (&global->link);
  pinos_signal_emit (&core->global_removed, core, global);

  ng.id = global->id;
  spa_list_for_each (registry, &core->registry_resource_list, link) {
    pinos_resource_send_message (registry,
                                 PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE,
                                 &ng,
                                 true);
  }

  pinos_main_loop_defer (core->main_loop,
                         global,
                         SPA_RESULT_WAIT_SYNC,
                         sync_destroy,
                         global);
}

PinosPort *
pinos_core_find_port (PinosCore       *core,
                      PinosPort       *other_port,
                      uint32_t         id,
                      PinosProperties *props,
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
    }
  }
  if (best == NULL) {
    asprintf (error, "No matching Node found");
  }
  return best;
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
