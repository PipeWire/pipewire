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

#ifndef __SPI_EVENT_H__
#define __SPI_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpiEvent SpiEvent;

#include <spi/defs.h>

typedef enum {
  SPI_EVENT_TYPE_INVALID                  = 0,
  SPI_EVENT_TYPE_ACTIVATED,
  SPI_EVENT_TYPE_DEACTIVATED,
  SPI_EVENT_TYPE_HAVE_OUTPUT,
  SPI_EVENT_TYPE_NEED_INPUT,
  SPI_EVENT_TYPE_REQUEST_DATA,
  SPI_EVENT_TYPE_DRAINED,
  SPI_EVENT_TYPE_MARKER,
  SPI_EVENT_TYPE_ERROR,
  SPI_EVENT_TYPE_BUFFERING,
} SpiEventType;

struct _SpiEvent {
  volatile int   refcount;
  SpiNotify      notify;
  SpiEventType   type;
  uint32_t       port_id;
  void          *data;
  size_t         size;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPI_EVENT_H__ */
