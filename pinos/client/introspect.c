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

static void
pinos_spa_dict_destroy (SpaDict *dict)
{
  SpaDictItem *item;

  spa_dict_for_each (item, dict) {
    free ((void *)item->key);
    free ((void *)item->value);
  }
  free (dict->items);
  free (dict);
}

static SpaDict *
pinos_spa_dict_copy (SpaDict *dict)
{
  SpaDict *copy;
  unsigned int i;

  if (dict == NULL)
    return NULL;

  copy = calloc (1, sizeof (SpaDict));
  if (copy == NULL)
    goto no_mem;
  copy->items = calloc (dict->n_items, sizeof (SpaDictItem));
  if (copy->items == NULL)
    goto no_items;
  copy->n_items = dict->n_items;

  for (i = 0; i < dict->n_items; i++) {
    copy->items[i].key = strdup (dict->items[i].key);
    copy->items[i].value = strdup (dict->items[i].value);
  }
  return copy;

no_items:
  free (copy);
no_mem:
  return NULL;
}

PinosCoreInfo *
pinos_core_info_update (PinosCoreInfo       *info,
                        const PinosCoreInfo *update)
{
  uint64_t change_mask;

  if (update == NULL)
    return info;

  if (info == NULL) {
    info = calloc (1, sizeof (PinosCoreInfo));
    if (info == NULL)
      return NULL;
    change_mask = ~0;
  } else {
    change_mask = info->change_mask | update->change_mask;
  }
  info->id = update->id;
  info->change_mask = change_mask;

  if (update->change_mask & (1 << 0)) {
    if (info->user_name)
      free ((void*)info->user_name);
    info->user_name = update->user_name ? strdup (update->user_name) : NULL;
  }
  if (update->change_mask & (1 << 1)) {
    if (info->host_name)
      free ((void*)info->host_name);
    info->host_name = update->host_name ? strdup (update->host_name) : NULL;
  }
  if (update->change_mask & (1 << 2)) {
    if (info->version)
      free ((void*)info->version);
    info->version = update->version ? strdup (update->version) : NULL;
  }
  if (update->change_mask & (1 << 3)) {
    if (info->name)
      free ((void*)info->name);
    info->name = update->name ? strdup (update->name) : NULL;
  }
  if (update->change_mask & (1 << 4))
    info->cookie = update->cookie;
  if (update->change_mask & (1 << 5)) {
    if (info->props)
      pinos_spa_dict_destroy (info->props);
    info->props = pinos_spa_dict_copy (update->props);
  }
  return info;
}

void
pinos_core_info_free (PinosCoreInfo *info)
{
  if (info == NULL)
    return;

  if (info->user_name)
    free ((void*)info->user_name);
  if (info->host_name)
    free ((void*)info->host_name);
  if (info->version)
    free ((void*)info->version);
  if (info->name)
    free ((void*)info->name);
  if (info->props)
    pinos_spa_dict_destroy (info->props);
  free (info);
}

PinosNodeInfo *
pinos_node_info_update (PinosNodeInfo       *info,
                        const PinosNodeInfo *update)
{
  uint64_t change_mask;

  if (update == NULL)
    return info;

  if (info == NULL) {
    info = calloc (1, sizeof (PinosNodeInfo));
    if (info == NULL)
      return NULL;
    change_mask = ~0;
  } else {
    change_mask = info->change_mask | update->change_mask;
  }
  info->id = update->id;
  info->change_mask = change_mask;

  if (update->change_mask & (1 << 0)) {
    if (info->name)
      free ((void*)info->name);
    info->name = update->name ? strdup (update->name) : NULL;
  }
  if (update->change_mask & (1 << 1)) {
    info->state = update->state;
    if (info->error)
      free ((void*)info->error);
    info->error = update->error ? strdup (update->error) : NULL;
  }
  if (update->change_mask & (1 << 2)) {
    if (info->props)
      pinos_spa_dict_destroy (info->props);
    info->props = pinos_spa_dict_copy (update->props);
  }
  return info;
}

void
pinos_node_info_free (PinosNodeInfo *info)
{
  if (info == NULL)
    return;
  if (info->name)
    free ((void*)info->name);
  if (info->error)
    free ((void*)info->error);
  if (info->props)
    pinos_spa_dict_destroy (info->props);
  free (info);
}

PinosModuleInfo *
pinos_module_info_update (PinosModuleInfo       *info,
                          const PinosModuleInfo *update)
{
  uint64_t change_mask;

  if (update == NULL)
    return info;

  if (info == NULL) {
    info = calloc (1, sizeof (PinosModuleInfo));
    if (info == NULL)
      return NULL;
    change_mask = ~0;
  } else {
    change_mask = info->change_mask | update->change_mask;
  }
  info->id = update->id;
  info->change_mask = change_mask;

  if (update->change_mask & (1 << 0)) {
    if (info->name)
      free ((void*)info->name);
    info->name = update->name ? strdup (update->name) : NULL;
  }
  if (update->change_mask & (1 << 1)) {
    if (info->filename)
      free ((void*)info->filename);
    info->filename = update->filename ? strdup (update->filename) : NULL;
  }
  if (update->change_mask & (1 << 2)) {
    if (info->args)
      free ((void*)info->args);
    info->args = update->args ? strdup (update->args) : NULL;
  }
  if (update->change_mask & (1 << 3)) {
    if (info->props)
      pinos_spa_dict_destroy (info->props);
    info->props = pinos_spa_dict_copy (update->props);
  }
  return info;
}

void
pinos_module_info_free (PinosModuleInfo *info)
{
  if (info == NULL)
    return;

  if (info->name)
    free ((void*)info->name);
  if (info->filename)
    free ((void*)info->filename);
  if (info->args)
    free ((void*)info->args);
  if (info->props)
    pinos_spa_dict_destroy (info->props);
  free (info);
}


PinosClientInfo *
pinos_client_info_update (PinosClientInfo       *info,
                          const PinosClientInfo *update)
{
  uint64_t change_mask;

  if (update == NULL)
    return info;

  if (info == NULL) {
    info = calloc (1, sizeof (PinosClientInfo));
    if (info == NULL)
      return NULL;
    change_mask = ~0;
  } else {
    change_mask = info->change_mask | update->change_mask;
  }
  info->id = update->id;
  info->change_mask = change_mask;

  if (update->change_mask & (1 << 0)) {
    if (info->props)
      pinos_spa_dict_destroy (info->props);
    info->props = pinos_spa_dict_copy (update->props);
  }
  return info;
}

void
pinos_client_info_free (PinosClientInfo *info)
{
  if (info == NULL)
    return;
  if (info->props)
    pinos_spa_dict_destroy (info->props);
  free (info);
}

PinosLinkInfo *
pinos_link_info_update (PinosLinkInfo       *info,
                        const PinosLinkInfo *update)
{
  uint64_t change_mask;

  if (update == NULL)
    return info;

  if (info == NULL) {
    info = calloc (1, sizeof (PinosLinkInfo));
    if (info == NULL)
      return NULL;
    change_mask = ~0;
  } else {
    change_mask = info->change_mask | update->change_mask;
  }
  info->id = update->id;
  info->change_mask = change_mask;

  if (update->change_mask & (1 << 0))
    info->output_node_id = update->output_node_id;
  if (update->change_mask & (1 << 1))
    info->output_port_id = update->output_port_id;
  if (update->change_mask & (1 << 2))
    info->input_node_id = update->input_node_id;
  if (update->change_mask & (1 << 3))
    info->input_port_id = update->input_port_id;

  return info;
}

void
pinos_link_info_free (PinosLinkInfo *info)
{
  free (info);
}
