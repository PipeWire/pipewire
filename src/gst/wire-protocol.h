/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
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

#ifndef _GST_FDPAY_WIRE_PROTOCOL_H_
#define _GST_FDPAY_WIRE_PROTOCOL_H_

#include <stdint.h>

/* Almost the simplest possible FD passing protocol.  Each message should have
 * a file-descriptor attached which should be mmapable.  The data in the FD can
 * be found at offset and is size bytes long. */
typedef struct {
  uint64_t offset;
  uint64_t size;
} FDMessage;

#endif
