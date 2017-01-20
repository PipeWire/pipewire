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

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "pinos/client/pinos.h"

#include "pinos/server/node.h"
#include "pinos/server/data-loop.h"
#include "pinos/server/main-loop.h"
#include "pinos/server/work-queue.h"

typedef struct
{
  PinosNode this;

  PinosWorkQueue *work;

  bool async_init;
} PinosNodeImpl;

static void init_complete (PinosNode *this);

static void
update_port_ids (PinosNode *node, bool create)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);
  uint32_t *input_port_ids, *output_port_ids;
  unsigned int n_input_ports, n_output_ports, max_input_ports, max_output_ports;
  unsigned int i;
  SpaList *ports;

  if (node->node == NULL)
    return;

  spa_node_get_n_ports (node->node,
                        &n_input_ports,
                        &max_input_ports,
                        &n_output_ports,
                        &max_output_ports);

  input_port_ids = alloca (sizeof (uint32_t) * n_input_ports);
  output_port_ids = alloca (sizeof (uint32_t) * n_output_ports);

  spa_node_get_port_ids (node->node,
                         max_input_ports,
                         input_port_ids,
                         max_output_ports,
                         output_port_ids);

  node->input_port_map = realloc (node->input_port_map, sizeof (PinosPort *) * max_input_ports);
  node->output_port_map = realloc (node->output_port_map, sizeof (PinosPort *) * max_output_ports);

  pinos_log_debug ("node %p: update_port ids %u/%u, %u/%u", node,
      n_input_ports, max_input_ports, n_output_ports, max_output_ports);

  i = 0;
  ports = &node->input_ports;
  while (true) {
    PinosPort *p = (ports == &node->input_ports) ? NULL : SPA_CONTAINER_OF (ports, PinosPort, link);

    if (p && i < n_input_ports && p->port_id == input_port_ids[i]) {
      node->input_port_map[p->port_id] = p;
      pinos_log_debug ("node %p: exiting input port %d", node, input_port_ids[i]);
      i++;
      ports = ports->next;
    } else if ((p && i < n_input_ports && input_port_ids[i] < p->port_id) || i < n_input_ports) {
      PinosPort *np;
      pinos_log_debug ("node %p: input port added %d", node, input_port_ids[i]);

      np = pinos_port_new (node, PINOS_DIRECTION_INPUT, input_port_ids[i]);
      spa_list_insert (ports, &np->link);
      ports = np->link.next;
      node->input_port_map[np->port_id] = np;

      if (!impl->async_init)
        pinos_signal_emit (&node->port_added, node, np);
      i++;
    } else if (p) {
      node->input_port_map[p->port_id] = NULL;
      ports = ports->next;
      if (!impl->async_init)
        pinos_signal_emit (&node->port_removed, node, p);
      pinos_log_debug ("node %p: input port removed %d", node, p->port_id);
      pinos_port_destroy (p);
    } else {
      pinos_log_debug ("node %p: no more input ports", node);
      break;
    }
  }

  i = 0;
  ports = &node->output_ports;
  while (true) {
    PinosPort *p = (ports == &node->output_ports) ? NULL : SPA_CONTAINER_OF (ports, PinosPort, link);

    if (p && i < n_output_ports && p->port_id == output_port_ids[i]) {
      pinos_log_debug ("node %p: exiting output port %d", node, output_port_ids[i]);
      i++;
      ports = ports->next;
      node->output_port_map[p->port_id] = p;
    } else if ((p && i < n_output_ports && output_port_ids[i] < p->port_id) || i < n_output_ports) {
      PinosPort *np;
      pinos_log_debug ("node %p: output port added %d", node, output_port_ids[i]);

      np = pinos_port_new (node, PINOS_DIRECTION_OUTPUT, output_port_ids[i]);
      spa_list_insert (ports, &np->link);
      ports = np->link.next;
      node->output_port_map[np->port_id] = np;

      if (!impl->async_init)
        pinos_signal_emit (&node->port_added, node, np);
      i++;
    } else if (p) {
      node->output_port_map[p->port_id] = NULL;
      ports = ports->next;
      if (!impl->async_init)
        pinos_signal_emit (&node->port_removed, node, p);
      pinos_log_debug ("node %p: output port removed %d", node, p->port_id);
      pinos_port_destroy (p);
    } else {
      pinos_log_debug ("node %p: no more output ports", node);
      break;
    }
  }

  node->transport = pinos_transport_new (max_input_ports,
                                         max_output_ports);

  node->transport->area->n_inputs = n_input_ports;
  node->transport->area->n_outputs = n_output_ports;

  for (i = 0; i < max_input_ports; i++)
    spa_node_port_set_input (node->node, i, &node->transport->inputs[i]);
  for (i = 0; i < max_output_ports; i++)
    spa_node_port_set_output (node->node, i, &node->transport->outputs[i]);

  pinos_signal_emit (&node->transport_changed, node);
}

