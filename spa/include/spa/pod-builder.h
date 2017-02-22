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

#ifndef __SPA_POD_BUILDER_H__
#define __SPA_POD_BUILDER_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaPodFrame {
  struct _SpaPodFrame *parent;
  off_t                offset;
} SpaPODFrame;

typedef struct {
  void        *data;
  size_t       size;
  off_t        offset;
  SpaPODFrame *stack;
} SpaPODBuilder;

static inline uint32_t
spa_pod_builder_top (SpaPODBuilder *builder)
{
  if (builder->stack && builder->stack->offset != -1)
    return SPA_MEMBER(builder->data, builder->stack->offset, SpaPOD)->type;
  return SPA_POD_TYPE_INVALID;
}

static inline bool
spa_pod_builder_in_array (SpaPODBuilder *builder)
{
  if (builder->stack && builder->stack->offset != -1) {
    SpaPOD *p = SPA_MEMBER(builder->data, builder->stack->offset, SpaPOD);
    if (p->type == SPA_POD_TYPE_ARRAY && p->size > 0)
      return true;
    if (p->type == SPA_POD_TYPE_PROP && p->size > (sizeof (SpaPODPropBody) - sizeof(SpaPOD)))
      return true;
  }
  return false;
}

static inline off_t
spa_pod_builder_push (SpaPODBuilder *builder,
                      SpaPODFrame   *frame,
                      off_t          offset)
{
  frame->parent = builder->stack;
  frame->offset = offset;
  builder->stack = frame;
  return offset;
}

static inline void
spa_pod_builder_advance (SpaPODBuilder *builder, uint32_t size, bool pad)
{
  SpaPODFrame *f;

  if (pad) {
    size += SPA_ROUND_UP_N (builder->offset, 8) - builder->offset;
  }
  builder->offset += size;

  for (f = builder->stack; f; f = f->parent) {
    if (f->offset != -1)
      SPA_MEMBER (builder->data, f->offset, SpaPOD)->size += size;
  }
}

static inline void
spa_pod_builder_pop (SpaPODBuilder *builder,
                     SpaPODFrame   *frame)
{
  builder->stack = frame->parent;
  spa_pod_builder_advance (builder, 0, true);
}

static inline off_t
spa_pod_builder_raw (SpaPODBuilder *builder, const void *data, uint32_t size, bool pad)
{
  off_t offset = builder->offset;

  if (offset + size <= builder->size)
    memcpy (builder->data + offset, data, size);
  else
    offset = -1;

  spa_pod_builder_advance (builder, size, pad);

  return offset;
}

static inline off_t
spa_pod_builder_string_body (SpaPODBuilder *builder,
                             const char    *str,
                             uint32_t       len)
{
  off_t out = spa_pod_builder_raw (builder, str, len + 1 , true);
  if (out != -1)
    *SPA_MEMBER (builder->data, out + len, char) = '\0';
  return out;
}

static inline off_t
spa_pod_builder_pod (SpaPODBuilder *builder, uint32_t size, uint32_t type)
{
  const SpaPOD p = { size, type };
  return spa_pod_builder_raw (builder, &p, sizeof (p), false);
}

static inline off_t
spa_pod_builder_primitive (SpaPODBuilder *builder, const SpaPOD *p)
{
  const void *data;
  size_t size;
  bool pad;

  if (spa_pod_builder_in_array (builder)) {
    data = SPA_POD_BODY_CONST (p);
    size = SPA_POD_BODY_SIZE (p);
    pad = false;
  } else {
    data = p;
    size = SPA_POD_SIZE (p);
    pad = true;
  }
  return spa_pod_builder_raw (builder, data, size, pad);
}

static inline off_t
spa_pod_builder_bool (SpaPODBuilder *builder, bool val)
{
  const SpaPODBool p = { { sizeof (uint32_t), SPA_POD_TYPE_BOOL }, val ? 1 : 0 };
  return spa_pod_builder_primitive (builder, &p.pod);
}

static inline off_t
spa_pod_builder_int (SpaPODBuilder *builder, int32_t val)
{
  const SpaPODInt p = { { sizeof (val), SPA_POD_TYPE_INT }, val };
  return spa_pod_builder_primitive (builder, &p.pod);
}

