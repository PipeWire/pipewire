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

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <lib/mapper.h>
#include <spa/format-utils.h>
#include <spa/loop.h>
#include "debug.h"

SpaResult
spa_debug_port_info (const SpaPortInfo *info, const SpaTypeMap *map)
{
  if (info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  fprintf (stderr, "SpaPortInfo %p:\n", info);
  fprintf (stderr, " flags: \t%08x\n", info->flags);

  return SPA_RESULT_OK;
}

SpaResult
spa_debug_buffer (const SpaBuffer *buffer, const SpaTypeMap *map)
{
  int i;

  if (buffer == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  fprintf (stderr, "SpaBuffer %p:\n", buffer);
  fprintf (stderr, " id:      %08X\n", buffer->id);
  fprintf (stderr, " n_metas: %u (at %p)\n", buffer->n_metas, buffer->metas);
  for (i = 0; i < buffer->n_metas; i++) {
    SpaMeta *m = &buffer->metas[i];
    const char *type_name;

    type_name = spa_type_map_get_type (map, m->type);
    fprintf (stderr, "  meta %d: type %d (%s), data %p, size %d:\n", i, m->type,
        type_name, m->data, m->size);

    if (!strcmp (type_name, SPA_TYPE_META__Header)) {
      SpaMetaHeader *h = m->data;
      fprintf (stderr, "    SpaMetaHeader:\n");
      fprintf (stderr, "      flags:      %08x\n", h->flags);
      fprintf (stderr, "      seq:        %u\n", h->seq);
      fprintf (stderr, "      pts:        %"PRIi64"\n", h->pts);
      fprintf (stderr, "      dts_offset: %"PRIi64"\n", h->dts_offset);
    }
    else if (!strcmp (type_name, SPA_TYPE_META__Pointer)) {
      SpaMetaPointer *h = m->data;
      fprintf (stderr, "    SpaMetaPointer:\n");
      fprintf (stderr, "      type:       %s\n", spa_type_map_get_type (map, h->type));
      fprintf (stderr, "      ptr:        %p\n", h->ptr);
    }
    else if (!strcmp (type_name, SPA_TYPE_META__VideoCrop)) {
      SpaMetaVideoCrop *h = m->data;
      fprintf (stderr, "    SpaMetaVideoCrop:\n");
      fprintf (stderr, "      x:      %d\n", h->x);
      fprintf (stderr, "      y:      %d\n", h->y);
      fprintf (stderr, "      width:  %d\n", h->width);
      fprintf (stderr, "      height: %d\n", h->height);
    }
    else if (!strcmp (type_name, SPA_TYPE_META__Ringbuffer)) {
      SpaMetaRingbuffer *h = m->data;
      fprintf (stderr, "    SpaMetaRingbuffer:\n");
      fprintf (stderr, "      readindex:   %d\n", h->ringbuffer.readindex);
      fprintf (stderr, "      writeindex:  %d\n", h->ringbuffer.writeindex);
      fprintf (stderr, "      size:        %d\n", h->ringbuffer.size);
      fprintf (stderr, "      mask:        %d\n", h->ringbuffer.mask);
    }
    else if (!strcmp (type_name, SPA_TYPE_META__Shared)) {
      SpaMetaShared *h = m->data;
      fprintf (stderr, "    SpaMetaShared:\n");
      fprintf (stderr, "      flags:  %d\n", h->flags);
      fprintf (stderr, "      fd:     %d\n", h->fd);
      fprintf (stderr, "      offset: %d\n", h->offset);
      fprintf (stderr, "      size:   %d\n", h->size);
    }
    else {
      fprintf (stderr, "    Unknown:\n");
      spa_debug_dump_mem (m->data, m->size);
    }
  }
  fprintf (stderr, " n_datas: \t%u (at %p)\n", buffer->n_datas, buffer->datas);
  for (i = 0; i < buffer->n_datas; i++) {
    SpaData *d = &buffer->datas[i];
    fprintf (stderr, "   type:    %d (%s)\n", d->type, spa_type_map_get_type (map, d->type));
    fprintf (stderr, "   flags:   %d\n", d->flags);
    fprintf (stderr, "   data:    %p\n", d->data);
    fprintf (stderr, "   fd:      %d\n", d->fd);
    fprintf (stderr, "   offset:  %d\n", d->mapoffset);
    fprintf (stderr, "   maxsize: %u\n", d->maxsize);
    fprintf (stderr, "   chunk:   %p\n", d->chunk);
    fprintf (stderr, "    offset: %d\n", d->chunk->offset);
    fprintf (stderr, "    size:   %u\n", d->chunk->size);
    fprintf (stderr, "    stride: %d\n", d->chunk->stride);
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_dump_mem (const void *mem, size_t size)
{
  const uint8_t *t = mem;
  int i;

  if (mem == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < size; i++) {
    if (i % 16 == 0)
      printf ("%p: ", &t[i]);
    printf ("%02x ", t[i]);
    if (i % 16 == 15 || i == size - 1)
      printf ("\n");
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_props (const SpaProps *props, const SpaTypeMap *map)
{
  spa_debug_pod (&props->pod, map);
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_param (const SpaParam *param, const SpaTypeMap *map)
{
  spa_debug_pod (&param->pod, map);
  return SPA_RESULT_OK;
}

struct pod_type_name {
  const char *name;
  const char *CCName;
} pod_type_names[] = {
  { "invalid", "*Invalid*" },
  { "none", "None" },
  { "bool", "Bool" },
  { "id", "Id" },
  { "int", "Int" },
  { "long", "Long" },
  { "float", "Float" },
  { "double", "Double" },
  { "string", "String" },
  { "bytes", "Bytes" },
  { "pointer", "Pointer" },
  { "rectangle", "Rectangle" },
  { "fraction", "Fraction" },
  { "bitmask", "Bitmask" },
  { "array", "Array" },
  { "struct", "Struct" },
  { "object", "Object" },
  { "prop", "Prop" },
};

static void
print_pod_value (const SpaTypeMap *map, uint32_t size, uint32_t type, void *body, int prefix)
{
  switch (type) {
    case SPA_POD_TYPE_BOOL:
      printf ("%-*sBool %d\n", prefix, "", *(int32_t *) body);
      break;
    case SPA_POD_TYPE_ID:
      printf ("%-*sId %d %s\n", prefix, "", *(int32_t *) body,
          spa_type_map_get_type (map, *(int32_t*)body));
      break;
    case SPA_POD_TYPE_INT:
      printf ("%-*sInt %d\n", prefix, "", *(int32_t *) body);
      break;
    case SPA_POD_TYPE_LONG:
      printf ("%-*sLong %"PRIi64"\n", prefix, "", *(int64_t *) body);
      break;
    case SPA_POD_TYPE_FLOAT:
      printf ("%-*sFloat %f\n", prefix, "", *(float *) body);
      break;
    case SPA_POD_TYPE_DOUBLE:
      printf ("%-*sDouble %f\n", prefix, "", *(double *) body);
      break;
    case SPA_POD_TYPE_STRING:
      printf ("%-*sString \"%s\"\n", prefix, "", (char *) body);
      break;
    case SPA_POD_TYPE_POINTER:
    {
      SpaPODPointerBody *b = body;
      printf ("%-*sPointer %s %p\n", prefix, "",
          spa_type_map_get_type (map, b->type), b->value);
      break;
    }
    case SPA_POD_TYPE_RECTANGLE:
    {
      SpaRectangle *r = body;
      printf ("%-*sRectangle %dx%d\n", prefix, "", r->width, r->height);
      break;
    }
    case SPA_POD_TYPE_FRACTION:
    {
      SpaFraction *f = body;
      printf ("%-*sFraction %d/%d\n", prefix, "", f->num, f->denom);
      break;
    }
    case SPA_POD_TYPE_BITMASK:
      printf ("%-*sBitmask\n", prefix, "");
      break;
    case SPA_POD_TYPE_ARRAY:
    {
      SpaPODArrayBody *b = body;
      void *p;
      printf ("%-*sArray: child.size %d, child.type %d\n", prefix, "", b->child.size, b->child.type);

      SPA_POD_ARRAY_BODY_FOREACH (b, size, p)
        print_pod_value (map, b->child.size, b->child.type, p, prefix + 2);
      break;
    }
    case SPA_POD_TYPE_STRUCT:
    {
      SpaPOD *b = body, *p;
      printf ("%-*sStruct: size %d\n", prefix, "", size);
      SPA_POD_FOREACH (b, size, p)
        print_pod_value (map, p->size, p->type, SPA_POD_BODY (p), prefix + 2);
      break;
    }
    case SPA_POD_TYPE_OBJECT:
    {
      SpaPODObjectBody *b = body;
      SpaPOD *p;

      printf ("%-*sObject: size %d, id %d, type %s\n", prefix, "", size, b->id,
          spa_type_map_get_type (map, b->type));
      SPA_POD_OBJECT_BODY_FOREACH (b, size, p)
        print_pod_value (map, p->size, p->type, SPA_POD_BODY (p), prefix + 2);
      break;
    }
    case SPA_POD_TYPE_PROP:
    {
      SpaPODPropBody *b = body;
      void *alt;
      int i;

      printf ("%-*sProp: key %s, flags %d\n", prefix, "",
          spa_type_map_get_type (map, b->key), b->flags);
      if (b->flags & SPA_POD_PROP_FLAG_UNSET)
        printf ("%-*sUnset (Default):\n", prefix + 2, "");
      else
        printf ("%-*sValue: size %u\n", prefix + 2, "", b->value.size);
      print_pod_value (map, b->value.size, b->value.type, SPA_POD_BODY (&b->value), prefix + 4);

      i = 0;
      SPA_POD_PROP_ALTERNATIVE_FOREACH (b, size, alt) {
        if (i == 0)
          printf ("%-*sAlternatives:\n", prefix + 2, "");
        print_pod_value (map, b->value.size, b->value.type, alt, prefix + 4);
        i++;
      }
      break;
    }
    case SPA_POD_TYPE_BYTES:
      printf ("%-*sBytes\n", prefix, "");
      spa_debug_dump_mem (body, size);
      break;
    case SPA_POD_TYPE_NONE:
      printf ("%-*sNone\n", prefix, "");
      spa_debug_dump_mem (body, size);
      break;
    default:
      printf ("unhandled POD type %d\n", type);
      break;
  }
}

SpaResult
spa_debug_pod (const SpaPOD *pod, const SpaTypeMap *map)
{
  map = map ? map : spa_type_map_get_default ();
  print_pod_value (map, pod->size, pod->type, SPA_POD_BODY (pod), 0);
  return SPA_RESULT_OK;
}

static void
print_format_value (const SpaTypeMap *map, uint32_t size, uint32_t type, void *body)
{
  switch (type) {
    case SPA_POD_TYPE_BOOL:
      fprintf (stderr, "%s", *(int32_t *) body ? "true" : "false");
      break;
    case SPA_POD_TYPE_ID:
    {
      const char *str = spa_type_map_get_type (map, *(int32_t*)body);
      if (str) {
        const char *h = rindex (str, ':');
        if (h)
          str = h + 1;
      } else {
        str = "unknown";
      }
      fprintf (stderr, "%s", str);
      break;
    }
    case SPA_POD_TYPE_INT:
      fprintf (stderr, "%"PRIi32, *(int32_t *) body);
      break;
    case SPA_POD_TYPE_LONG:
      fprintf (stderr, "%"PRIi64, *(int64_t *) body);
      break;
    case SPA_POD_TYPE_FLOAT:
      fprintf (stderr, "%f", *(float *) body);
      break;
    case SPA_POD_TYPE_DOUBLE:
      fprintf (stderr, "%g", *(double *) body);
      break;
    case SPA_POD_TYPE_STRING:
      fprintf (stderr, "%s", (char *) body);
      break;
    case SPA_POD_TYPE_RECTANGLE:
    {
      SpaRectangle *r = body;
      fprintf (stderr, "%"PRIu32"x%"PRIu32, r->width, r->height);
      break;
    }
    case SPA_POD_TYPE_FRACTION:
    {
      SpaFraction *f = body;
      fprintf (stderr, "%"PRIu32"/%"PRIu32, f->num, f->denom);
      break;
    }
    case SPA_POD_TYPE_BITMASK:
      fprintf (stderr, "Bitmask");
      break;
    case SPA_POD_TYPE_BYTES:
      fprintf (stderr, "Bytes");
      break;
    default:
      break;
  }
}

SpaResult
spa_debug_format (const SpaFormat *format, const SpaTypeMap *map)
{
  int i;
  const char *media_type;
  const char *media_subtype;
  SpaPODProp *prop;
  uint32_t mtype, mstype;

  if (format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  map = map ? map : spa_type_map_get_default ();

  mtype = format->body.media_type.value;
  mstype = format->body.media_subtype.value;

  media_type = spa_type_map_get_type (map, mtype);
  media_subtype = spa_type_map_get_type (map, mstype);

  fprintf (stderr, "%-6s %s/%s\n", "", rindex (media_type, ':')+1, rindex (media_subtype, ':')+1);

  SPA_FORMAT_FOREACH (format, prop) {
    const char *key;

    if ((prop->body.flags & SPA_POD_PROP_FLAG_UNSET) &&
        (prop->body.flags & SPA_POD_PROP_FLAG_OPTIONAL))
      continue;

    key = spa_type_map_get_type (map, prop->body.key);

    fprintf (stderr, "  %20s : (%s) ", rindex (key, ':')+1, pod_type_names[prop->body.value.type].name);

    if (!(prop->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
      print_format_value (map, prop->body.value.size,
                               prop->body.value.type,
                               SPA_POD_BODY (&prop->body.value));
    } else {
      const char *ssep, *esep, *sep;
      void *alt;

      switch (prop->body.flags & SPA_POD_PROP_RANGE_MASK) {
        case SPA_POD_PROP_RANGE_MIN_MAX:
        case SPA_POD_PROP_RANGE_STEP:
          ssep = "[ ";
          sep = ", ";
          esep = " ]";
          break;
        default:
        case SPA_POD_PROP_RANGE_ENUM:
        case SPA_POD_PROP_RANGE_FLAGS:
          ssep = "{ ";
          sep = ", ";
          esep = " }";
          break;
      }

      fprintf (stderr, "%s", ssep);

      i = 0;
      SPA_POD_PROP_ALTERNATIVE_FOREACH (&prop->body, prop->pod.size, alt) {
        if (i > 0)
          fprintf (stderr, "%s", sep);
        print_format_value (map, prop->body.value.size, prop->body.value.type, alt);
        i++;
      }
      fprintf (stderr, "%s", esep);
    }
    fprintf (stderr, "\n");
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_dict (const SpaDict *dict)
{
  unsigned int i;

  if (dict == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < dict->n_items; i++)
    fprintf (stderr, "          %s = \"%s\"\n", dict->items[i].key, dict->items[i].value);

  return SPA_RESULT_OK;
}


#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_INFO

#define TRACE_BUFFER 4096

typedef struct {
  SpaLog log;
  SpaRingbuffer  trace_rb;
  uint8_t        trace_data[TRACE_BUFFER];
  SpaSource     *source;
} DebugLog;

static void
do_logv (SpaLog        *log,
         SpaLogLevel    level,
         const char    *file,
         int            line,
         const char    *func,
         const char    *fmt,
         va_list        args)
{
  DebugLog *l = SPA_CONTAINER_OF (log, DebugLog, log);
  char text[512], location[1024];
  static const char *levels[] = { "-", "E", "W", "I", "D", "T", "*T*" };
  int size;
  bool do_trace;

  if ((do_trace = (level == SPA_LOG_LEVEL_TRACE && l->source)))
    level++;

  vsnprintf (text, sizeof(text), fmt, args);
  size = snprintf (location, sizeof(location), "[%s][%s:%i %s()] %s\n",
      levels[level], strrchr (file, '/')+1, line, func, text);

  if (SPA_UNLIKELY (do_trace)) {
    uint32_t index;
    uint64_t count = 1;

    spa_ringbuffer_get_write_index (&l->trace_rb, &index);
    spa_ringbuffer_write_data (&l->trace_rb, l->trace_data,
                                index & l->trace_rb.mask,
                                location, size);
    spa_ringbuffer_write_update (&l->trace_rb, index + size);

    write (l->source->fd, &count, sizeof(uint64_t));
  } else
    fputs (location, stderr);
}

static void
do_log (SpaLog        *log,
        SpaLogLevel    level,
        const char    *file,
        int            line,
        const char    *func,
        const char    *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  do_logv (log, level, file, line, func, fmt, args);
  va_end (args);
}

static DebugLog log = {
  { sizeof (SpaLog),
    NULL,
    DEFAULT_LOG_LEVEL,
    do_log,
    do_logv,
  },
  {  0, 0, TRACE_BUFFER, TRACE_BUFFER - 1 },
};

SpaLog *
spa_log_get_default (void)
{
  return &log.log;
}

static void
on_trace_event (SpaSource *source)
{
  int32_t avail;
  uint32_t index;
  uint64_t count;

  if (read (source->fd, &count, sizeof (uint64_t)) != sizeof (uint64_t))
    fprintf (stderr, "failed to read event fd: %s", strerror (errno));

  while ((avail = spa_ringbuffer_get_read_index (&log.trace_rb, &index)) > 0) {
    uint32_t offset, first;

    if (avail > log.trace_rb.size) {
      index += avail - log.trace_rb.size;
      avail = log.trace_rb.size;
    }
    offset = index & log.trace_rb.mask;
    first = SPA_MIN (avail, log.trace_rb.size - offset);

    fwrite (log.trace_data + offset, first, 1, stderr);
    if (SPA_UNLIKELY (avail > first)) {
      fwrite (log.trace_data, avail - first, 1, stderr);
    }
    spa_ringbuffer_read_update (&log.trace_rb, index + avail);
  }
}

void
spa_log_default_set_trace_event (SpaSource *source)
{
  log.source = source;
  log.source->func = on_trace_event;
  log.source->data = &log;
}
