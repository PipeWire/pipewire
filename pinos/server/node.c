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
#include "pinos/client/interfaces.h"

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
update_port_ids (PinosNode *node)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (node, PinosNodeImpl, this);
  uint32_t *input_port_ids, *output_port_ids;
  uint32_t n_input_ports, n_output_ports, max_input_ports, max_output_ports;
  uint32_t i;
  SpaList *ports;
  SpaResult res;

  if (node->node == NULL)
    return;

  spa_node_get_n_ports (node->node,
                        &n_input_ports,
                        &max_input_ports,
                        &n_output_ports,
                        &max_output_ports);

  node->n_input_ports = n_input_ports;
  node->max_input_ports = max_input_ports;
  node->n_output_ports = n_output_ports;
  node->max_output_ports = max_output_ports;

  node->input_port_map = calloc (max_input_ports, sizeof (PinosPort *));
  node->output_port_map = calloc (max_output_ports, sizeof (PinosPort *));

  input_port_ids = alloca (sizeof (uint32_t) * n_input_ports);
  output_port_ids = alloca (sizeof (uint32_t) * n_output_ports);

  spa_node_get_port_ids (node->node,
                         max_input_ports,
                         input_port_ids,
                         max_output_ports,
                         output_port_ids);

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
      if ((res = spa_node_port_set_io (node->node, SPA_DIRECTION_INPUT, np->port_id, &np->io)) < 0)
        pinos_log_warn ("node %p: can't set input IO %d", node, res);

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
      if ((res = spa_node_port_set_io (node->node, SPA_DIRECTION_OUTPUT, np->port_id, &np->io)) < 0)
        pinos_log_warn ("node %p: can't set output IO %d", node, res);

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
  pinos_signal_emit (&node->initialized, node);
}

static SpaResult
pause_node (PinosNode *this)
{
  SpaResult res;

  if (this->state <= PINOS_NODE_STATE_IDLE)
    return SPA_RESULT_OK;

  pinos_log_debug ("node %p: pause node", this);
  {
    SpaCommand cmd = SPA_COMMAND_INIT (this->core->type.command_node.Pause);
    if ((res = spa_node_send_command (this->node, &cmd)) < 0)
      pinos_log_debug ("got error %d", res);
  }

  return res;
}

static SpaResult
start_node (PinosNode *this)
{
  SpaResult res;

  pinos_log_debug ("node %p: start node", this);
  {
    SpaCommand cmd = SPA_COMMAND_INIT (this->core->type.command_node.Start);
    if ((res = spa_node_send_command (this->node, &cmd)) < 0)
      pinos_log_debug ("got error %d", res);
  }
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
    p->state = PINOS_PORT_STATE_CONFIGURE;
  }

  spa_list_for_each (p, &this->output_ports, link) {
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_OUTPUT, p->port_id, 0, NULL)) < 0)
      pinos_log_warn ("error unset format output: %d", res);
    p->buffers = NULL;
    p->n_buffers = 0;
    if (p->allocated)
      pinos_memblock_free (&p->buffer_mem);
    p->allocated = false;
    p->state = PINOS_PORT_STATE_CONFIGURE;
  }
  return res;
}

static void
send_clock_update (PinosNode *this)
{
  SpaResult res;
  SpaCommandNodeClockUpdate cu =
    SPA_COMMAND_NODE_CLOCK_UPDATE_INIT(
        this->core->type.command_node.ClockUpdate,
        SPA_COMMAND_NODE_CLOCK_UPDATE_TIME |
            SPA_COMMAND_NODE_CLOCK_UPDATE_SCALE |
            SPA_COMMAND_NODE_CLOCK_UPDATE_STATE |
            SPA_COMMAND_NODE_CLOCK_UPDATE_LATENCY, /* change_mask */
        1,                                         /* rate */
        0,                                         /* ticks */
        0,                                         /* monotonic_time */
        0,                                         /* offset */
        (1 << 16) | 1,                             /* scale */
        SPA_CLOCK_STATE_RUNNING,                   /* state */
        0,                                         /* flags */
        0);                                        /* latency */

  if (this->clock && this->live) {
    cu.body.flags.value = SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE;
    res = spa_clock_get_time (this->clock,
                              &cu.body.rate.value,
                              &cu.body.ticks.value,
                              &cu.body.monotonic_time.value);
  }

  if ((res = spa_node_send_command (this->node, (SpaCommand *)&cu)) < 0)
    pinos_log_debug ("got error %d", res);
}

