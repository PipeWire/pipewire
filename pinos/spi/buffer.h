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

#ifndef __SPI_BUFFER_H__
#define __SPI_BUFFER_H__

G_BEGIN_DECLS

#include <inttypes.h>

#include <pinos/spi/result.h>
#include <pinos/spi/params.h>

typedef struct _SpiBuffer SpiBuffer;

typedef enum {
  SPI_META_TYPE_INVALID               = 0,
  SPI_META_TYPE_HEADER,
} SpiMetaType;

typedef struct {
  uint32_t flags;
  uint32_t seq;
  int64_t pts;
  int64_t dts_offset;
} SpiMetaHeader;

typedef struct {
  SpiMetaType  type;
  void        *data;
  size_t       size;
} SpiMeta;

/**
 * SpiDataType:
 * @SPI_DATA_TYPE_INVALID: invalid data type, is ignored
 * @SPI_DATA_TYPE_MEMPTR: data and size point to memory
 * @SPI_DATA_TYPE_FD: data points to SpiDataFd
 * @SPI_DATA_TYPE_FD: data points to SpiDataFd
 */
typedef enum {
  SPI_DATA_TYPE_INVALID               = 0,
  SPI_DATA_TYPE_MEMPTR,
  SPI_DATA_TYPE_FD,
} SpiDataType;

/**
 * SpiDataFd
 * fd: a file descriptor
 * offset: offset in the data referenced by @fd
 * @size: size of data referenced by fd
 */
typedef struct {
  int          fd;
  unsigned int offset;
  size_t       size;
} SpiDataFD;

/**
 * SpiData:
 * @id: user id
 * @type: the type of data
 * @data: pointer to data
 * @size: size of data
 */
typedef struct {
  SpiDataType  type;
  void        *data;
  size_t       size;
} SpiData;

/**
 * SpiBuffer:
 * @refcount: reference counter
 * @notify: called when the refcount reaches 0
 * @size: total size of the buffer
 * @n_metas: number of metadata
 * @metas: array of @n_metas metadata
 * @n_datas: number of data pointers
 * @datas: array of @n_datas data pointers
 */
struct _SpiBuffer {
  volatile int      refcount;
  SpiNotify         notify;
  size_t            size;
  unsigned int      n_metas;
  SpiMeta          *metas;
  unsigned int      n_datas;
  SpiData          *datas;
};

static inline SpiBuffer *
spi_buffer_ref (SpiBuffer *buffer)
{
  if (buffer != NULL)
    buffer->refcount++;
  return buffer;
}

static inline SpiBuffer *
spi_buffer_unref (SpiBuffer *buffer)
{
  if (buffer != NULL) {
    if (--buffer->refcount == 0) {
      buffer->notify (buffer);
      return NULL;
    }
  }
  return buffer;
}


#endif /* __SPI_BUFFER_H__ */
