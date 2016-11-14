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
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"

static PinosPort *
new_pinos_port (PinosNode *node, PinosDirection direction, uint32_t port)
{
  PinosPort *np;
  np = calloc (1, sizeof (PinosPort));
  np->node = node;
  np->direction = direction;
  np->port = port;
  np->links = g_ptr_array_new ();
  pinos_signal_init (&np->destroy_signal);
  return np;
}

static void
free_node_port (PinosPort *np)
{
  g_ptr_array_free (np->links, TRUE);
  g_slice_free (PinosPort, np);
}

typedef struct
{
  PinosNode this;

  PinosClient *client;
  PinosNode1 *iface;

  uint32_t seq;

  gboolean async_init;
  GList *input_ports;
  GList *output_ports;
  guint n_used_output_links;
  guint n_used_input_links;

  GError *error;
  guint idle_timeout;

  struct {
    GPtrArray *links;
  } rt;
} PinosNodeImpl;


static void init_complete (PinosNode *this);

static void
update_port_ids (PinosNode *node, gboolean create)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);
  uint32_t *input_port_ids, *output_port_ids;
  guint n_input_ports, n_output_ports, max_input_ports, max_output_ports;
  guint i;
  GList *ports;

  if (node->node == NULL)
    return;

  spa_node_get_n_ports (node->node,
                        &n_input_ports,
                        &max_input_ports,
                        &n_output_ports,
                        &max_output_ports);

  input_port_ids = g_alloca (sizeof (uint32_t) * n_input_ports);
  output_port_ids = g_alloca (sizeof (uint32_t) * n_output_ports);

  spa_node_get_port_ids (node->node,
                         max_input_ports,
                         input_port_ids,
                         max_output_ports,
                         output_port_ids);

  pinos_log_debug ("node %p: update_port ids %u/%u, %u/%u", node,
      n_input_ports, max_input_ports, n_output_ports, max_output_ports);

  i = 0;
  ports = impl->input_ports;
  while (true) {
    PinosPort *p = (ports ? ports->data : NULL);

    if (p && i < n_input_ports && p->port == input_port_ids[i]) {
      i++;
      ports = g_list_next (ports);
    } else if ((p && i < n_input_ports && input_port_ids[i] < p->port) || i < n_input_ports) {
      PinosPort *np;
      pinos_log_debug ("node %p: input port added %d", node, input_port_ids[i]);

      np = new_pinos_port (node, PINOS_DIRECTION_INPUT, input_port_ids[i]);
      impl->input_ports = g_list_insert_before (impl->input_ports, ports, np);

      if (!impl->async_init)
        pinos_signal_emit (&node->core->port_added, node, np);
      i++;
    } else if (p) {
      GList *next;
      pinos_log_debug ("node %p: input port removed %d", node, p->port);

      next = g_list_next (ports);
      impl->input_ports = g_list_delete_link (impl->input_ports, ports);
      ports = next;

      if (!impl->async_init)
        pinos_signal_emit (&node->core->port_removed, node, p);

      free_node_port (p);
    } else
      break;
  }

  i = 0;
  ports = impl->output_ports;
  while (true) {
    PinosPort *p = (ports ? ports->data : NULL);

    if (p && i < n_output_ports && p->port == output_port_ids[i]) {
      i++;
      ports = g_list_next (ports);
    } else if ((p && i < n_output_ports && output_port_ids[i] < p->port) || i < n_output_ports) {
      PinosPort *np;
      pinos_log_debug ("node %p: output port added %d", node, output_port_ids[i]);

      np = new_pinos_port (node, PINOS_DIRECTION_OUTPUT, output_port_ids[i]);
      impl->output_ports = g_list_insert_before (impl->output_ports, ports, np);

      if (!impl->async_init)
        pinos_signal_emit (&node->core->port_added, node, np);
      i++;
    } else if (p) {
      GList *next;
      pinos_log_debug ("node %p: output port removed %d", node, p->port);

      next = g_list_next (ports);
      impl->output_ports = g_list_delete_link (impl->output_ports, ports);
      ports = next;

      if (!impl->async_init)
        pinos_signal_emit (&node->core->port_removed, node, p);

      free_node_port (p);
    } else
      break;
  }

  node->have_inputs = n_input_ports > 0;
  node->have_outputs = n_output_ports > 0;

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
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);
  SpaResult res = SPA_RESULT_OK;
  GList *walk;

  pinos_log_debug ("node %p: suspend node", this);

  for (walk = impl->input_ports; walk; walk = g_list_next (walk)) {
    PinosPort *p = walk->data;
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_INPUT, p->port, 0, NULL)) < 0)
      pinos_log_warn ("error unset format output: %d", res);
    p->buffers = NULL;
    p->n_buffers = 0;
    if (p->allocated)
      pinos_memblock_free (&p->buffer_mem);
    p->allocated = FALSE;
  }
  for (walk = impl->output_ports; walk; walk = g_list_next (walk)) {
    PinosPort *p = walk->data;
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_OUTPUT, p->port, 0, NULL)) < 0)
      pinos_log_warn ("error unset format output: %d", res);
    p->buffers = NULL;
    p->n_buffers = 0;
    if (p->allocated)
      pinos_memblock_free (&p->buffer_mem);
    p->allocated = FALSE;
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
do_read_link (SpaPoll        *poll,
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

  if (link->input == NULL)
    return SPA_RESULT_OK;

  while (link->in_ready > 0 && spa_ringbuffer_get_read_offset (&link->ringbuffer, &offset) > 0) {
    SpaPortInput *input = &this->transport->inputs[link->input->port];

    input->buffer_id = link->queue[offset];

    if ((res = spa_node_process_input (link->input->node->node)) < 0)
      pinos_log_warn ("node %p: error pushing buffer: %d, %d", this, res, input->status);

    spa_ringbuffer_read_advance (&link->ringbuffer, 1);
    link->in_ready--;
  }
  return SPA_RESULT_OK;
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
      if (!pinos_main_loop_defer_complete (this->core->main_loop, this, ac->seq, ac->res)) {
        pinos_signal_emit (&this->async_complete, this, ac->seq, ac->res);
      }
      break;
    }

    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
      SpaNodeEventNeedInput *ni = (SpaNodeEventNeedInput *) event;
      guint i;

      for (i = 0; i < impl->rt.links->len; i++) {
        PinosLink *link = g_ptr_array_index (impl->rt.links, i);

        if (link->input == NULL || link->input->port != ni->port_id)
          continue;

        link->in_ready++;
        spa_poll_invoke (&link->input->node->data_loop->poll,
                         do_read_link,
                         SPA_ID_INVALID,
                         sizeof (PinosLink *),
                         &link,
                         link->input->node);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    {
      SpaNodeEventHaveOutput *ho = (SpaNodeEventHaveOutput *) event;
      SpaResult res;
      guint i;
      gboolean pushed = FALSE;
      SpaPortOutput *po = &this->transport->outputs[ho->port_id];

      if ((res = spa_node_process_output (node)) < 0) {
        pinos_log_warn ("node %p: got pull error %d, %d", this, res, po->status);
        break;
      }

      for (i = 0; i < impl->rt.links->len; i++) {
        PinosLink *link = g_ptr_array_index (impl->rt.links, i);
        PinosPort *output = link->output;
        PinosPort *input = link->input;
        size_t offset;

        if (output == NULL || input == NULL ||
            output->node->node != node || output->port != ho->port_id)
          continue;

        if (spa_ringbuffer_get_write_offset (&link->ringbuffer, &offset) > 0) {
          link->queue[offset] = po->buffer_id;
          spa_ringbuffer_write_advance (&link->ringbuffer, 1);

          spa_poll_invoke (&link->input->node->data_loop->poll,
                           do_read_link,
                           SPA_ID_INVALID,
                           sizeof (PinosLink *),
                           &link,
                           link->input->node);
          pushed = TRUE;
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
      guint i;

      for (i = 0; i < impl->rt.links->len; i++) {
        PinosLink *link = g_ptr_array_index (impl->rt.links, i);

        if (link->input == NULL || link->input->port != rb->port_id || link->output == NULL)
          continue;

        if ((res = spa_node_port_reuse_buffer (link->output->node->node,
                                               link->output->port,
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

static gboolean
handle_remove (PinosNode1             *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PinosNode *this = user_data;

  pinos_log_debug ("node %p: remove", this);
  pinos_node_destroy (this);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
init_complete (PinosNode *this)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);
  PinosProperties *props = this->properties;

  update_port_ids (this, false);
  pinos_log_debug ("node %p: init completed", this);
  impl->async_init = false;

  if (impl->client)
    pinos_node1_set_owner (impl->iface, this->global->object_path);
  else
    pinos_node1_set_owner (impl->iface, "/");
  pinos_node1_set_name (impl->iface, this->name);
  pinos_node1_set_properties (impl->iface, props ? pinos_properties_to_variant (props) : NULL);

  pinos_node_update_state (this, PINOS_NODE_STATE_SUSPENDED);
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
  PinosObjectSkeleton *skel;

  impl = calloc (1, sizeof (PinosNodeImpl));
  this = &impl->this;
  this->core = core;
  pinos_log_debug ("node %p: new", this);

  this->name = strdup (name);
  this->properties = properties;

  this->node = node;
  this->clock = clock;

  if (spa_node_set_event_callback (this->node, on_node_event, this) < 0)
    pinos_log_warn ("node %p: error setting callback", this);

  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->async_complete);
  pinos_signal_init (&this->transport_changed);
  pinos_signal_init (&this->loop_changed);

  impl->iface = pinos_node1_skeleton_new ();
  g_signal_connect (impl->iface, "handle-remove",
                                 (GCallback) handle_remove,
                                 node);

  this->state = PINOS_NODE_STATE_CREATING;
  pinos_node1_set_state (impl->iface, this->state);

  impl->rt.links = g_ptr_array_new_full (256, NULL);

  //g_signal_connect (this, "notify", (GCallback) on_property_notify, this);

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
    impl->async_init = TRUE;
    pinos_main_loop_defer (this->core->main_loop,
                           this,
                           SPA_RESULT_RETURN_ASYNC (0),
                           (PinosDeferFunc) init_complete,
                           NULL,
                           NULL);
  }
  spa_list_insert (core->node_list.prev, &this->list);

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_NODE);
  pinos_object_skeleton_set_node1 (skel, impl->iface);

  this->global = pinos_core_add_global (core,
                                        core->registry.uri.node,
                                        this,
                                        skel);
  return this;
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

  pinos_log_debug ("node %p: destroy", impl);
  pinos_signal_emit (&this->destroy_signal, this);

  pinos_node_set_state (this, PINOS_NODE_STATE_SUSPENDED);

  spa_list_remove (&this->list);
  pinos_core_remove_global (this->core, this->global);

  pinos_main_loop_defer_cancel (this->core->main_loop, this, 0);

  g_clear_object (&impl->iface);
  free (this->name);
  g_clear_error (&impl->error);
  if (this->properties)
    pinos_properties_free (this->properties);
  free (impl);
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
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);
  guint free_port, n_ports, max_ports;
  GList *ports, *walk;
  PinosPort *port = NULL;

  g_return_val_if_fail (node, NULL);

  if (direction == PINOS_DIRECTION_INPUT) {
    max_ports = node->transport->area->max_inputs;
    n_ports = node->transport->area->n_inputs;
    ports = impl->input_ports;
  } else {
    max_ports = node->transport->area->max_outputs;
    n_ports = node->transport->area->n_outputs;
    ports = impl->output_ports;
  }
  free_port = 0;

  pinos_log_debug ("node %p: direction %d max %u, n %u, free_port %u", node, direction, max_ports, n_ports, free_port);

  for (walk = ports; walk; walk = g_list_next (walk)) {
    PinosPort *p = walk->data;

    if (free_port < p->port) {
      port = p;
      break;
    }
    free_port = p->port + 1;
  }
  if (free_port >= max_ports && ports) {
    port = ports->data;
  } else
    return NULL;

  return port;

}


static SpaResult
do_add_link (SpaPoll        *poll,
             bool            async,
             uint32_t        seq,
             size_t          size,
             void           *data,
             void           *user_data)
{
  PinosNode *this = user_data;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);
  PinosLink *link = ((PinosLink**)data)[0];

  g_ptr_array_add (impl->rt.links, link);

  return SPA_RESULT_OK;
}

static PinosLink *
find_link (PinosPort *output_port, PinosPort *input_port)
{
  guint i;

  for (i = 0; i < output_port->links->len; i++) {
    PinosLink *pl = g_ptr_array_index (output_port->links, i);
    if (pl->input == input_port) {
      return pl;
    }
  }
  return NULL;
}

PinosLink *
pinos_port_get_link (PinosPort       *output_port,
                     PinosPort       *input_port)
{
  g_return_val_if_fail (output_port != NULL, NULL);
  g_return_val_if_fail (input_port != NULL, NULL);

  return find_link (output_port, input_port);
}

/**
 * pinos_port_link:
 * @output_port: an output port
 * @input_port: an input port
 * @format_filter: a format filter
 * @properties: extra properties
 * @error: an error or %NULL
 *
 * Make a link between @output_port and @input_port
 *
 * If the ports were already linked, the existing links will be returned.
 *
 * Returns: a new #PinosLink or %NULL and @error is set.
 */
PinosLink *
pinos_port_link (PinosPort       *output_port,
                 PinosPort       *input_port,
                 GPtrArray       *format_filter,
                 PinosProperties *properties,
                 GError         **error)
{
  PinosNodeImpl *input_impl, *output_impl;
  PinosNode *input_node, *output_node;
  PinosLink *link;

  g_return_val_if_fail (output_port != NULL, NULL);
  g_return_val_if_fail (input_port != NULL, NULL);

  output_node = output_port->node;
  output_impl = SPA_CONTAINER_OF (output_node, PinosNodeImpl, this);
  input_node = input_port->node;
  input_impl = SPA_CONTAINER_OF (input_node, PinosNodeImpl, this);

  pinos_log_debug ("port link %p:%u -> %p:%u", output_node, output_port->port, input_node, input_port->port);

  if (output_node == input_node)
    goto same_node;

  if (input_port->links->len > 0)
    goto was_linked;

  link = find_link (output_port, input_port);

  if (link == NULL)  {
    input_node->live = output_node->live;
    if (output_node->clock)
      input_node->clock = output_node->clock;
    pinos_log_debug ("node %p: clock %p, live %d", output_node, output_node->clock, output_node->live);

    link = pinos_link_new (output_node->core,
                           output_port,
                           input_port,
                           format_filter,
                           properties);

    g_ptr_array_add (output_port->links, link);
    g_ptr_array_add (input_port->links, link);

    output_impl->n_used_output_links++;
    input_impl->n_used_input_links++;

    spa_poll_invoke (&output_node->data_loop->poll,
                     do_add_link,
                     SPA_ID_INVALID,
                     sizeof (PinosLink *),
                     &link,
                     output_node);
    spa_poll_invoke (&input_node->data_loop->poll,
                     do_add_link,
                     SPA_ID_INVALID,
                     sizeof (PinosLink *),
                     &link,
                     input_node);
  }
  return link;

same_node:
  {
    g_set_error (error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_LINK,
                 "can't link a node to itself");
    return NULL;
  }
was_linked:
  {
    g_set_error (error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_LINK,
                 "input port was already linked");
    return NULL;
  }
}

static SpaResult
pinos_port_pause (PinosPort *port)
{
  SpaNodeCommand cmd;

  cmd.type = SPA_NODE_COMMAND_PAUSE;
  cmd.size = sizeof (cmd);
  return spa_node_port_send_command (port->node->node,
                                    port->direction,
                                    port->port,
                                    &cmd);
}

static SpaResult
do_remove_link_done (SpaPoll        *poll,
                     bool            async,
                     uint32_t        seq,
                     size_t          size,
                     void           *data,
                     void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);
  PinosLink *link = ((PinosLink**)data)[0];

  pinos_log_debug ("port %p: finish unlink", port);
  if (port->direction == PINOS_DIRECTION_OUTPUT) {
    if (g_ptr_array_remove_fast (port->links, link))
      impl->n_used_output_links--;
    link->output = NULL;
  } else {
    if (g_ptr_array_remove_fast (port->links, link))
      impl->n_used_input_links--;
    link->input = NULL;
  }

  if (impl->n_used_output_links == 0 &&
      impl->n_used_input_links == 0) {
    pinos_node_report_idle (this);
  }

  if (!port->allocated) {
    pinos_log_debug ("port %p: clear buffers on port", port);
    spa_node_port_use_buffers (port->node->node,
                               port->direction,
                               port->port,
                               NULL, 0);
    port->buffers = NULL;
    port->n_buffers = 0;
  }

  pinos_main_loop_defer_complete (this->core->main_loop,
                                  port,
                                  seq,
                                  SPA_RESULT_OK);
  g_object_unref (link);
  g_object_unref (port->node);

  return SPA_RESULT_OK;
}

static SpaResult
do_remove_link (SpaPoll        *poll,
                bool            async,
                uint32_t        seq,
                size_t          size,
                void           *data,
                void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);
  PinosLink *link = ((PinosLink**)data)[0];
  SpaResult res;

#if 0
  /* FIXME we should only pause when all links are gone */
  pinos_port_pause (port);
#endif

  g_ptr_array_remove_fast (impl->rt.links, link);

  res = spa_poll_invoke (this->core->main_loop->poll,
                         do_remove_link_done,
                         seq,
                         sizeof (PinosLink *),
                         &link,
                         port);
  return res;
}