static SpaResult
do_pull (PinosNode *this)
{
  SpaResult res = SPA_RESULT_OK;
  PinosPort *inport;
  bool have_output = false;

  spa_list_for_each (inport, &this->input_ports, link) {
    PinosLink *link;
    PinosPort *outport;
    SpaPortIO *pi;
    SpaPortIO *po;

    pi = &inport->io;
    pinos_log_trace ("node %p: need input port %d, %d %d", this,
        inport->port_id, pi->buffer_id, pi->status);

    if (pi->status != SPA_RESULT_NEED_BUFFER)
      continue;

    spa_list_for_each (link, &inport->rt.links, rt.input_link) {
      if (link->rt.input == NULL || link->rt.output == NULL)
        continue;

      outport = link->rt.output;
      po = &outport->io;

      /* pull */
      *po = *pi;
      pi->buffer_id = SPA_ID_INVALID;

      pinos_log_trace ("node %p: process output %p %d", outport->node, po, po->buffer_id);

      res = spa_node_process_output (outport->node->node);

      if (res == SPA_RESULT_NEED_BUFFER) {
        res = do_pull (outport->node);
        pinos_log_trace ("node %p: pull return %d", outport->node, res);
      }
      else if (res == SPA_RESULT_HAVE_BUFFER) {
        *pi = *po;
        pinos_log_trace ("node %p: have output %d %d", this, pi->status, pi->buffer_id);
        have_output = true;
      }
      else if (res < 0) {
        pinos_log_warn ("node %p: got process output %d", outport->node, res);
      }

    }
  }
  if (have_output) {
    pinos_log_trace ("node %p: doing process input", this);
    res =  spa_node_process_input (this->node);
  }
  return res;
}

static void
on_node_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  PinosNode *this = user_data;
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);

  if (SPA_EVENT_TYPE (event) == this->core->type.event_node.AsyncComplete) {
    SpaEventNodeAsyncComplete *ac = (SpaEventNodeAsyncComplete *) event;

    pinos_log_debug ("node %p: async complete event %d %d", this, ac->body.seq.value, ac->body.res.value);
    pinos_work_queue_complete (impl->work, this, ac->body.seq.value, ac->body.res.value);
    pinos_signal_emit (&this->async_complete, this, ac->body.seq.value, ac->body.res.value);
  }
  else if (SPA_EVENT_TYPE (event) == this->core->type.event_node.RequestClockUpdate) {
    send_clock_update (this);
  }
}

static void
on_node_need_input (SpaNode *node, void *user_data)
{
  PinosNode *this = user_data;

  do_pull (this);
}

static void
on_node_have_output (SpaNode *node, void *user_data)
{
  PinosNode *this = user_data;
  SpaResult res;
  PinosPort *outport;

  spa_list_for_each (outport, &this->output_ports, link) {
    PinosLink *link;
    SpaPortIO *po;

    po = &outport->io;
    if (po->buffer_id == SPA_ID_INVALID)
      continue;

    pinos_log_trace ("node %p: have output %d", this, po->buffer_id);

    spa_list_for_each (link, &outport->rt.links, rt.output_link) {
      PinosPort *inport;

      if (link->rt.input == NULL || link->rt.output == NULL)
        continue;

      inport = link->rt.input;
      inport->io = *po;

      pinos_log_trace ("node %p: do process input %d", this, po->buffer_id);

      if ((res = spa_node_process_input (inport->node->node)) < 0)
        pinos_log_warn ("node %p: got process input %d", inport->node, res);

    }
    po->status = SPA_RESULT_NEED_BUFFER;
  }
  res = spa_node_process_output (this->node);
}

