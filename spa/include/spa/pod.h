/* Simple Plugin API
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_POD_H__
#define __SPA_POD_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SPA_POD_URI             "http://spaplug.in/ns/pod"
#define SPA_POD_PREFIX          SPA_POD_URI "#"

#include <stdarg.h>

#include <spa/defs.h>

/**
 * SpaPODType:
 */
typedef enum {
  SPA_POD_TYPE_INVALID         = 0,
  SPA_POD_TYPE_NONE            = 1,
  SPA_POD_TYPE_BOOL,
  SPA_POD_TYPE_URI,
  SPA_POD_TYPE_INT,
  SPA_POD_TYPE_LONG,
  SPA_POD_TYPE_FLOAT,
  SPA_POD_TYPE_DOUBLE,
  SPA_POD_TYPE_STRING,
  SPA_POD_TYPE_BYTES,
  SPA_POD_TYPE_POINTER,
  SPA_POD_TYPE_RECTANGLE,
  SPA_POD_TYPE_FRACTION,
  SPA_POD_TYPE_BITMASK,
  SPA_POD_TYPE_ARRAY,
  SPA_POD_TYPE_STRUCT,
  SPA_POD_TYPE_OBJECT,
  SPA_POD_TYPE_PROP,
  SPA_POD_TYPE_POD,
} SpaPODType;

typedef struct {
  uint32_t     size;
  uint32_t     type;
} SpaPOD;

typedef struct {
  SpaPOD       pod;
  int32_t      value;
  int32_t      __padding;
} SpaPODInt;

typedef SpaPODInt SpaPODBool;
typedef SpaPODInt SpaPODURI;

typedef struct {
  SpaPOD       pod;
  int64_t      value;
} SpaPODLong;

typedef struct {
  SpaPOD       pod;
  float        value;
} SpaPODFloat;

typedef struct {
  SpaPOD       pod;
  double       value;
} SpaPODDouble;

typedef struct {
  SpaPOD       pod;
  /* value here */
} SpaPODString;

typedef struct {
  SpaPOD       pod;
  /* value here */
} SpaPODBytes;

typedef struct {
  uint32_t     type;
  void        *value;
} SpaPODPointerBody;

typedef struct {
  SpaPOD            pod;
  SpaPODPointerBody body;
} SpaPODPointer;

typedef struct {
  SpaPOD       pod;
  SpaRectangle value;
} SpaPODRectangle;

typedef struct {
  SpaPOD       pod;
  SpaFraction  value;
} SpaPODFraction;

typedef struct {
  SpaPOD       pod;
  /* array of uint8_t follows with the bitmap */
} SpaPODBitmap;

typedef struct {
  SpaPOD    child;
  /* array with elements of child.size follows */
} SpaPODArrayBody;

typedef struct {
  SpaPOD           pod;
  SpaPODArrayBody  body;
} SpaPODArray;

typedef struct {
  SpaPOD           pod;
  /* one or more SpaPOD follow */
} SpaPODStruct;

typedef struct {
  uint32_t         key;
#define SPA_POD_PROP_RANGE_NONE         0
#define SPA_POD_PROP_RANGE_MIN_MAX      1
#define SPA_POD_PROP_RANGE_STEP         2
#define SPA_POD_PROP_RANGE_ENUM         3
#define SPA_POD_PROP_RANGE_FLAGS        4
#define SPA_POD_PROP_RANGE_MASK         0xf
#define SPA_POD_PROP_FLAG_UNSET         (1 << 4)
#define SPA_POD_PROP_FLAG_OPTIONAL      (1 << 5)
#define SPA_POD_PROP_FLAG_READABLE      (1 << 6)
#define SPA_POD_PROP_FLAG_WRITABLE      (1 << 7)
#define SPA_POD_PROP_FLAG_READWRITE     (SPA_POD_PROP_FLAG_READABLE | SPA_POD_PROP_FLAG_WRITABLE)
#define SPA_POD_PROP_FLAG_DEPRECATED    (1 << 8)
  uint32_t         flags;
  SpaPOD           value;
  /* array with elements of value.size follows,
   * first element is value/default, rest are alternatives */
} SpaPODPropBody;

typedef struct {
  SpaPOD         pod;
  SpaPODPropBody body;
} SpaPODProp;

typedef struct {
  uint32_t         id;
  uint32_t         type;
  /* contents follow, series of SpaPODProp */
} SpaPODObjectBody;

typedef struct {
  SpaPOD           pod;
  SpaPODObjectBody body;
} SpaPODObject;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_H__ */
