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

#include <spa/type-map.h>
#include <spa/event-node.h>
#include <spa/command-node.h>
#include <spa/monitor.h>
#include <spa/param-alloc.h>

#include <pipewire/map.h>
#include <pipewire/transport.h>

#define PIPEWIRE_TYPE__Core		"PipeWire:Object:Core"
#define PIPEWIRE_TYPE_CORE_BASE		PIPEWIRE_TYPE__Core ":"

#define PIPEWIRE_TYPE__Registry		"PipeWire:Object:Registry"
#define PIPEWIRE_TYPE_REGISYRY_BASE	PIPEWIRE_TYPE__Registry ":"

#define PIPEWIRE_TYPE__Node		"PipeWire:Object:Node"
#define PIPEWIRE_TYPE_NODE_BASE		PIPEWIRE_TYPE__Node ":"

#define PIPEWIRE_TYPE__Client		"PipeWire:Object:Client"
#define PIPEWIRE_TYPE_CLIENT_BASE	PIPEWIRE_TYPE__Client ":"

#define PIPEWIRE_TYPE__Link		"PipeWire:Object:Link"
#define PIPEWIRE_TYPE_LINK_BASE		PIPEWIRE_TYPE__Link ":"

#define PIPEWIRE_TYPE__Module		"PipeWire:Object:Module"
#define PIPEWIRE_TYPE_MODULE_BASE	PIPEWIRE_TYPE__Module ":"

#define PW_TYPE__Protocol		"PipeWire:Protocol"
#define PW_TYPE_PROTOCOL_BASE		PW_TYPE__Protocol ":"

/** \class pw_interface
 * \brief The interface definition
 *
 * The interface implements the methods and events for a \ref
 * pw_proxy. It typically implements marshal functions for the
 * methods and calls the user installed implementation after
 * demarshalling the events.
 *
 * \sa pw_proxy, pw_resource
 */
struct pw_interface {
	const char *type;	/**< interface type */
	uint32_t version;	/**< version */
	uint32_t n_methods;	/**< number of methods in the interface */
	const void *methods;	/**< method implementations of the interface */
	uint32_t n_events;	/**< number of events in the interface */
	const void *events;	/**< event implementations of the interface */
};

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
	uint32_t node_factory;
	uint32_t link;
	uint32_t client;
	uint32_t module;

	uint32_t spa_log;
	uint32_t spa_node;
	uint32_t spa_clock;
	uint32_t spa_monitor;
	uint32_t spa_format;
	uint32_t spa_props;

	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_monitor monitor;
	struct spa_type_param_alloc_buffers param_alloc_buffers;
	struct spa_type_param_alloc_meta_enable param_alloc_meta_enable;
	struct spa_type_param_alloc_video_padding param_alloc_video_padding;
	struct pw_type_event_transport event_transport;
};

void
pw_type_init(struct pw_type *type);

bool
pw_pod_remap_data(uint32_t type, void *body, uint32_t size, struct pw_map *types);

static inline bool
pw_pod_remap(struct spa_pod *pod, struct pw_map *types)
{
	return pw_pod_remap_data(pod->type, SPA_POD_BODY(pod), pod->size, types);
}

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_TYPE_H__ */
