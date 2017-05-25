/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_PLUGIN_H__
#define __SPA_PLUGIN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/dict.h>

#define SPA_TYPE__Handle            SPA_TYPE_INTERFACE_BASE "Handle"
#define SPA_TYPE__HandleFactory     SPA_TYPE_INTERFACE_BASE "HandleFactory"

struct spa_handle {
  /* user_data that can be set by the application */
  void * user_data;
  /**
   * spa_handle::get_interface:
   * @handle: a #spa_handle
   * @interface_id: the interface id
   * @interface: result to hold the interface.
   *
   * Get the interface provided by @handle with @interface_id.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_NOT_IMPLEMENTED when there are no extensions
   *          #SPA_RESULT_INVALID_ARGUMENTS when handle or info is %NULL
   */
  int   (*get_interface)        (struct spa_handle  *handle,
                                 uint32_t            interface_id,
                                 void              **interface);
  /**
   * spa_handle::clear
   * @handle: a pointer to memory
   *
   * Clean up the memory of @handle. After this, @handle should not be used
   * anymore.
   *
   * Returns: #SPA_RESULT_OK on success
   */
  int   (*clear)                (struct spa_handle   *handle);
};

#define spa_handle_get_interface(h,...)  (h)->get_interface((h),__VA_ARGS__)
#define spa_handle_clear(h)              (h)->clear((h))

/**
 * struct spa_interface_info:
 * @type: the type of the interface, can be used to get the interface
 *
 * This structure lists the information about available interfaces on
 * handles.
 */
struct spa_interface_info {
  const char *type;
};

/**
 * struct spa_support:
 * @type: the type of the support item
 * @data: specific data for the item
 *
 * Extra supporting infrastructure passed to the init() function of
 * a factory. It can be extra information or interfaces such as logging.
 */
struct spa_support {
  const char  *type;
  void        *data;
};

struct spa_handle_factory {
  /**
   * spa_handle_factory::name
   *
   * The name
   */
  const char * name;
  /**
   * spa_handle_factory::info
   *
   * Extra information about the handles of this factory.
   */
  const struct spa_dict * info;
  /**
   * spa_handle_factory::size
   *
   * The size of handles from this factory
   */
  const size_t size;

  /**
   * spa_handle_factory::init
   * @factory: a #spa_handle_factory
   * @handle: a pointer to memory
   * @info: extra handle specific information, usually obtained
   *        from a #spa_monitor. This can be used to configure the handle.
   * @support: support items
   * @n_support: number of elements in @support
   *
   * Initialize an instance of this factory. The caller should allocate
   * memory at least spa_handle_factory::size bytes and pass this as @handle.
   *
   * @support can optionally contain extra interfaces or data ites that the
   * plugin can use such as a logger.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_NOT_IMPLEMENTED when an instance can't be made
   *          #SPA_RESULT_INVALID_ARGUMENTS when factory or handle are %NULL
   */
  int   (*init)                 (const struct spa_handle_factory *factory,
                                 struct spa_handle               *handle,
                                 const struct spa_dict           *info,
                                 const struct spa_support        *support,
                                 uint32_t                         n_support);

  /**
   * spa_handle_factory::enum_interface_info:
   * @factory: a #spa_handle_factory
   * @info: result to hold spa_interface_info.
   * @index: index to keep track of the enumeration, 0 for first item
   *
   * Enumerate the interface information for @factory.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_NOT_IMPLEMENTED when there are no interfaces
   *          #SPA_RESULT_INVALID_ARGUMENTS when handle or info is %NULL
   *          #SPA_RESULT_ENUM_END when there are no more infos
   */
  int   (*enum_interface_info)  (const struct spa_handle_factory  *factory,
                                 const struct spa_interface_info **info,
                                 uint32_t                          index);
};

#define spa_handle_factory_init(h,...)                (h)->init((h),__VA_ARGS__)
#define spa_handle_factory_enum_interface_info(h,...) (h)->enum_interface_info((h),__VA_ARGS__)

/**
 * spa_handle_factory_enum_func_t:
 * @factory: a location to hold the factory result
 * @index: index to keep track of the enumeration
 *
 * The function signature of the entry point in a plugin.
 *
 * Returns: #SPA_RESULT_OK on success
 *          #SPA_RESULT_INVALID_ARGUMENTS when factory is %NULL
 *          #SPA_RESULT_ENUM_END when there are no more factories
 */
typedef int (*spa_handle_factory_enum_func_t) (const struct spa_handle_factory **factory,
                                               uint32_t                          index);

#define SPA_HANDLE_FACTORY_ENUM_FUNC_NAME "spa_handle_factory_enum"

/**
 * spa_handle_factory_enum:
 * @factory: a location to hold the factory result
 * @index: index to keep track of the enumeration
 *
 * The entry point in a plugin.
 *
 * Returns: #SPA_RESULT_OK on success
 *          #SPA_RESULT_INVALID_ARGUMENTS when factory is %NULL
 *          #SPA_RESULT_ENUM_END when there are no more factories
 */
int spa_handle_factory_enum       (const struct spa_handle_factory **factory,
                                   uint32_t                          index);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PLUGIN_H__ */
