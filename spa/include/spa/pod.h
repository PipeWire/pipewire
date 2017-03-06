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

#include <spa/defs.h>

/**
 * SpaPODType:
 */
typedef enum {
  SPA_POD_TYPE_INVALID         = 0,
  SPA_POD_TYPE_BOOL,
  SPA_POD_TYPE_URI,
  SPA_POD_TYPE_INT,
  SPA_POD_TYPE_LONG,
  SPA_POD_TYPE_FLOAT,
  SPA_POD_TYPE_DOUBLE,
  SPA_POD_TYPE_STRING,
  SPA_POD_TYPE_RECTANGLE,
  SPA_POD_TYPE_FRACTION,
  SPA_POD_TYPE_BITMASK,
  SPA_POD_TYPE_ARRAY,
  SPA_POD_TYPE_STRUCT,
  SPA_POD_TYPE_OBJECT,
  SPA_POD_TYPE_PROP,
  SPA_POD_TYPE_BYTES
} SpaPODType;

typedef struct {
  uint32_t     size;
  uint32_t     type;
} SpaPOD;

#define SPA_POD_BODY_SIZE(pod)           (((SpaPOD*)(pod))->size)
#define SPA_POD_SIZE(pod)                (sizeof(SpaPOD) + SPA_POD_BODY_SIZE(pod))
#define SPA_POD_CONTENTS_SIZE(type,pod)  (SPA_POD_SIZE(pod)-sizeof(type))

#define SPA_POD_CONTENTS(type,pod)       SPA_MEMBER((pod),sizeof(type),void)
#define SPA_POD_CONTENTS_CONST(type,pod) SPA_MEMBER((pod),sizeof(type),const void)
#define SPA_POD_BODY(pod)                SPA_MEMBER((pod),sizeof(SpaPOD),void)
#define SPA_POD_BODY_CONST(pod)          SPA_MEMBER((pod),sizeof(SpaPOD),const void)

typedef struct {
  SpaPOD       pod;
  int32_t      value;
} SpaPODInt;

static inline bool
spa_pod_get_int (SpaPOD **pod, int32_t *val)
{
  if (*pod == NULL || (*pod)->type != SPA_POD_TYPE_INT)
    return false;
  *val = ((SpaPODInt *)(*pod))->value;
  *pod = SPA_MEMBER (*pod, SPA_ROUND_UP_N (SPA_POD_SIZE (*pod), 8), SpaPOD);
  return true;
}

typedef SpaPODInt SpaPODBool;
typedef SpaPODInt SpaPODURI;

typedef struct {
  SpaPOD       pod;
  int64_t      value;
} SpaPODLong;

static inline bool
spa_pod_get_long (SpaPOD **pod, int64_t *val)
{
  if (*pod == NULL || (*pod)->type != SPA_POD_TYPE_LONG)
    return false;
  *val = ((SpaPODLong *)*pod)->value;
  *pod = SPA_MEMBER (*pod, SPA_ROUND_UP_N (SPA_POD_SIZE (*pod), 8), SpaPOD);
  return true;
}

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

static inline bool
spa_pod_get_string (SpaPOD **pod, const char **val)
{
  if (*pod == NULL || (*pod)->type != SPA_POD_TYPE_STRING)
    return false;
  *val = SPA_POD_CONTENTS (SpaPODString, *pod);
  *pod = SPA_MEMBER (*pod, SPA_ROUND_UP_N (SPA_POD_SIZE (*pod), 8), SpaPOD);
  return true;
}

typedef struct {
  SpaPOD       pod;
  /* value here */
} SpaPODBytes;

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
  /* array of uint32_t follows with the bitmap */
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

static inline bool
spa_pod_get_struct (SpaPOD **pod, SpaPOD **val)
{
  if (*pod == NULL || (*pod)->type != SPA_POD_TYPE_STRUCT)
    return false;
  *val = *pod;
  *pod = SPA_MEMBER (*pod, SPA_ROUND_UP_N (SPA_POD_SIZE (*pod), 8), SpaPOD);
  return true;
}

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

