/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PIPEWIRE_FACTORY_H
#define PIPEWIRE_FACTORY_H

#ifdef __cplusplus
extern "C" {
#endif

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
#define PW_VERSION_FACTORY_EVENTS	0
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

#endif /* PIPEWIRE_FACTORY_H */
