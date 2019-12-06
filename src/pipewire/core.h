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

#ifndef PIPEWIRE_CORE_H
#define PIPEWIRE_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

/** \class pw_core
 *
 * \brief the core PipeWire object
 *
 * The core object manages all available resources.
 *
 * See \ref page_core_api
 */
struct pw_core;

#include <pipewire/client.h>
#include <pipewire/introspect.h>
#include <pipewire/interfaces.h>
#include <pipewire/core-proxy.h>
#include <pipewire/global.h>
#include <pipewire/loop.h>
#include <pipewire/factory.h>
#include <pipewire/properties.h>

/** \page page_core_api Core API
 *
 * \section page_core_overview Overview
 *
 * \subpage page_core
 *
 * \subpage page_global
 *
 * \subpage page_client
 *
 * \subpage page_resource
 *
 * \subpage page_node
 *
 * \subpage page_port
 *
 * \subpage page_link
 */

/** \page page_core Core
 *
 * \section page_core_overview Overview
 *
 * The core object is a singleton object that manages the state and
 * resources of a PipeWire instance.
 */

/** core events emited by the core object added with \ref pw_core_add_listener */
struct pw_core_events {
#define PW_VERSION_CORE_EVENTS	0
	uint32_t version;

	/** The core is being destroyed */
	void (*destroy) (void *data);
	/** The core is being freed */
	void (*free) (void *data);
	/** The core info changed,  use \ref pw_core_get_info() to get the updated info */
	void (*info_changed) (void *data, const struct pw_core_info *info);
	/** a new client object is added */
	void (*check_access) (void *data, struct pw_client *client);
	/** a new global object was added */
	void (*global_added) (void *data, struct pw_global *global);
	/** a global object was removed */
	void (*global_removed) (void *data, struct pw_global *global);
};

/** Make a new core object for a given main_loop. Ownership of the properties is taken */
struct pw_core * pw_core_new(struct pw_loop *main_loop,		/**< a main loop to run in */
			     struct pw_properties *props,	/**< extra properties */
			     size_t user_data_size		/**< extra user data size */);

/** destroy a core object, all resources except the main_loop will be destroyed */
void pw_core_destroy(struct pw_core *core);

/** Get the core user data */
void *pw_core_get_user_data(struct pw_core *core);

/** Add a new event listener to a core */
void pw_core_add_listener(struct pw_core *core,
			  struct spa_hook *listener,
			  const struct pw_core_events *events,
			  void *data);

/** Get the core info object */
const struct pw_core_info *pw_core_get_info(struct pw_core *core);

/** Get the core global object */
struct pw_global *pw_core_get_global(struct pw_core *core);

/** Get the core properties */
const struct pw_properties *pw_core_get_properties(struct pw_core *core);

/** Update the core properties */
int pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict);

/** Get the core support objects */
const struct spa_support *pw_core_get_support(struct pw_core *core, uint32_t *n_support);

/** get the core main loop */
struct pw_loop *pw_core_get_main_loop(struct pw_core *core);

/** Iterate the globals of the core. The callback should return
 * 0 to fetch the next item, any other value stops the iteration and returns
 * the value. When all callbacks return 0, this function returns 0 when all
 * globals are iterated. */
int pw_core_for_each_global(struct pw_core *core,	/**< the core */
			    int (*callback) (void *data, struct pw_global *global),
			    void *data);

/** Find a core global by id */
struct pw_global *pw_core_find_global(struct pw_core *core,	/**< the core */
				      uint32_t id		/**< the global id */);

/** Find a factory by name */
struct pw_factory *
pw_core_find_factory(struct pw_core *core	/**< the core */,
		     const char *name		/**< the factory name */);

/** add a spa library for the given factory_name regex */
int pw_core_add_spa_lib(struct pw_core *core, const char *factory_regex, const char *lib);

/** find the library name for a spa factory */
const char * pw_core_find_spa_lib(struct pw_core *core, const char *factory_name);

struct spa_handle *pw_core_load_spa_handle(struct pw_core *core,
		const char *factory_name,
		const struct spa_dict *info);


/** data for registering export functions */
struct pw_export_type {
	struct spa_list link;
	uint32_t type;
	struct pw_proxy * (*func) (struct pw_core_proxy *core_proxy,
		uint32_t type, struct pw_properties *properties, void *object,
		size_t user_data_size);
};

/** register a type that can be exported on a core_proxy. This is usually used by
 * extension modules */
int pw_core_register_export_type(struct pw_core *core, struct pw_export_type *type);
/** find information about registered export type */
const struct pw_export_type *pw_core_find_export_type(struct pw_core *core, uint32_t type);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CORE_H */