static SpaResult
pause_node (PinosNode *this)
{
  SpaResult res;
  SpaNodeCommand cmd;

  if (this->node->state <= SPA_NODE_STATE_PAUSED)
    return SPA_RESULT_OK;

  pinos_log_debug ("node %p: pause node", this);
  cmd.type = SPA_NODE_COMMAND_PAUSE;
  cmd.size = sizeof (cmd);
  if ((res = spa_node_send_command (this->node, &cmd)) < 0)
    pinos_log_debug ("got error %d", res);

  return res;
}

static SpaResult
start_node (PinosNode *this)
{
  SpaResult res;
  SpaNodeCommand cmd;

  pinos_log_debug ("node %p: start node", this);
  cmd.type = SPA_NODE_COMMAND_START;
  cmd.size = sizeof (cmd);
  if ((res = spa_node_send_command (this->node, &cmd)) < 0)
    pinos_log_debug ("got error %d", res);

  return res;
}

static SpaResult
suspend_node (PinosNode *this)
{
  SpaResult res = SPA_RESULT_OK;
  PinosPort *p;

  pinos_log_debug ("node %p: suspend node", this);

  spa_list_for_each (p, &this->input_ports, link) {
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_INPUT, p->port_id, 0, NULL)) < 0)
      pinos_log_warn ("error unset format input: %d", res);
    p->buffers = NULL;
    p->n_buffers = 0;
    if (p->allocated)
      pinos_memblock_free (&p->buffer_mem);
    p->allocated = false;
  }

  spa_list_for_each (p, &this->output_ports, link) {
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_OUTPUT, p->port_id, 0, NULL)) < 0)
      pinos_log_warn ("error unset format output: %d", res);
    p->buffers = NULL;
    p->n_buffers = 0;
    if (p->allocated)
      pinos_memblock_free (&p->buffer_mem);
    p->allocated = false;
  }
  return res;
}

static void
send_clock_update (PinosNode *this)
{
  SpaNodeCommandClockUpdate cu;
  SpaResult res;

  cu.command.type = SPA_NODE_COMMAND_CLOCK_UPDATE;
  cu.command.size = sizeof (cu);
  cu.flags = 0;
  cu.change_mask = SPA_NODE_COMMAND_CLOCK_UPDATE_TIME |
                   SPA_NODE_COMMAND_CLOCK_UPDATE_SCALE |
                   SPA_NODE_COMMAND_CLOCK_UPDATE_STATE |
                   SPA_NODE_COMMAND_CLOCK_UPDATE_LATENCY;
  if (this->clock && this->live) {
    cu.flags = SPA_NODE_COMMAND_CLOCK_UPDATE_FLAG_LIVE;
    res = spa_clock_get_time (this->clock, &cu.rate, &cu.ticks, &cu.monotonic_time);
  } else {
    cu.rate = 1;
    cu.ticks = 0;
    cu.monotonic_time = 0;
  }
  cu.scale = (1 << 16) | 1;
  cu.state = SPA_CLOCK_STATE_RUNNING;

  if ((res = spa_node_send_command (this->node, &cu.command)) < 0)
    pinos_log_debug ("got error %d", res);
}

static void
on_node_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  PinosNode *this = user_data;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_INVALID:
    case SPA_NODE_EVENT_TYPE_ERROR:
    case SPA_NODE_EVENT_TYPE_BUFFERING:
    case SPA_NODE_EVENT_TYPE_REQUEST_REFRESH:
      break;

    case SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE:
    {
      SpaNodeEventAsyncComplete *ac = (SpaNodeEventAsyncComplete *) event;

      pinos_log_debug ("node %p: async complete event %d %d", this, ac->seq, ac->res);
      if (!pinos_work_queue_complete (impl->work, this, ac->seq, ac->res)) {
        pinos_signal_emit (&this->async_complete, this, ac->seq, ac->res);
      }
      break;
    }

    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
      SpaResult res;
      int i;
      bool processed = false;

