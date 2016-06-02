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

#ifndef __SPI_RESULT_H__
#define __SPI_RESULT_H__

G_BEGIN_DECLS

typedef enum {
  SPI_RESULT_OK                       =  0,
  SPI_RESULT_ERROR                    = -1,
  SPI_RESULT_INACTIVE                 = -2,
  SPI_RESULT_NO_FORMAT                = -3,
  SPI_RESULT_INVALID_COMMAND          = -4,
  SPI_RESULT_INVALID_PORT             = -5,
  SPI_RESULT_HAVE_ENOUGH_INPUT        = -6,
  SPI_RESULT_NEED_MORE_INPUT          = -7,
  SPI_RESULT_HAVE_EVENT               = -8,
  SPI_RESULT_PORTS_CHANGED            = -9,
  SPI_RESULT_FORMAT_CHANGED           = -10,
  SPI_RESULT_PROPERTIES_CHANGED       = -11,
  SPI_RESULT_NOT_IMPLEMENTED          = -12,
  SPI_RESULT_INVALID_PARAM_ID         = -13,
  SPI_RESULT_PARAM_UNSET              = -14,
  SPI_RESULT_NO_MORE_FORMATS          = -15,
  SPI_RESULT_WRONG_PARAM_TYPE         = -16,
  SPI_RESULT_INVALID_MEDIA_TYPE       = -17,
  SPI_RESULT_INVALID_FORMAT_PARAMS    = -18,
  SPI_RESULT_FORMAT_INCOMPLETE        = -19,
  SPI_RESULT_NO_MORE_PARAM_INFO       = -20,
  SPI_RESULT_INVALID_ARGUMENTS        = -21,
} SpiResult;

typedef void (*SpiNotify) (void *data);

G_END_DECLS

#endif /* __SPI_RESULT_H__ */
