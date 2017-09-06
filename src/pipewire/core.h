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

#ifndef __PIPEWIRE_CORE_H__
#define __PIPEWIRE_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/hook.h>
#include <spa/format.h>

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
#include <pipewire/global.h>
#include <pipewire/introspect.h>
#include <pipewire/loop.h>
#include <pipewire/node-factory.h>
#include <pipewire/port.h>
#include <pipewire/properties.h>
#include <pipewire/type.h>

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

#define PW_PERM_R	0400	/**< object can be seen and events can be received */
#define PW_PERM_W	0200	/**< methods can be called that modify the object */
#define PW_PERM_X	0100	/**< methods can be called on the object. The W flag must be
				  *  present in order to call methods that modify the object. */
#define PW_PERM_RWX	(PW_PERM_R|PW_PERM_W|PW_PERM_X)

/** the permission function. It returns the allowed access permissions for \a global
  * for \a client */
typedef uint32_t (*pw_permission_func_t) (struct pw_global *global,
					  struct pw_client *client, void *data);

#define PW_PERM_IS_R(p) (((p)&PW_PERM_R) == PW_PERM_R)
#define PW_PERM_IS_W(p) (((p)&PW_PERM_W) == PW_PERM_W)
#define PW_PERM_IS_X(p) (((p)&PW_PERM_X) == PW_PERM_X)

/** core events emited by the core object added with \ref pw_core_add_listener */
struct pw_core_events {
#define PW_VERSION_CORE_EVENTS	0
	uint32_t version;

	/** The core is being destroyed */
	void (*destroy) (void *data);
	/** The core is being freed */
	void (*free) (void *data);
	/** The core info changed,  use \ref pw_core_get_info() to get the updated info */
	void (*info_changed) (void *data, struct pw_core_info *info);
	/** a new global object was added */
	void (*global_added) (void *data, struct pw_global *global);
	/** a global object was removed */
	void (*global_removed) (void *data, struct pw_global *global);
};

/** Make a new core object for a given main_loop. Ownership of the properties is taken */
struct pw_core * pw_core_new(struct pw_loop *main_loop, struct pw_properties *props);

/** destroy a core object, all resources except the main_loop will be destroyed */
void pw_core_destroy(struct pw_core *core);

/** Add a new event listener to a core */
void pw_core_add_listener(struct pw_core *core,
			  struct spa_hook *listener,
			  const struct pw_core_events *events,
			  void *data);

/** Set a callback that will be called to check the permissions of a global
  * object for a client */
void pw_core_set_permission_callback(struct pw_core *core,
				     pw_permission_func_t callback,
				     void *data);

/** Get the type object of a core */
struct pw_type *pw_core_get_type(struct pw_core *core);

/** Get the core info object */
const struct pw_core_info *pw_core_get_info(struct pw_core *core);

/** Get the core global object */
struct pw_global *pw_core_get_global(struct pw_core *core);

/** Get the core properties */
const struct pw_properties *pw_core_get_properties(struct pw_core *core);

/** Update the core properties */
void pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict);

/** Get the core support objects */
const struct spa_support *pw_core_get_support(struct pw_core *core, uint32_t *n_support);

/** get the core main loop */
struct pw_loop *pw_core_get_main_loop(struct pw_core *core);

/** iterate the globals */
bool pw_core_for_each_global(struct pw_core *core,
			     bool (*callback) (void *data, struct pw_global *global),
			     void *data);

/** Find a core global by id */
struct pw_global *pw_core_find_global(struct pw_core *core, uint32_t id);

/** Find a good format between 2 ports */
struct spa_format *
pw_core_find_format(struct pw_core *core,
		    struct pw_port *output,
		    struct pw_port *input,
		    struct pw_properties *props,
		    uint32_t n_format_filters,
		    struct spa_format **format_filters,
		    char **error);

/** Find a ports compatible with \a other_port and the format filters */
struct pw_port *
pw_core_find_port(struct pw_core *core,
		  struct pw_port *other_port,
		  uint32_t id,
		  struct pw_properties *props,
		  uint32_t n_format_filters,
		  struct spa_format **format_filters,
		  char **error);

/** Find a node factory by name */
struct pw_node_factory *
pw_core_find_node_factory(struct pw_core *core, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CORE_H__ */
