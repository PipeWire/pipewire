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

#include <pipewire/server/core.h>

struct pw_module {
  struct pw_core   *core;
  SpaList           link;
  struct pw_global *global;

  struct pw_module_info info;

  void *user_data;

  PW_SIGNAL (destroy_signal, (struct pw_listener *listener,
                              struct pw_module   *module));
};

/**
 * pw_module_init_func_t:
 * @module: A #struct pw_module
 * @args: Arguments to the module
 *
 * A module should provide an init function with this signature. This function
 * will be called when a module is loaded.
 *
 * Returns: %true on success, %false otherwise
 */
typedef bool (*pw_module_init_func_t) (struct pw_module *module, char *args);

struct pw_module * pw_module_load              (struct pw_core *core,
                                                const char     *name,
                                                const char     *args,
                                                char          **err);
void               pw_module_destroy           (struct pw_module *module);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_MODULE_H__ */
