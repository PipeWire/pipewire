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

#include "pinos/client/pinos.h"

#include "pinos/client/context.h"
#include "pinos/client/subscribe.h"

/**
 * pinos_node_state_as_string:
 * @state: a #PinosNodeeState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const char *
pinos_node_state_as_string (PinosNodeState state)
{
  switch (state) {
    case PINOS_NODE_STATE_ERROR:
      return "error";
    case PINOS_NODE_STATE_CREATING:
      return "creating";
    case PINOS_NODE_STATE_SUSPENDED:
      return "suspended";
    case PINOS_NODE_STATE_INITIALIZING:
      return "initializing";
    case PINOS_NODE_STATE_IDLE:
      return "idle";
    case PINOS_NODE_STATE_RUNNING:
      return "running";
  }
  return "invalid-state";
}

/**
 * pinos_direction_as_string:
 * @direction: a #PinosDirection
 *
 * Return the string representation of @direction.
 *
 * Returns: the string representation of @direction.
 */
const char *
pinos_direction_as_string (PinosDirection direction)
{
  switch (direction) {
    case PINOS_DIRECTION_INVALID:
      return "invalid";
    case PINOS_DIRECTION_INPUT:
      return "input";
    case PINOS_DIRECTION_OUTPUT:
      return "output";
  }
  return "invalid-direction";
}

/**
 * pinos_link_state_as_string:
 * @state: a #PinosLinkeState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const char *
pinos_link_state_as_string (PinosLinkState state)
{
  switch (state) {
    case PINOS_LINK_STATE_ERROR:
      return "error";
    case PINOS_LINK_STATE_UNLINKED:
      return "unlinked";
    case PINOS_LINK_STATE_INIT:
      return "init";
    case PINOS_LINK_STATE_NEGOTIATING:
      return "negotiating";
    case PINOS_LINK_STATE_ALLOCATING:
      return "allocating";
    case PINOS_LINK_STATE_PAUSED:
      return "paused";
    case PINOS_LINK_STATE_RUNNING:
      return "running";
  }
  return "invalid-state";
}
