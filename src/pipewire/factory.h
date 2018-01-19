/* PipeWire
 * Copyright (C) 2016 Axis Communications AB
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

#ifndef __PIPEWIRE_FACTORY_H__
#define __PIPEWIRE_FACTORY_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PW_TYPE_INTERFACE__Factory		PW_TYPE_INTERFACE_BASE "Factory"
#define PW_TYPE_FACTORY_BASE			PW_TYPE_INTERFACE__Factory ":"

/** \class pw_factory
 *
 * \brief PipeWire factory interface.
 *
 * The factory is used to make objects on demand.
 */
struct pw_factory;

#include <pipewire/core.h>
#include <pipewire/client.h>
#include <pipewire/global.h>
#include <pipewire/properties.h>
#include <pipewire/resource.h>

/** Factory events, listen to them with \ref pw_factory_add_listener */
struct pw_factory_events {
#define PW_VERSION_FACRORY_EVENTS	0
	uint32_t version;

	/** the factory is destroyed */
        void (*destroy) (void *data);
};

struct pw_factory_implementation {
#define PW_VERSION_FACTORY_IMPLEMENTATION	0
	uint32_t version;

	/** The function to create an object from this factory */
	void *(*create_object) (void *data,
				struct pw_resource *resource,
				uint32_t type,
				uint32_t version,
				struct pw_properties *properties,
				uint32_t new_id);
};

struct pw_factory *pw_factory_new(struct pw_core *core,
				  const char *name,
				  uint32_t type,
				  uint32_t version,
				  struct pw_properties *properties,
				  size_t user_data_size);

int pw_factory_register(struct pw_factory *factory,
			struct pw_client *owner,
			struct pw_global *parent,
			struct pw_properties *properties);

void pw_factory_destroy(struct pw_factory *factory);

void *pw_factory_get_user_data(struct pw_factory *factory);

/** Get the global of this factory */
struct pw_global *pw_factory_get_global(struct pw_factory *factory);

/** Add an event listener */
void pw_factory_add_listener(struct pw_factory *factory,
			     struct spa_hook *listener,
			     const struct pw_factory_events *events,
			     void *data);

void pw_factory_set_implementation(struct pw_factory *factory,
				   const struct pw_factory_implementation *implementation,
				   void *data);

void *pw_factory_create_object(struct pw_factory *factory,
			       struct pw_resource *resource,
			       uint32_t type,
			       uint32_t version,
			       struct pw_properties *properties,
			       uint32_t new_id);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_FACTORY_H__ */