#define SPA_POD_PROP_N_VALUES(prop) (((prop)->pod.size - sizeof (SpaPODPropBody)) / (prop)->body.value.size)

typedef struct {
  uint32_t         id;
  uint32_t         type;
  /* contents follow, series of SpaPODProp */
} SpaPODObjectBody;

typedef struct {
  SpaPOD           pod;
  SpaPODObjectBody body;
} SpaPODObject;

static inline bool
spa_pod_get_object (SpaPOD **pod, const SpaPOD **val)
{
  if (*pod == NULL || (*pod)->type != SPA_POD_TYPE_OBJECT)
    return false;
  *val = *pod;
  *pod = SPA_MEMBER (*pod, SPA_ROUND_UP_N (SPA_POD_SIZE (*pod), 8), SpaPOD);
  return true;
}

static inline bool
spa_pod_get_bytes (SpaPOD **pod, const void **val, uint32_t *size)
{
  if (*pod == NULL || (*pod)->type != SPA_POD_TYPE_BYTES)
    return false;
  *val = SPA_POD_CONTENTS (SpaPODBytes, *pod);
  *size = SPA_POD_SIZE (*pod);
  *pod = SPA_MEMBER (*pod, SPA_ROUND_UP_N (SPA_POD_SIZE (*pod), 8), SpaPOD);
  return true;
}


#define SPA_POD_ARRAY_BODY_FOREACH(body, size, iter) \
  for ((iter) = SPA_MEMBER (body, sizeof(SpaPODArrayBody), __typeof__(*iter)); \
       (iter) < SPA_MEMBER (body, (size), __typeof__(*iter)); \
       (iter) = SPA_MEMBER ((iter), (body)->child.size, __typeof__(*iter)))

#define SPA_POD_FOREACH(pod, size, iter) \
  for ((iter) = (pod); \
       (iter) < SPA_MEMBER ((pod), (size), SpaPOD); \
       (iter) = SPA_MEMBER ((iter), SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), SpaPOD))

#define SPA_POD_CONTENTS_FOREACH(pod, offset, iter) \
  SPA_POD_FOREACH(SPA_MEMBER ((pod), (offset), SpaPOD),SPA_POD_SIZE (pod),iter)

#define SPA_POD_OBJECT_BODY_FOREACH(body, size, iter) \
  for ((iter) = SPA_MEMBER ((body), sizeof (SpaPODObjectBody), SpaPODProp); \
       (iter) < SPA_MEMBER ((body), (size), SpaPODProp); \
       (iter) = SPA_MEMBER ((iter), SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), SpaPODProp))

#define SPA_POD_OBJECT_FOREACH(obj, iter) \
  SPA_POD_OBJECT_BODY_FOREACH(&obj->body, SPA_POD_BODY_SIZE(obj), iter)

#define SPA_POD_PROP_ALTERNATIVE_FOREACH(body, _size, iter) \
  for ((iter) = SPA_MEMBER ((body), (body)->value.size + sizeof (SpaPODPropBody), __typeof__(*iter)); \
       (iter) < SPA_MEMBER ((body), (_size), __typeof__(*iter)); \
       (iter) = SPA_MEMBER ((iter), (body)->value.size, __typeof__(*iter)))

static inline SpaPODProp *
spa_pod_contents_find_prop (const SpaPOD *pod, off_t offset, uint32_t key)
{
  SpaPOD *res;
  SPA_POD_CONTENTS_FOREACH (pod, offset, res) {
    if (res->type == SPA_POD_TYPE_PROP && ((SpaPODProp*)res)->body.key == key)
      return (SpaPODProp *)res;
  }
  return NULL;
}

static inline SpaPODProp *
spa_pod_object_find_prop (const SpaPODObject *obj, uint32_t key)
{
  return spa_pod_contents_find_prop (&obj->pod, sizeof (SpaPODObject), key);
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_H__ */