//      pinos_log_debug ("node %p: need input", this);

      for (i = 0; i < this->transport->area->n_inputs; i++) {
        PinosLink *link;
        PinosPort *inport, *outport;
        SpaPortInput *pi;
        SpaPortOutput *po;

        pi = &this->transport->inputs[i];
        if (pi->buffer_id != SPA_ID_INVALID)
          continue;

        inport = this->input_port_map[i];
        spa_list_for_each (link, &inport->rt.links, rt.input_link) {
          if (link->rt.input == NULL || link->rt.output == NULL)
            continue;

          outport = link->rt.output;
          po = &outport->node->transport->outputs[outport->port_id];

          if (po->buffer_id != SPA_ID_INVALID) {
            processed = true;

            pi->buffer_id = po->buffer_id;
            po->buffer_id = SPA_ID_INVALID;
          }
          if ((res = spa_node_process_output (outport->node->node)) < 0)
            pinos_log_warn ("node %p: got process output %d", outport->node, res);
        }
      }
      if (processed) {
        if ((res = spa_node_process_input (this->node)) < 0)
          pinos_log_warn ("node %p: got process input %d", this, res);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    {
      SpaResult res;
      int i;
      bool processed = false;

//      pinos_log_debug ("node %p: have output", this);

      for (i = 0; i < this->transport->area->n_outputs; i++) {
        PinosLink *link;
        PinosPort *inport, *outport;
        SpaPortInput *pi;
        SpaPortOutput *po;

        po = &this->transport->outputs[i];
        if (po->buffer_id == SPA_ID_INVALID)
          continue;

        outport = this->output_port_map[i];
        spa_list_for_each (link, &outport->rt.links, rt.output_link) {
          if (link->rt.input == NULL || link->rt.output == NULL)
            continue;

          inport = link->rt.input;
          pi = &inport->node->transport->inputs[inport->port_id];

          processed = true;

          pi->buffer_id = po->buffer_id;

          if ((res = spa_node_process_input (inport->node->node)) < 0)
            pinos_log_warn ("node %p: got process input %d", inport->node, res);
        }
        po->buffer_id = SPA_ID_INVALID;
      }
      if (processed) {
        if ((res = spa_node_process_output (this->node)) < 0)
          pinos_log_warn ("node %p: got process output %d", this, res);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaResult res;
      SpaNodeEventReuseBuffer *rb = (SpaNodeEventReuseBuffer *) event;
      PinosPort *port = this->input_port_map[rb->port_id];
      PinosLink *link;

//      pinos_log_debug ("node %p: reuse buffer %u", this, rb->buffer_id);

      spa_list_for_each (link, &port->rt.links, rt.input_link) {
        if (link->rt.input == NULL || link->rt.output == NULL)
          continue;

        if ((res = spa_node_port_reuse_buffer (link->rt.output->node->node,
                                               link->rt.output->port_id,
                                               rb->buffer_id)) < 0)
          pinos_log_warn ("node %p: error reuse buffer: %d", node, res);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE:
      send_clock_update (this);
      break;
  }
}

static SpaResult
node_dispatch_func (void             *object,
                    PinosMessageType  type,
                    void             *message,
                    void             *data)
{
  PinosResource *resource = object;
  PinosNode *node = resource->object;

  switch (type) {
    default:
      pinos_log_warn ("node %p: unhandled message %d", node, type);
      break;
  }
  return SPA_RESULT_OK;
}


static void
node_unbind_func (void *data)
{
  PinosResource *resource = data;
  spa_list_remove (&resource->link);
}

static SpaResult
node_bind_func (PinosGlobal *global,
                PinosClient *client,
                uint32_t     version,
                uint32_t     id)
{
  PinosNode *this = global->object;
  PinosResource *resource;
  PinosMessageNodeInfo m;
  PinosNodeInfo info;

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 node_unbind_func);
  if (resource == NULL)
    goto no_mem;

  pinos_resource_set_dispatch (resource,
                               node_dispatch_func,
                               global);

  pinos_log_debug ("node %p: bound to %d", this, resource->id);

  spa_list_insert (this->resource_list.prev, &resource->link);

  m.info = &info;
  info.id = global->id;
  info.change_mask = ~0;
  info.name = this->name;
  info.state = this->state;
  info.error = this->error;
  info.props = this->properties ? &this->properties->dict : NULL;

  return pinos_client_send_message (client,
                                    resource,
                                    PINOS_MESSAGE_NODE_INFO,
                                    &m,
                                    true);
no_mem:
  pinos_client_send_error (client,
                           client->core_resource,
                           SPA_RESULT_NO_MEMORY,
                           "no memory");
  return SPA_RESULT_NO_MEMORY;
}

static void
init_complete (PinosNode *this)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);

  update_port_ids (this, false);
  pinos_log_debug ("node %p: init completed", this);
  impl->async_init = false;

  pinos_node_update_state (this, PINOS_NODE_STATE_SUSPENDED, NULL);

  spa_list_insert (this->core->node_list.prev, &this->link);
  this->global = pinos_core_add_global (this->core,
                                        NULL,
                                        this->core->uri.node,
                                        0,
                                        this,
                                        node_bind_func);
}

