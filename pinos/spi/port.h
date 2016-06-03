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

#ifndef __SPI_PORT_H__
#define __SPI_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spi/defs.h>

/**
 * SpiPortInfoFlags:
 * @SPI_PORT_INFO_FLAG_NONE: no flags
 * @SPI_PORT_INFO_FLAG_REMOVABLE: port can be removed
 * @SPI_PORT_INFO_FLAG_OPTIONAL: processing on port is optional
 * @SPI_PORT_INFO_FLAG_CAN_GIVE_BUFFER: the port can give a buffer
 * @SPI_PORT_INFO_FLAG_CAN_USE_BUFFER: the port can use a provided buffer
 * @SPI_PORT_INFO_FLAG_IN_PLACE: the port can process data in-place and will need
 *    a writable input buffer when no output buffer is specified.
 * @SPI_PORT_INFO_FLAG_NO_REF: the port does not keep a ref on the buffer
 */
typedef enum {
  SPI_PORT_INFO_FLAG_NONE                  = 0,
  SPI_PORT_INFO_FLAG_REMOVABLE             = 1 << 0,
  SPI_PORT_INFO_FLAG_OPTIONAL              = 1 << 1,
  SPI_PORT_INFO_FLAG_CAN_GIVE_BUFFER       = 1 << 2,
  SPI_PORT_INFO_FLAG_CAN_USE_BUFFER        = 1 << 3,
  SPI_PORT_INFO_FLAG_IN_PLACE              = 1 << 4,
  SPI_PORT_INFO_FLAG_NO_REF                = 1 << 5,
} SpiPortInfoFlags;

/**
 * SpiPortInfo
 * @flags: extra port flags
 * @size: minimum size of the buffers or 0 when not specified
 * @align: required alignment of the data
 * @maxbuffering: the maximum amount of bytes that the element will keep
 *                around internally
 * @latency: latency on this port in nanoseconds
 * @features: NULL terminated array of extra port features
 *
 */
typedef struct {
  SpiPortInfoFlags    flags;
  size_t              minsize;
  uint32_t            align;
  unsigned int        maxbuffering;
  uint64_t            latency;
  const char        **features;
} SpiPortInfo;

/**
 * SpiPortStatusFlags:
 * @SPI_PORT_STATUS_FLAG_NONE: no status flags
 * @SPI_PORT_STATUS_FLAG_HAVE_OUTPUT: port has output
 * @SPI_PORT_STATUS_FLAG_NEED_INPUT: port needs input
 */
typedef enum {
  SPI_PORT_STATUS_FLAG_NONE                  = 0,
  SPI_PORT_STATUS_FLAG_HAVE_OUTPUT           = 1 << 0,
  SPI_PORT_STATUS_FLAG_NEED_INPUT            = 1 << 1,
} SpiPortStatusFlags;

typedef struct {
  SpiPortStatusFlags   flags;
} SpiPortStatus;

#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPI_PORT_H__ */
