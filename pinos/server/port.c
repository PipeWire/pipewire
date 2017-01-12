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

#include "pinos/server/port.h"

typedef struct
{
  PinosPort this;

  uint32_t seq;
} PinosPortImpl;

PinosPort *
pinos_port_new (PinosNode      *node,
                PinosDirection  direction,
                uint32_t        port_id)
{
  PinosPortImpl *impl;
  PinosPort *this;

  impl = calloc (1, sizeof (PinosPortImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  this->node = node;
  this->direction = direction;
  this->port_id = port_id;

  spa_list_init (&this->links);
  spa_list_init (&this->rt.links);
  pinos_signal_init (&this->destroy_signal);

  return this;
}

void
pinos_port_destroy (PinosPort *port)
{
  pinos_log_debug ("port %p: destroy", port);

  pinos_signal_emit (&port->destroy_signal, port);

  spa_list_remove (&port->link);

  spa_node_port_use_buffers (port->node->node,
                             port->direction,
                             port->port_id,
                             NULL, 0);
  port->buffers = NULL;
  port->n_buffers = 0;

  free (port);
}

static SpaResult
do_add_link (SpaLoop        *loop,
             bool            async,
             uint32_t        seq,
             size_t          size,
             void           *data,
             void           *user_data)
{
  PinosPort *this = user_data;
  PinosLink *link = ((PinosLink**)data)[0];

  if (this->direction == PINOS_DIRECTION_INPUT) {
    spa_list_insert (this->rt.links.prev, &link->rt.input_link);
    link->rt.input = this;
  }
  else {
    spa_list_insert (this->rt.links.prev, &link->rt.output_link);
    link->rt.output = this;
  }

  return SPA_RESULT_OK;
}

static PinosLink *
find_link (PinosPort *output_port, PinosPort *input_port)
{
  PinosLink *pl;

  spa_list_for_each (pl, &output_port->links, output_link) {
    if (pl->input == input_port)
      return pl;
  }
  return NULL;
}

PinosLink *
pinos_port_get_link (PinosPort       *output_port,
                     PinosPort       *input_port)
{
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
                 SpaFormat      **format_filter,
                 PinosProperties *properties,
                 char           **error)
{
  PinosNode *input_node, *output_node;
  PinosLink *link;

  output_node = output_port->node;
  input_node = input_port->node;

  pinos_log_debug ("port link %p:%u -> %p:%u", output_node, output_port->port_id, input_node, input_port->port_id);

  if (output_node == input_node)
    goto same_node;

  if (!spa_list_is_empty (&input_port->links))
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
    if (link == NULL)
      goto no_mem;

    spa_list_insert (output_port->links.prev, &link->output_link);
    spa_list_insert (input_port->links.prev, &link->input_link);

    output_node->n_used_output_links++;
    input_node->n_used_input_links++;

    pinos_loop_invoke (output_node->data_loop->loop,
                       do_add_link,
                       SPA_ID_INVALID,
                       sizeof (PinosLink *),
                       &link,
                       output_port);
    pinos_loop_invoke (input_node->data_loop->loop,
                       do_add_link,
                       SPA_ID_INVALID,
                       sizeof (PinosLink *),
                       &link,
                       input_port);
  }
  return link;

same_node:
  {
    asprintf (error, "can't link a node to itself");
    return NULL;
  }
was_linked:
  {
    asprintf (error, "input port was already linked");
    return NULL;
  }
no_mem:
  return NULL;
}

static SpaResult
pinos_port_pause (PinosPort *port)
{
  SpaNodeCommand cmd;

  cmd.type = SPA_NODE_COMMAND_PAUSE;
  cmd.size = sizeof (cmd);
  return spa_node_port_send_command (port->node->node,
                                    port->direction,
                                    port->port_id,
                                    &cmd);
}

static SpaResult
do_remove_link_done (SpaLoop        *loop,
                     bool            async,
                     uint32_t        seq,
                     size_t          size,
                     void           *data,
                     void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *node = port->node;
  PinosLink *link = ((PinosLink**)data)[0];

  pinos_log_debug ("port %p: finish unlink", port);
  if (port->direction == PINOS_DIRECTION_OUTPUT) {
    if (link->output) {
      spa_list_remove (&link->output_link);
      node->n_used_output_links--;
      link->output = NULL;
    }
  } else {
    if (link->input) {
      spa_list_remove (&link->input_link);
      node->n_used_input_links--;
      link->input = NULL;
    }
  }

  if (node->n_used_output_links == 0 &&
      node->n_used_input_links == 0) {
    pinos_node_update_state (node, PINOS_NODE_STATE_IDLE, NULL);
  }

  if (!port->allocated) {
    pinos_log_debug ("port %p: clear buffers on port", port);
    spa_node_port_use_buffers (port->node->node,
                               port->direction,
                               port->port_id,
                               NULL, 0);
    port->buffers = NULL;
    port->n_buffers = 0;
  }

  return SPA_RESULT_OK;
}

static SpaResult
do_remove_link (SpaLoop        *loop,
                bool            async,
                uint32_t        seq,
                size_t          size,
                void           *data,
                void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  PinosLink *link = ((PinosLink**)data)[0];
  SpaResult res;

  if (port->direction == PINOS_DIRECTION_INPUT) {
    spa_list_remove (&link->rt.input_link);
    link->rt.input = NULL;
  } else {
    spa_list_remove (&link->rt.output_link);
    link->rt.output = NULL;
  }

#if 0
  if (spa_list_is_empty (&port->rt.links))
    pinos_port_pause (port);
#endif

  res = pinos_loop_invoke (this->core->main_loop->loop,
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
  PinosPortImpl *impl = SPA_CONTAINER_OF (port, PinosPortImpl, this);

  pinos_log_debug ("port %p: start unlink %p", port, link);

  res = pinos_loop_invoke (port->node->data_loop->loop,
                           do_remove_link,
                           impl->seq++,
                           sizeof (PinosLink *),
                           &link,
                           port);
  return res;
}

static SpaResult
do_clear_buffers_done (SpaLoop        *loop,
                       bool            async,
                       uint32_t        seq,
                       size_t          size,
                       void           *data,
                       void           *user_data)
{
  PinosPort *port = user_data;
  SpaResult res;

  pinos_log_debug ("port %p: clear buffers finish", port);

  res = spa_node_port_use_buffers (port->node->node,
                                   port->direction,
                                   port->port_id,
                                   NULL, 0);
  port->buffers = NULL;
  port->n_buffers = 0;

  return res;
}

static SpaResult
do_clear_buffers (SpaLoop        *loop,
                  bool            async,
                  uint32_t        seq,
                  size_t          size,
                  void           *data,
                  void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *node = port->node;
  SpaResult res;

  pinos_port_pause (port);

  res = pinos_loop_invoke (node->core->main_loop->loop,
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
  PinosPortImpl *impl = SPA_CONTAINER_OF (port, PinosPortImpl, this);

  pinos_log_debug ("port %p: clear buffers", port);
  res = pinos_loop_invoke (port->node->data_loop->loop,
                           do_clear_buffers,
                           impl->seq++,
                           0, NULL,
                           port);
  return res;
}