void
pinos_node_set_data_loop (PinosNode        *node,
                          PinosDataLoop    *loop)
{
  node->data_loop = loop;
  pinos_signal_emit (&node->loop_changed, node);
}

PinosNode *
pinos_node_new (PinosCore       *core,
                const char      *name,
                SpaNode         *node,
                SpaClock        *clock,
                PinosProperties *properties)
{
  PinosNodeImpl *impl;
  PinosNode *this;

  impl = calloc (1, sizeof (PinosNodeImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  this->core = core;
  pinos_log_debug ("node %p: new", this);

  impl->work = pinos_work_queue_new (this->core->main_loop->loop);

  this->name = strdup (name);
  this->properties = properties;

  this->node = node;
  this->clock = clock;
  this->data_loop = core->data_loop;

  spa_list_init (&this->resource_list);

  if (spa_node_set_event_callback (this->node, on_node_event, this) < 0)
    pinos_log_warn ("node %p: error setting callback", this);

  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->port_added);
  pinos_signal_init (&this->port_removed);
  pinos_signal_init (&this->state_request);
  pinos_signal_init (&this->state_changed);
  pinos_signal_init (&this->free_signal);
  pinos_signal_init (&this->async_complete);
  pinos_signal_init (&this->transport_changed);
  pinos_signal_init (&this->loop_changed);

  this->state = PINOS_NODE_STATE_CREATING;

  spa_list_init (&this->input_ports);
  spa_list_init (&this->output_ports);

  if (this->node->info) {
    unsigned int i;

    if (this->properties == NULL)
      this->properties = pinos_properties_new (NULL, NULL);

    if (this->properties)
      goto no_mem;

    for (i = 0; i < this->node->info->n_items; i++)
      pinos_properties_set (this->properties,
                            this->node->info->items[i].key,
                            this->node->info->items[i].value);
  }

  if (this->node->state > SPA_NODE_STATE_INIT) {
    init_complete (this);
  } else {
    impl->async_init = true;
    pinos_work_queue_add (impl->work,
                          this,
                          SPA_RESULT_RETURN_ASYNC (0),
                          (PinosWorkFunc) init_complete,
                          NULL);
  }

  return this;

no_mem:
  free (this->name);
  free (impl);
  return NULL;
}

static SpaResult
do_node_remove_done (SpaLoop        *loop,
                     bool            async,
                     uint32_t        seq,
                     size_t          size,
                     void           *data,
                     void           *user_data)
{
  PinosNode *this = user_data;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);
  PinosPort *port, *tmp;

  pinos_log_debug ("node %p: remove done, destroy ports", this);
  spa_list_for_each_safe (port, tmp, &this->input_ports, link)
    pinos_port_destroy (port);

  spa_list_for_each_safe (port, tmp, &this->output_ports, link)
    pinos_port_destroy (port);

  pinos_log_debug ("node %p: free", this);
  pinos_signal_emit (&this->free_signal, this);

  pinos_work_queue_destroy (impl->work);

  if (this->transport)
    pinos_transport_destroy (this->transport);
  if (this->input_port_map)
    free (this->input_port_map);
  if (this->output_port_map)
    free (this->output_port_map);

  free (this->name);
  free (this->error);
  if (this->properties)
    pinos_properties_free (this->properties);
  free (impl);

  return SPA_RESULT_OK;
}

static SpaResult
do_node_remove (SpaLoop        *loop,
                bool            async,
                uint32_t        seq,
                size_t          size,
                void           *data,
                void           *user_data)
{
  PinosNode *this = user_data;
  PinosPort *port, *tmp;
  SpaResult res;

  pause_node (this);

  spa_list_for_each_safe (port, tmp, &this->input_ports, link) {
    PinosLink *link, *tlink;
    spa_list_for_each_safe (link, tlink, &port->rt.links, rt.input_link) {
      spa_list_remove (&link->rt.input_link);
      link->rt.input = NULL;
    }
  }
  spa_list_for_each_safe (port, tmp, &this->output_ports, link) {
    PinosLink *link, *tlink;
    spa_list_for_each_safe (link, tlink, &port->rt.links, rt.output_link) {
      spa_list_remove (&link->rt.output_link);
      link->rt.output = NULL;
    }
  }

  res = pinos_loop_invoke (this->core->main_loop->loop,
                           do_node_remove_done,
                           seq,
                           0,
                           NULL,
                           this);

  return res;
}

