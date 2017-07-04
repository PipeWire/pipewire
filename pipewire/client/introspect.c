/* PipeWire
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

#include "pipewire/client/pipewire.h"

#include "pipewire/client/context.h"
#include "pipewire/client/subscribe.h"

const char *pw_node_state_as_string(enum pw_node_state state)
{
	switch (state) {
	case PW_NODE_STATE_ERROR:
		return "error";
	case PW_NODE_STATE_CREATING:
		return "creating";
	case PW_NODE_STATE_SUSPENDED:
		return "suspended";
	case PW_NODE_STATE_IDLE:
		return "idle";
	case PW_NODE_STATE_RUNNING:
		return "running";
	}
	return "invalid-state";
}

const char *pw_direction_as_string(enum pw_direction direction)
{
	switch (direction) {
	case PW_DIRECTION_INPUT:
		return "input";
	case PW_DIRECTION_OUTPUT:
		return "output";
	default:
		return "invalid";
	}
	return "invalid-direction";
}

const char *pw_link_state_as_string(enum pw_link_state state)
{
	switch (state) {
	case PW_LINK_STATE_ERROR:
		return "error";
	case PW_LINK_STATE_UNLINKED:
		return "unlinked";
	case PW_LINK_STATE_INIT:
		return "init";
	case PW_LINK_STATE_NEGOTIATING:
		return "negotiating";
	case PW_LINK_STATE_ALLOCATING:
		return "allocating";
	case PW_LINK_STATE_PAUSED:
		return "paused";
	case PW_LINK_STATE_RUNNING:
		return "running";
	}
	return "invalid-state";
}

static void pw_spa_dict_destroy(struct spa_dict *dict)
{
	struct spa_dict_item *item;

	spa_dict_for_each(item, dict) {
		free((void *) item->key);
		free((void *) item->value);
	}
	free(dict->items);
	free(dict);
}

static struct spa_dict *pw_spa_dict_copy(struct spa_dict *dict)
{
	struct spa_dict *copy;
	uint32_t i;

	if (dict == NULL)
		return NULL;

	copy = calloc(1, sizeof(struct spa_dict));
	if (copy == NULL)
		goto no_mem;
	copy->items = calloc(dict->n_items, sizeof(struct spa_dict_item));
	if (copy->items == NULL)
		goto no_items;
	copy->n_items = dict->n_items;

	for (i = 0; i < dict->n_items; i++) {
		copy->items[i].key = strdup(dict->items[i].key);
		copy->items[i].value = strdup(dict->items[i].value);
	}
	return copy;

      no_items:
	free(copy);
      no_mem:
	return NULL;
}

struct pw_core_info *pw_core_info_update(struct pw_core_info *info,
					 const struct pw_core_info *update)
{
	uint64_t change_mask;

	if (update == NULL)
		return info;

	if (info == NULL) {
		info = calloc(1, sizeof(struct pw_core_info));
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
			free((void *) info->user_name);
		info->user_name = update->user_name ? strdup(update->user_name) : NULL;
	}
	if (update->change_mask & (1 << 1)) {
		if (info->host_name)
			free((void *) info->host_name);
		info->host_name = update->host_name ? strdup(update->host_name) : NULL;
	}
	if (update->change_mask & (1 << 2)) {
		if (info->version)
			free((void *) info->version);
		info->version = update->version ? strdup(update->version) : NULL;
	}
	if (update->change_mask & (1 << 3)) {
		if (info->name)
			free((void *) info->name);
		info->name = update->name ? strdup(update->name) : NULL;
	}
	if (update->change_mask & (1 << 4))
		info->cookie = update->cookie;
	if (update->change_mask & (1 << 5)) {
		if (info->props)
			pw_spa_dict_destroy(info->props);
		info->props = pw_spa_dict_copy(update->props);
	}
	return info;
}

void pw_core_info_free(struct pw_core_info *info)
{
	if (info->user_name)
		free((void *) info->user_name);
	if (info->host_name)
		free((void *) info->host_name);
	if (info->version)
		free((void *) info->version);
	if (info->name)
		free((void *) info->name);
	if (info->props)
		pw_spa_dict_destroy(info->props);
	free(info);
}

struct pw_node_info *pw_node_info_update(struct pw_node_info *info,
					 const struct pw_node_info *update)
{
	uint64_t change_mask;
	int i;

	if (update == NULL)
		return info;

	if (info == NULL) {
		info = calloc(1, sizeof(struct pw_node_info));
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
			free((void *) info->name);
		info->name = update->name ? strdup(update->name) : NULL;
	}
	if (update->change_mask & (1 << 1)) {
		info->max_input_ports = update->max_input_ports;
		info->n_input_ports = update->n_input_ports;
	}
	if (update->change_mask & (1 << 2)) {
		for (i = 0; i < info->n_input_formats; i++)
			free(info->input_formats[i]);
		info->n_input_formats = update->n_input_formats;
		if (info->n_input_formats)
			info->input_formats =
			    realloc(info->input_formats,
				    info->n_input_formats * sizeof(struct spa_format *));
		else {
			free(info->input_formats);
			info->input_formats = NULL;
		}
		for (i = 0; i < info->n_input_formats; i++) {
			info->input_formats[i] = spa_format_copy(update->input_formats[i]);
		}
	}
	if (update->change_mask & (1 << 3)) {
		info->max_output_ports = update->max_output_ports;
		info->n_output_ports = update->n_output_ports;
	}
	if (update->change_mask & (1 << 4)) {
		for (i = 0; i < info->n_output_formats; i++)
			free(info->output_formats[i]);
		info->n_output_formats = update->n_output_formats;
		if (info->n_output_formats)
			info->output_formats =
			    realloc(info->output_formats,
				    info->n_output_formats * sizeof(struct spa_format *));
		else {
			free(info->output_formats);
			info->output_formats = NULL;
		}
		for (i = 0; i < info->n_output_formats; i++) {
			info->output_formats[i] = spa_format_copy(update->output_formats[i]);
		}
	}

	if (update->change_mask & (1 << 5)) {
		info->state = update->state;
		if (info->error)
			free((void *) info->error);
		info->error = update->error ? strdup(update->error) : NULL;
	}
	if (update->change_mask & (1 << 6)) {
		if (info->props)
			pw_spa_dict_destroy(info->props);
		info->props = pw_spa_dict_copy(update->props);
	}
	return info;
}

void pw_node_info_free(struct pw_node_info *info)
{
	int i;

	if (info->name)
		free((void *) info->name);
	if (info->input_formats) {
		for (i = 0; i < info->n_input_formats; i++)
			free(info->input_formats[i]);
		free(info->input_formats);
	}
	if (info->output_formats) {
		for (i = 0; i < info->n_output_formats; i++)
			free(info->output_formats[i]);
		free(info->output_formats);
	}
	if (info->error)
		free((void *) info->error);
	if (info->props)
		pw_spa_dict_destroy(info->props);
	free(info);
}

struct pw_module_info *pw_module_info_update(struct pw_module_info *info,
					     const struct pw_module_info *update)
{
	uint64_t change_mask;

	if (update == NULL)
		return info;

	if (info == NULL) {
		info = calloc(1, sizeof(struct pw_module_info));
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
			free((void *) info->name);
		info->name = update->name ? strdup(update->name) : NULL;
	}
	if (update->change_mask & (1 << 1)) {
		if (info->filename)
			free((void *) info->filename);
		info->filename = update->filename ? strdup(update->filename) : NULL;
	}
	if (update->change_mask & (1 << 2)) {
		if (info->args)
			free((void *) info->args);
		info->args = update->args ? strdup(update->args) : NULL;
	}
	if (update->change_mask & (1 << 3)) {
		if (info->props)
			pw_spa_dict_destroy(info->props);
		info->props = pw_spa_dict_copy(update->props);
	}
	return info;
}

void pw_module_info_free(struct pw_module_info *info)
{
	if (info->name)
		free((void *) info->name);
	if (info->filename)
		free((void *) info->filename);
	if (info->args)
		free((void *) info->args);
	if (info->props)
		pw_spa_dict_destroy(info->props);
	free(info);
}


struct pw_client_info *pw_client_info_update(struct pw_client_info *info,
					     const struct pw_client_info *update)
{
	uint64_t change_mask;

	if (update == NULL)
		return info;

	if (info == NULL) {
		info = calloc(1, sizeof(struct pw_client_info));
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
			pw_spa_dict_destroy(info->props);
		info->props = pw_spa_dict_copy(update->props);
	}
	return info;
}

void pw_client_info_free(struct pw_client_info *info)
{
	if (info->props)
		pw_spa_dict_destroy(info->props);
	free(info);
}

struct pw_link_info *pw_link_info_update(struct pw_link_info *info,
					 const struct pw_link_info *update)
{
	uint64_t change_mask;

	if (update == NULL)
		return info;

	if (info == NULL) {
		info = calloc(1, sizeof(struct pw_link_info));
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
	if (update->change_mask & (1 << 4)) {
		if (info->format)
			free(info->format);
		info->format = spa_format_copy(update->format);
	}
	return info;
}

void pw_link_info_free(struct pw_link_info *info)
{
	if (info->format)
		free(info->format);
	free(info);
}