SpaResult
pinos_port_unlink (PinosPort *port, PinosLink *link)
{
  SpaResult res;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (port->node, PinosNodeImpl, this);

  pinos_log_debug ("port %p: start unlink %p", port, link);

  g_object_ref (link);
  g_object_ref (port->node);
  res = spa_poll_invoke (&port->node->data_loop->poll,
                         do_remove_link,
                         impl->seq++,
                         sizeof (PinosLink *),
                         &link,
                         port);
  return res;
}

static SpaResult
do_clear_buffers_done (SpaPoll        *poll,
                       bool            async,
                       uint32_t        seq,
                       size_t          size,
                       void           *data,
                       void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  SpaResult res;

  pinos_log_debug ("port %p: clear buffers finish", port);

  res = spa_node_port_use_buffers (port->node->node,
                                   port->direction,
                                   port->port,
                                   NULL, 0);
  port->buffers = NULL;
  port->n_buffers = 0;

  pinos_main_loop_defer_complete (this->core->main_loop,
                                  port,
                                  seq,
                                  res);
  return res;
}

static SpaResult
do_clear_buffers (SpaPoll        *poll,
                  bool            async,
                  uint32_t        seq,
                  size_t          size,
                  void           *data,
                  void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  SpaResult res;

  pinos_port_pause (port);

  res = spa_poll_invoke (this->core->main_loop->poll,
                         do_clear_buffers_done,
                         seq,
                         0, NULL,
                         port);
  return res;
}

