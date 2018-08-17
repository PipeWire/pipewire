/* PipeWire
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
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

#ifndef __PIPEWIRE_MODULE_H__
#define __PIPEWIRE_MODULE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/hook.h>

#include <pipewire/core.h>

#define PW_TYPE__Module           PW_TYPE_OBJECT_BASE "Module"
#define PW_TYPE_MODULE_BASE       PW_TYPE__Module ":"

#define PIPEWIRE_SYMBOL_MODULE_INIT "pipewire__module_init"

/** \class pw_module
 *
 * A dynamically loadable module
 */
struct pw_module;

/** Module init function signature
 *
 * \param module A \ref pw_module
 * \param args Arguments to the module
 * \return 0 on success, < 0 otherwise with an errno style error
 *
 * A module should provide an init function with this signature. This function
 * will be called when a module is loaded.
 *
 * \memberof pw_module
 */
typedef int (*pw_module_init_func_t) (struct pw_module *module, const char *args);

/** Module events added with \ref pw_module_add_listener */
struct pw_module_events {
#define PW_VERSION_MODULE_EVENTS	0
	uint32_t version;

	/** The module is destroyed */
	void (*destroy) (void *data);
};

/** The name of the module */
#define PW_MODULE_PROP_NAME	"pipewire.module.name"

struct pw_module *
pw_module_load(struct pw_core *core,
	       const char *name,		/**< name of the module */
	       const char *args			/**< arguments of the module */,
	       struct pw_client *owner,		/**< optional owner */
	       struct pw_global *parent,	/**< parent global */
	       struct pw_properties *properties	/**< extra global properties */);

/** Get the core of a module */
struct pw_core * pw_module_get_core(struct pw_module *module);

/** Get the global of a module */
struct pw_global * pw_module_get_global(struct pw_module *module);

/** Get the module info */
const struct pw_module_info *pw_module_get_info(struct pw_module *module);

/** Add an event listener to a module */
void pw_module_add_listener(struct pw_module *module,
			    struct spa_hook *listener,
			    const struct pw_module_events *events,
			    void *data);

/** Destroy a module */
void pw_module_destroy(struct pw_module *module);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_MODULE_H__ */