static void
on_node_reuse_buffer (SpaNode *node, uint32_t port_id, uint32_t buffer_id, void *user_data)
{
  PinosNode *this = user_data;
  PinosPort *inport;

  pinos_log_trace ("node %p: reuse buffer %u", this, buffer_id);

  spa_list_for_each (inport, &this->input_ports, link) {
    PinosLink *link;
    PinosPort *outport;

    spa_list_for_each (link, &inport->rt.links, rt.input_link) {
      if (link->rt.input == NULL || link->rt.output == NULL)
        continue;

      outport = link->rt.output;
      outport->io.buffer_id = buffer_id;
    }
  }
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
  PinosNodeInfo info;
  int i;

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 node_unbind_func);
  if (resource == NULL)
    goto no_mem;

  pinos_log_debug ("node %p: bound to %d", this, resource->id);

  spa_list_insert (this->resource_list.prev, &resource->link);

  info.id = global->id;
  info.change_mask = ~0;
  info.name = this->name;
  info.max_inputs = this->max_input_ports;
  info.n_inputs = this->n_input_ports;
  info.input_formats = NULL;
  for (info.n_input_formats = 0; ; info.n_input_formats++) {
    SpaFormat *fmt;

    if (spa_node_port_enum_formats (this->node,
                                    SPA_DIRECTION_INPUT,
                                    0,
                                    &fmt,
                                    NULL,
                                    info.n_input_formats) < 0)
      break;

    info.input_formats = realloc (info.input_formats, sizeof (SpaFormat*) * (info.n_input_formats + 1));
    info.input_formats[info.n_input_formats] = spa_format_copy (fmt);
  }
  info.max_outputs = this->max_output_ports;
  info.n_outputs = this->n_output_ports;
  info.output_formats = NULL;
  for (info.n_output_formats = 0; ; info.n_output_formats++) {
    SpaFormat *fmt;

    if (spa_node_port_enum_formats (this->node,
                                    SPA_DIRECTION_OUTPUT,
                                    0,
                                    &fmt,
                                    NULL,
                                    info.n_output_formats) < 0)
      break;

    info.output_formats = realloc (info.output_formats, sizeof (SpaFormat*) * (info.n_output_formats + 1));
    info.output_formats[info.n_output_formats] = spa_format_copy (fmt);
  }
  info.state = this->state;
  info.error = this->error;
  info.props = this->properties ? &this->properties->dict : NULL;

  pinos_node_notify_info (resource, &info);

  if (info.input_formats) {
    for (i = 0; i < info.n_input_formats; i++)
      free (info.input_formats[i]);
    free (info.input_formats);
  }

  if (info.output_formats) {
    for (i = 0; i < info.n_output_formats; i++)
      free (info.output_formats[i]);
    free (info.output_formats);
  }

  return SPA_RESULT_OK;

no_mem:
  pinos_log_error ("can't create node resource");
  pinos_core_notify_error (client->core_resource,
                           client->core_resource->id,
                           SPA_RESULT_NO_MEMORY,
                           "no memory");
  return SPA_RESULT_NO_MEMORY;
}

static void
init_complete (PinosNode *this)
{
  PinosNodeImpl *impl = SPA_CONTAINER_OF (this, PinosNodeImpl, this);

  update_port_ids (this);
  pinos_log_debug ("node %p: init completed", this);
  impl->async_init = false;

  spa_list_insert (this->core->node_list.prev, &this->link);
  pinos_core_add_global (this->core,
                         this->owner,
                         this->core->type.node,
                         0,
                         this,
                         node_bind_func,
                         &this->global);

  pinos_node_update_state (this, PINOS_NODE_STATE_SUSPENDED, NULL);
}

void
pinos_node_set_data_loop (PinosNode        *node,
                          PinosDataLoop    *loop)
{
  node->data_loop = loop;
  pinos_signal_emit (&node->loop_changed, node);
}

static const SpaNodeCallbacks node_callbacks = {
  &on_node_event,
  &on_node_need_input,
  &on_node_have_output,
  &on_node_reuse_buffer,
};

