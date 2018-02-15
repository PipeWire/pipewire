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

#ifndef __PIPEWIRE_TYPE_H__
#define __PIPEWIRE_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/type-map.h>
#include <spa/node/event.h>
#include <spa/node/command.h>
#include <spa/monitor/monitor.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/io.h>
#include <spa/node/io.h>

#include <pipewire/map.h>

#define PW_TYPE_BASE		"PipeWire:"

#define PW_TYPE__Object		PW_TYPE_BASE "Object"
#define PW_TYPE_OBJECT_BASE	PW_TYPE__Object ":"

#define PW_TYPE__Interface	PW_TYPE_BASE "Interface"
#define PW_TYPE_INTERFACE_BASE	PW_TYPE__Interface ":"

/** \class pw_type
 * \brief PipeWire type support struct
 *
 * This structure contains some of the most common types
 * and should be initialized with \ref pw_type_init() */
struct pw_type {
	struct spa_type_map *map;	/**< the type mapper */

	uint32_t core;
	uint32_t registry;
	uint32_t node;
	uint32_t port;
	uint32_t factory;
	uint32_t link;
	uint32_t client;
	uint32_t module;

	uint32_t spa_log;
	uint32_t spa_node;
	uint32_t spa_clock;
	uint32_t spa_monitor;
	uint32_t spa_format;
	uint32_t spa_props;

	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_monitor monitor;
	struct spa_type_param_buffers param_buffers;
	struct spa_type_param_meta param_meta;
	struct spa_type_param_io param_io;
};

int pw_type_init(struct pw_type *type);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_TYPE_H__ */
