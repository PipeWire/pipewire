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
#include "pinos/client/enumtypes.h"

#include "pinos/server/node.h"
#include "pinos/server/data-loop.h"
#include "pinos/server/main-loop.h"

typedef struct
{
  PinosNode this;

  PinosClient *client;

  uint32_t seq;

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
        pinos_signal_emit (&node->core->port_added, node, np);
      i++;
    } else if (p) {
      node->input_port_map[p->port_id] = NULL;
      ports = ports->next;
      if (!impl->async_init)
        pinos_signal_emit (&node->core->port_removed, node, p);
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
        pinos_signal_emit (&node->core->port_added, node, np);
      i++;
    } else if (p) {
      node->output_port_map[p->port_id] = NULL;
      ports = ports->next;
      if (!impl->async_init)
        pinos_signal_emit (&node->core->port_removed, node, p);
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

static SpaResult
do_read_link (SpaLoop        *loop,
              bool            async,
              uint32_t        seq,
              size_t          size,
              void           *data,
              void           *user_data)
{
  PinosNode *this = user_data;
  PinosLink *link = ((PinosLink**)data)[0];
  size_t offset;
  SpaResult res;

  if (link->rt.input == NULL)
    return SPA_RESULT_OK;

  while (link->rt.in_ready > 0 && spa_ringbuffer_get_read_offset (&link->ringbuffer, &offset) > 0) {
    SpaPortInput *input = &this->transport->inputs[link->rt.input->port_id];

    input->buffer_id = link->queue[offset];

    if ((res = spa_node_process_input (link->rt.input->node->node)) < 0)
      pinos_log_warn ("node %p: error pushing buffer: %d, %d", this, res, input->status);

    spa_ringbuffer_read_advance (&link->ringbuffer, 1);
    link->rt.in_ready--;
  }
  return SPA_RESULT_OK;
}


static void
on_node_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  PinosNode *this = user_data;

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
      if (!pinos_main_loop_defer_complete (this->core->main_loop, this, ac->seq, ac->res)) {
        pinos_signal_emit (&this->async_complete, this, ac->seq, ac->res);
      }
      break;
    }

    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
      SpaNodeEventNeedInput *ni = (SpaNodeEventNeedInput *) event;
      PinosPort *port = this->input_port_map[ni->port_id];
      PinosLink *link;

      spa_list_for_each (link, &port->rt.links, rt.input_link) {
        if (link->rt.input == NULL || link->rt.output == NULL)
          continue;

        link->rt.in_ready++;
        spa_loop_invoke (link->rt.input->node->data_loop->loop->loop,
                         do_read_link,
                         SPA_ID_INVALID,
                         sizeof (PinosLink *),
                         &link,
                         link->rt.input->node);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    {
      SpaNodeEventHaveOutput *ho = (SpaNodeEventHaveOutput *) event;
      SpaResult res;
      bool pushed = false;
      SpaPortOutput *po = &this->transport->outputs[ho->port_id];
      PinosPort *port = this->output_port_map[ho->port_id];
      PinosLink *link;

      if ((res = spa_node_process_output (node)) < 0) {
        pinos_log_warn ("node %p: got pull error %d, %d", this, res, po->status);
        break;
      }

      spa_list_for_each (link, &port->rt.links, rt.output_link) {
        size_t offset;

        if (link->rt.input == NULL || link->rt.output == NULL)
          continue;

        if (spa_ringbuffer_get_write_offset (&link->ringbuffer, &offset) > 0) {
          link->queue[offset] = po->buffer_id;
          spa_ringbuffer_write_advance (&link->ringbuffer, 1);

          spa_loop_invoke (link->rt.input->node->data_loop->loop->loop,
                           do_read_link,
                           SPA_ID_INVALID,
                           sizeof (PinosLink *),
                           &link,
                           link->rt.input->node);
          pushed = true;
        }
      }
      if (!pushed) {
        if ((res = spa_node_port_reuse_buffer (node, ho->port_id, po->buffer_id)) < 0)
          pinos_log_warn ("node %p: error reuse buffer: %d", node, res);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaResult res;
      SpaNodeEventReuseBuffer *rb = (SpaNodeEventReuseBuffer *) event;
      PinosPort *port = this->input_port_map[rb->port_id];
      PinosLink *link;

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

static void
init_complete (PinosNode *this)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);

  update_port_ids (this, false);
  pinos_log_debug ("node %p: init completed", this);
  impl->async_init = false;

  pinos_node_update_state (this, PINOS_NODE_STATE_SUSPENDED, NULL);
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
  this = &impl->this;
  this->core = core;
  pinos_log_debug ("node %p: new", this);

  this->name = strdup (name);
  this->properties = properties;

  this->node = node;
  this->clock = clock;
  this->data_loop = core->data_loop;

  if (spa_node_set_event_callback (this->node, on_node_event, this) < 0)
    pinos_log_warn ("node %p: error setting callback", this);

  pinos_signal_init (&this->destroy_signal);
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

    for (i = 0; i < this->node->info->n_items; i++)
      pinos_properties_set (this->properties,
                            this->node->info->items[i].key,
                            this->node->info->items[i].value);
  }

  if (this->node->state > SPA_NODE_STATE_INIT) {
    init_complete (this);
  } else {
    impl->async_init = true;
    pinos_main_loop_defer (this->core->main_loop,
                           this,
                           SPA_RESULT_RETURN_ASYNC (0),
                           (PinosDeferFunc) init_complete,
                           NULL);
  }
  spa_list_insert (core->node_list.prev, &this->link);

  this->global = pinos_core_add_global (core,
                                        core->registry.uri.node,
                                        this);
  return this;
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

  pinos_main_loop_defer_cancel (this->core->main_loop, this, 0);

  spa_list_for_each_safe (port, tmp, &this->input_ports, link)
    pinos_port_destroy (port);

  spa_list_for_each_safe (port, tmp, &this->output_ports, link)
    pinos_port_destroy (port);

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

  res = spa_loop_invoke (this->core->main_loop->loop,
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
SpaResult
pinos_node_destroy (PinosNode * this)
{
  SpaResult res;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);

  pinos_log_debug ("node %p: destroy", impl);
  pinos_signal_emit (&this->destroy_signal, this);

  spa_list_remove (&this->link);
  pinos_global_destroy (this->global);

  res = spa_loop_invoke (this->data_loop->loop->loop,
                         do_node_remove,
                         impl->seq++,
                         0,
                         NULL,
                         this);
  return res;
}

/**
 * pinos_node_get_client:
 * @node: a #PinosNode
 *
 * Get the owner client of @node.
 *
 * Returns: the owner client of @node.
 */
PinosClient *
pinos_node_get_client (PinosNode *node)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);
  return impl->client;
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
  unsigned int free_port, n_ports, max_ports;
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
  free_port = 0;

  pinos_log_debug ("node %p: direction %d max %u, n %u, free_port %u", node, direction, max_ports, n_ports, free_port);

  spa_list_for_each (p, ports, link) {
    if (free_port < p->port_id) {
      port = p;
      break;
    }
    free_port = p->port_id + 1;
  }

  if (free_port >= max_ports && !spa_list_is_empty (ports)) {
    port = spa_list_first (ports, PinosPort, link);
  } else
    return NULL;

  return port;

}

static void
on_state_complete (PinosNode *node,
                   gpointer   data,
                   SpaResult  res)
{
  PinosNodeState state = GPOINTER_TO_INT (data);
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

  pinos_signal_emit (&node->core->node_state_request, node, state);

  pinos_log_debug ("node %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_CREATING:
      return SPA_RESULT_ERROR;

    case PINOS_NODE_STATE_SUSPENDED:
      res = suspend_node (node);
      break;

    case PINOS_NODE_STATE_INITIALIZING:
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

  pinos_main_loop_defer (node->core->main_loop,
                         node,
                         res,
                         (PinosDeferFunc) on_state_complete,
                         GINT_TO_POINTER (state));

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
    pinos_log_debug ("node %p: update state from %s -> %s", node,
        pinos_node_state_as_string (old),
        pinos_node_state_as_string (state));

    if (node->error)
      free (node->error);
    node->error = error;
    node->state = state;
    pinos_signal_emit (&node->core->node_state_changed, node, old, state);
  }
}