PinosNode *
pinos_node_new (PinosCore       *core,
                PinosClient     *owner,
                const char      *name,
                bool             async,
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
  this->owner = owner;
  pinos_log_debug ("node %p: new, owner %p", this, owner);

  impl->work = pinos_work_queue_new (this->core->main_loop->loop);

  this->name = strdup (name);
  this->properties = properties;

  this->node = node;
  this->clock = clock;
  this->data_loop = core->data_loop;

  spa_list_init (&this->resource_list);

  if (spa_node_set_callbacks (this->node, &node_callbacks, sizeof (node_callbacks), this) < 0)
    pinos_log_warn ("node %p: error setting callback", this);

  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->port_added);
  pinos_signal_init (&this->port_removed);
  pinos_signal_init (&this->state_request);
  pinos_signal_init (&this->state_changed);
  pinos_signal_init (&this->free_signal);
  pinos_signal_init (&this->async_complete);
  pinos_signal_init (&this->initialized);
  pinos_signal_init (&this->loop_changed);

  this->state = PINOS_NODE_STATE_CREATING;

  spa_list_init (&this->input_ports);
  spa_list_init (&this->output_ports);

  if (this->node->info) {
    uint32_t i;

    if (this->properties == NULL)
      this->properties = pinos_properties_new (NULL, NULL);

    if (this->properties)
      goto no_mem;

    for (i = 0; i < this->node->info->n_items; i++)
      pinos_properties_set (this->properties,
                            this->node->info->items[i].key,
                            this->node->info->items[i].value);
  }

  impl->async_init = async;
  if (async) {
    pinos_work_queue_add (impl->work,
                          this,
                          SPA_RESULT_RETURN_ASYNC (0),
                          (PinosWorkFunc) init_complete,
                          NULL);
  } else {
    init_complete (this);
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
      pinos_port_pause_rt (link->rt.input);
      spa_list_remove (&link->rt.input_link);
      link->rt.input = NULL;
    }
  }
  spa_list_for_each_safe (port, tmp, &this->output_ports, link) {
    PinosLink *link, *tlink;
    spa_list_for_each_safe (link, tlink, &port->rt.links, rt.output_link) {
      pinos_port_pause_rt (link->rt.output);
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
 * Find a new unused port in @node with @direction
 *
 * Returns: the new port or %NULL on error
 */
PinosPort *
pinos_node_get_free_port (PinosNode       *node,
                          PinosDirection   direction)
{
  uint32_t *n_ports, max_ports;
  SpaList *ports;
  PinosPort *port = NULL, *p, **portmap;
  SpaResult res;
  int i;

  if (direction == PINOS_DIRECTION_INPUT) {
    max_ports = node->max_input_ports;
    n_ports = &node->n_input_ports;
    ports = &node->input_ports;
    portmap = node->input_port_map;
  } else {
    max_ports = node->max_output_ports;
    n_ports = &node->n_output_ports;
    ports = &node->output_ports;
    portmap = node->output_port_map;
  }

  pinos_log_debug ("node %p: direction %d max %u, n %u", node, direction, max_ports, *n_ports);

  spa_list_for_each (p, ports, link) {
    if (spa_list_is_empty (&p->links)) {
      port = p;
      break;
    }
  }

  if (port == NULL) {
    /* no port, can we create one ? */
    if (*n_ports < max_ports) {
      for (i = 0; i < max_ports && port == NULL; i++) {
        if (portmap[i] == NULL) {
          pinos_log_debug ("node %p: creating port direction %d %u", node, direction, i);
          port = portmap[i] = pinos_port_new (node, direction, i);
          spa_list_insert (ports, &port->link);
          (*n_ports)++;
          if ((res = spa_node_add_port (node->node, direction, i)) < 0) {
            pinos_log_error ("node %p: could not add port %d", node, i);
          }
          else {
            spa_node_port_set_io (node->node, direction, i, &port->io);
          }
        }
      }
    } else {
      /* for output we can reuse an existing port */
      if (direction == PINOS_DIRECTION_OUTPUT && !spa_list_is_empty (ports)) {
        port = spa_list_first (ports, PinosPort, link);
      }
    }
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

static void
node_activate (PinosNode *this)
{
  PinosPort *port;

  spa_list_for_each (port, &this->input_ports, link) {
    PinosLink *link;
    spa_list_for_each (link, &port->links, input_link)
      pinos_link_activate (link);
  }
  spa_list_for_each (port, &this->output_ports, link) {
    PinosLink *link;
    spa_list_for_each (link, &port->links, output_link)
      pinos_link_activate (link);
  }
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
      node_activate (node);
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
    info.change_mask = 1 << 5;
    info.state = node->state;
    info.error = node->error;

    spa_list_for_each (resource, &node->resource_list, link) {
      /* global is only set when there are resources */
      info.id = node->global->id;
      pinos_node_notify_info (resource, &info);
    }
  }
}