static inline off_t
spa_pod_builder_long (SpaPODBuilder *builder, int64_t val)
{
  const SpaPODLong p = { { sizeof (val), SPA_POD_TYPE_LONG }, val };
  return spa_pod_builder_primitive (builder, &p.pod);
}

static inline off_t
spa_pod_builder_float (SpaPODBuilder *builder, float val)
{
  const SpaPODFloat p = { { sizeof (val), SPA_POD_TYPE_FLOAT }, val };
  return spa_pod_builder_primitive (builder, &p.pod);
}

static inline off_t
spa_pod_builder_double (SpaPODBuilder *builder, double val)
{
  const SpaPODDouble p = { { sizeof (val), SPA_POD_TYPE_DOUBLE }, val };
  return spa_pod_builder_primitive (builder, &p.pod);
}

static inline off_t
spa_pod_builder_string (SpaPODBuilder *builder, const char *str, uint32_t len)
{
  const SpaPODString p = { { len + 1, SPA_POD_TYPE_STRING } };
  off_t out = spa_pod_builder_raw (builder, &p, sizeof (p) , false);
  if (spa_pod_builder_string_body (builder, str, len) == -1)
    out = -1;
  return out;
}

static inline off_t
spa_pod_builder_rectangle (SpaPODBuilder *builder, SpaRectangle *val)
{
  const SpaPODRectangle p = { { sizeof (val), SPA_POD_TYPE_RECTANGLE }, *val };
  return spa_pod_builder_primitive (builder, &p.pod);
}

static inline off_t
spa_pod_builder_fraction (SpaPODBuilder *builder, SpaFraction *val)
{
  const SpaPODFraction p = { { sizeof (val), SPA_POD_TYPE_FRACTION }, *val };
  return spa_pod_builder_primitive (builder, &p.pod);
}

static inline off_t
spa_pod_builder_push_array (SpaPODBuilder  *builder,
                            SpaPODFrame    *frame)
{
  const SpaPODArray p = { { sizeof (SpaPODArrayBody) - sizeof (SpaPOD), SPA_POD_TYPE_ARRAY }, { { 0, 0 } } };
  return spa_pod_builder_push (builder, frame, spa_pod_builder_raw (builder, &p, sizeof(p) - sizeof(SpaPOD), false));
}

static inline off_t
spa_pod_builder_array (SpaPODBuilder *builder,
                       uint32_t       child_size,
                       uint32_t       child_type,
                       uint32_t       n_elems,
                       const void*    elems)
{
  const SpaPODArray p = {
    { (uint32_t)(sizeof (SpaPODArrayBody) + n_elems * child_size), SPA_POD_TYPE_ARRAY },
    { { child_size, child_type } }
  };
  off_t out = spa_pod_builder_raw (builder, &p, sizeof(p), true);
  if (spa_pod_builder_raw (builder, elems, child_size * n_elems, true) == -1)
    out = -1;
  return out;
}

static inline off_t
spa_pod_builder_push_struct (SpaPODBuilder  *builder,
                             SpaPODFrame    *frame)
{
  const SpaPODStruct p = { { 0, SPA_POD_TYPE_STRUCT } };
  return spa_pod_builder_push (builder, frame, spa_pod_builder_raw (builder, &p, sizeof(p), false));
}

static inline off_t
spa_pod_builder_push_object (SpaPODBuilder  *builder,
                             SpaPODFrame    *frame,
                             uint32_t        id,
                             uint32_t        type)
{
  const SpaPODObject p = { { sizeof (SpaPODObjectBody), SPA_POD_TYPE_OBJECT }, { id, type } };
  return spa_pod_builder_push (builder, frame, spa_pod_builder_raw (builder, &p, sizeof(p), false));
}

static inline off_t
spa_pod_builder_push_prop (SpaPODBuilder *builder,
                           SpaPODFrame   *frame,
                           uint32_t       key,
                           uint32_t       flags)
{
  const SpaPODProp p = { { sizeof (SpaPODPropBody) - sizeof(SpaPOD), SPA_POD_TYPE_PROP},
                         { key, flags | SPA_POD_PROP_RANGE_NONE, { 0, 0 } } };
  return spa_pod_builder_push (builder, frame, spa_pod_builder_raw (builder, &p, sizeof(p) - sizeof(SpaPOD), false));
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_BUILDER_H__ */