SpaResult
pinos_port_clear_buffers (PinosPort *port)
{
  SpaResult res;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (port->node, PinosNodeImpl, this);

  pinos_log_debug ("port %p: clear buffers", port);
  res = spa_poll_invoke (&port->node->data_loop->poll,
                         do_clear_buffers,
                         impl->seq++,
                         0, NULL,
                         port);
  return res;
}

/**
 * pinos_node_get_ports:
 * @node: a #PinosNode
 * @direction: a #PinosDirection
 *
 * Get the port in @node.
 *
 * Returns: a #GList of #PinosPort g_list_free after usage.
 */
GList *
pinos_node_get_ports (PinosNode *node, PinosDirection direction)
{
  GList *ports;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);

  g_return_val_if_fail (node, NULL);

  if (direction == PINOS_DIRECTION_INPUT) {
    ports = impl->input_ports;
  } else {
    ports = impl->output_ports;
  }
  return ports;
}

static void
remove_idle_timeout (PinosNode *node)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);

  if (impl->idle_timeout) {
    g_source_remove (impl->idle_timeout);
    impl->idle_timeout = 0;
  }
}

static void
on_state_complete (PinosNode *node,
                   gpointer   data,
                   SpaResult  res)
{
  PinosNodeState state = GPOINTER_TO_INT (data);

  if (SPA_RESULT_IS_ERROR (res)) {
    GError *error = NULL;
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_STATE,
                 "error changing node state: %d", res);
    pinos_node_report_error (node, error);
  } else
    pinos_node_update_state (node, state);
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

  g_return_val_if_fail (node, SPA_RESULT_INVALID_ARGUMENTS);

  remove_idle_timeout (node);

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
                         GINT_TO_POINTER (state),
                         NULL);

  return res;
}