/**
 * pinos_node_destroy:
 * @node: a #PinosNode
 *
 * Remove @node. This will stop the transfer on the node and
 * free the resources allocated by @node.
 */
void
pinos_node_destroy (PinosNode * this)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);
  PinosResource *resource, *tmp;

  pinos_log_debug ("node %p: destroy", impl);
  pinos_signal_emit (&this->destroy_signal, this);

  if (!impl->async_init) {
    spa_list_remove (&this->link);
    pinos_global_destroy (this->global);
  }

  spa_list_for_each_safe (resource, tmp, &this->resource_list, link)
    pinos_resource_destroy (resource);

  pinos_loop_invoke (this->data_loop->loop,
                     do_node_remove,
                     1,
                     0,
                     NULL,
                     this);
}

/**
 * pinos_node_get_free_port:
 * @node: a #PinosNode
 * @direction: a #PinosDirection
 *
 * Find a new unused port id in @node with @direction
 *
 * Returns: the new port id or %SPA_ID_INVALID on error
 */
PinosPort *
pinos_node_get_free_port (PinosNode       *node,
                          PinosDirection   direction)
{
  unsigned int n_ports, max_ports;
  SpaList *ports;
  PinosPort *port = NULL, *p;

  if (direction == PINOS_DIRECTION_INPUT) {
    max_ports = node->transport->area->max_inputs;
    n_ports = node->transport->area->n_inputs;
    ports = &node->input_ports;
  } else {
    max_ports = node->transport->area->max_outputs;
    n_ports = node->transport->area->n_outputs;
    ports = &node->output_ports;
  }

  pinos_log_debug ("node %p: direction %d max %u, n %u", node, direction, max_ports, n_ports);

  spa_list_for_each (p, ports, link) {
    if (spa_list_is_empty (&p->links)) {
      port = p;
      break;
    }
  }

  if (port == NULL) {
    if (!spa_list_is_empty (ports))
      port = spa_list_first (ports, PinosPort, link);
    else
      return NULL;
  }

  return port;

}

static void
on_state_complete (PinosNode *node,
                   void      *data,
                   SpaResult  res)
{
  PinosNodeState state = SPA_PTR_TO_INT (data);
  char *error = NULL;

  pinos_log_debug ("node %p: state complete %d", node, res);
  if (SPA_RESULT_IS_ERROR (res)) {
    asprintf (&error, "error changing node state: %d", res);
    state = PINOS_NODE_STATE_ERROR;
  }
  pinos_node_update_state (node, state, error);
}

/**
 * pinos_node_set_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 *
 * Set the state of @node to @state.
 *
 * Returns: a #SpaResult
 */
SpaResult
pinos_node_set_state (PinosNode      *node,
                      PinosNodeState  state)
{
  SpaResult res = SPA_RESULT_OK;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);

  pinos_signal_emit (&node->state_request, node, state);

  pinos_log_debug ("node %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_CREATING:
      return SPA_RESULT_ERROR;

    case PINOS_NODE_STATE_SUSPENDED:
      res = suspend_node (node);
      break;

    case PINOS_NODE_STATE_IDLE:
      res = pause_node (node);
      break;

    case PINOS_NODE_STATE_RUNNING:
      send_clock_update (node);
      res = start_node (node);
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  if (SPA_RESULT_IS_ERROR (res))
    return res;

  pinos_work_queue_add (impl->work,
                        node,
                        res,
                        (PinosWorkFunc) on_state_complete,
                        SPA_INT_TO_PTR (state));

  return res;
}

/**
 * pinos_node_update_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 * @error: error when @state is #PINOS_NODE_STATE_ERROR
 *
 * Update the state of a node. This method is used from
 * inside @node itself.
 */
void
pinos_node_update_state (PinosNode      *node,
                         PinosNodeState  state,
                         char           *error)
{
  PinosNodeState old;

  old = node->state;
  if (old != state) {
    PinosMessageNodeInfo m;
    PinosNodeInfo info;
    PinosResource *resource;

    pinos_log_debug ("node %p: update state from %s -> %s", node,
        pinos_node_state_as_string (old),
        pinos_node_state_as_string (state));

    if (node->error)
      free (node->error);
    node->error = error;
    node->state = state;

    pinos_signal_emit (&node->state_changed, node, old, state);

    spa_zero (info);
    m.info = &info;
    info.change_mask = 1 << 1;
    info.state = node->state;
    info.error = node->error;

    spa_list_for_each (resource, &node->resource_list, link) {
      info.id = node->global->id;
      pinos_client_send_message (resource->client,
                                 resource,
                                 PINOS_MESSAGE_NODE_INFO,
                                 &m,
                                 true);
    }
  }
}
