/* Simple Plugin Interface
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

#ifndef __SPI_PLUGIN_H__
#define __SPI_PLUGIN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spi/defs.h>
#include <spi/params.h>

typedef struct _SpiHandle SpiHandle;
typedef struct _SpiHandleFactory SpiHandleFactory;

struct _SpiHandle {
  /* user_data that can be set by the application */
  void * user_data;
  /**
   * SpiHandle::get_interface:
   * @handle: a #SpiHandle
   * @interface_id: the interface id
   * @interface: result to hold the interface.
   *
   * Get the interface provided by @handle with @interface_id.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_NOT_IMPLEMENTED when there are no extensions
   *          #SPI_RESULT_INVALID_ARGUMENTS when handle or info is %NULL
   */
  SpiResult   (*get_interface)        (SpiHandle               *handle,
                                       uint32_t                 interface_id,
                                       void                   **interface);
};

/**
 * SpiInterfaceInfo:
 * @interface_id: the id of the interface, can be used to get the interface
 * @name: name of the interface
 * @description: Human readable description of the interface.
 *
 * This structure lists the information about available interfaces on
 * handles.
 */
typedef struct {
  uint32_t    interface_id;
  const char *name;
  const char *description;
} SpiInterfaceInfo;

struct _SpiHandleFactory {
  /**
   * SpiHandleFactory::name
   *
   * The name
   */
  const char * name;
  /**
   * SpiHandleFactory::info
   *
   * Extra information about the handles of this factory.
   */
  const SpiParams * info;

  /**
   * SpiHandleFactory::instantiate
   * @factory: a #SpiHandleFactory
   * @handle: a pointer to hold the result
   *
   * Make an instance of this factory.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_NOT_IMPLEMENTED when an instance can't be made
   *          #SPI_RESULT_INVALID_ARGUMENTS when factory or handle are %NULL
   */
  SpiResult   (*instantiate)          (SpiHandleFactory  *factory,
                                       SpiHandle        **handle);
  /**
   * SpiHandle::enum_interface_info:
   * @factory: a #SpiHandleFactory
   * @index: the interface index
   * @info: result to hold SpiInterfaceInfo.
   *
   * Get the interface information at @index.
   *
   * Returns: #SPI_RESULT_OK on success
   *          #SPI_RESULT_NOT_IMPLEMENTED when there are no interfaces
   *          #SPI_RESULT_INVALID_ARGUMENTS when handle or info is %NULL
   *          #SPI_RESULT_ENUM_END when there are no more infos
   */
  SpiResult   (*enum_interface_info)  (SpiHandleFactory        *factory,
                                       unsigned int             index,
                                       const SpiInterfaceInfo **info);
};


/**
 * spi_enum_handle_factory:
 * @index: the index to enumerate
 * @factory: a location to hold the factory result
 *
 * The main entry point in a plugin.
 *
 * Returns: #SPI_RESULT_OK on success
 *          #SPI_RESULT_INVALID_ARGUMENTS when factory is %NULL
 *          #SPI_RESULT_ENUM_END when there are no more factories
 */
SpiResult  spi_enum_handle_factory (unsigned int             index,
                                    const SpiHandleFactory **factory);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPI_PLUGIN_H__ */