/**
 * pinos_node_update_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 *
 * Update the state of a node. This method is used from
 * inside @node itself.
 */
void
pinos_node_update_state (PinosNode      *node,
                         PinosNodeState  state)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);
  PinosNodeState old;

  g_return_if_fail (node);

  old = node->state;
  if (old != state) {
    pinos_log_debug ("node %p: update state from %s -> %s", node,
        pinos_node_state_as_string (old),
        pinos_node_state_as_string (state));

    node->state = state;
    pinos_node1_set_state (impl->iface, state);
    pinos_signal_emit (&node->core->node_state_changed, node, old, state);
  }
}

/**
 * pinos_node_report_error:
 * @node: a #PinosNode
 * @error: a #GError
 *
 * Report an error from within @node.
 */
void
pinos_node_report_error (PinosNode *node,
                         GError    *error)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);
  PinosNodeState old;

  g_return_if_fail (node);

  g_clear_error (&impl->error);
  remove_idle_timeout (node);
  impl->error = error;
  old = node->state;
  node->state = PINOS_NODE_STATE_ERROR;
  pinos_log_debug ("node %p: got error state %s", node, error->message);
  pinos_node1_set_state (impl->iface, PINOS_NODE_STATE_ERROR);
  pinos_signal_emit (&node->core->node_state_changed, node, old, node->state);
}

static gboolean
idle_timeout (PinosNode *node)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);

  impl->idle_timeout = 0;
  pinos_log_debug ("node %p: idle timeout", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_SUSPENDED);

  return G_SOURCE_REMOVE;
}

/**
 * pinos_node_report_idle:
 * @node: a #PinosNode
 *
 * Mark @node as being idle. This will start a timeout that will
 * set the node to SUSPENDED.
 */
void
pinos_node_report_idle (PinosNode *node)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);

  g_return_if_fail (node);

  pinos_log_debug ("node %p: report idle", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_IDLE);

  impl->idle_timeout = g_timeout_add_seconds (3,
                                              (GSourceFunc) idle_timeout,
                                              node);
}

/**
 * pinos_node_report_busy:
 * @node: a #PinosNode
 *
 * Mark @node as being busy. This will set the state of the node
 * to the RUNNING state.
 */
void
pinos_node_report_busy (PinosNode *node)
{
  g_return_if_fail (node);

  pinos_log_debug ("node %p: report busy", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_RUNNING);
}
