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

#include "spa/debug.h"
#include "spa/props.h"
#include "spa/format.h"

struct meta_type_name {
  const char *name;
} meta_type_names[] = {
  { "invalid" },
  { "SpaMetaHeader" },
  { "SpaMetaPointer" },
  { "SpaMetaVideoCrop" },
  { "SpaMetaRingbuffer" },
  { "invalid" },
};
#define META_TYPE_NAME(t)  meta_type_names[SPA_CLAMP(t,0,SPA_N_ELEMENTS(meta_type_names)-1)].name

struct data_type_name {
  const char *name;
} data_type_names[] = {
  { "invalid" },
  { "memptr" },
  { "memfd" },
  { "dmabuf" },
  { "ID" },
  { "invalid" },
};
#define DATA_TYPE_NAME(t)  data_type_names[SPA_CLAMP(t,0,SPA_N_ELEMENTS(data_type_names)-1)].name

SpaResult
spa_debug_port_info (const SpaPortInfo *info)
{
  int i;

  if (info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  fprintf (stderr, "SpaPortInfo %p:\n", info);
  fprintf (stderr, " flags: \t%08x\n", info->flags);
  fprintf (stderr, " maxbuffering: \t%"PRIu64"\n", info->maxbuffering);
  fprintf (stderr, " latency: \t%" PRIu64 "\n", info->latency);
  fprintf (stderr, " n_params: \t%d\n", info->n_params);
  for (i = 0; i < info->n_params; i++) {
    SpaAllocParam *param = info->params[i];
    fprintf (stderr, " param %d, type %d, size %zd:\n", i, param->type, param->size);
    switch (param->type) {
      case SPA_ALLOC_PARAM_TYPE_INVALID:
        fprintf (stderr, "   INVALID\n");
        break;
      case SPA_ALLOC_PARAM_TYPE_BUFFERS:
      {
        SpaAllocParamBuffers *p = (SpaAllocParamBuffers *)param;
        fprintf (stderr, "   SpaAllocParamBuffers:\n");
        fprintf (stderr, "    minsize: \t\t%zd\n", p->minsize);
        fprintf (stderr, "    stride: \t\t%zd\n", p->stride);
        fprintf (stderr, "    min_buffers: \t%d\n", p->min_buffers);
        fprintf (stderr, "    max_buffers: \t%d\n", p->max_buffers);
        fprintf (stderr, "    align: \t\t%d\n", p->align);
        break;
      }
      case SPA_ALLOC_PARAM_TYPE_META_ENABLE:
      {
        SpaAllocParamMetaEnable *p = (SpaAllocParamMetaEnable *)param;
        fprintf (stderr, "   SpaAllocParamMetaEnable:\n");
        fprintf (stderr, "    type: \t%d (%s)\n", p->type, META_TYPE_NAME(p->type));
        break;
      }
      case SPA_ALLOC_PARAM_TYPE_VIDEO_PADDING:
      {
        SpaAllocParamVideoPadding *p = (SpaAllocParamVideoPadding *)param;
        fprintf (stderr, "   SpaAllocParamVideoPadding:\n");
        fprintf (stderr, "    padding_top: \t%d\n", p->padding_top);
        fprintf (stderr, "    padding_bottom: \t%d\n", p->padding_bottom);
        fprintf (stderr, "    padding_left: \t%d\n", p->padding_left);
        fprintf (stderr, "    padding_right: \t%d\n", p->padding_right);
        fprintf (stderr, "    stide_align: \t[%d, %d, %d, %d]\n",
            p->stride_align[0], p->stride_align[1], p->stride_align[2], p->stride_align[3]);
        break;
      }
      default:
        fprintf (stderr, "   UNKNOWN\n");
        break;
    }
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_buffer (const SpaBuffer *buffer)
{
  int i;

  if (buffer == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  fprintf (stderr, "SpaBuffer %p:\n", buffer);
  fprintf (stderr, " id:      %08X\n", buffer->id);
  fprintf (stderr, " n_metas: %u (at %p)\n", buffer->n_metas, buffer->metas);
  for (i = 0; i < buffer->n_metas; i++) {
    SpaMeta *m = &buffer->metas[i];
    fprintf (stderr, "  meta %d: type %d (%s), data %p, size %zd:\n", i, m->type, META_TYPE_NAME (m->type), m->data, m->size);
    switch (m->type) {
      case SPA_META_TYPE_HEADER:
      {
        SpaMetaHeader *h = m->data;
        fprintf (stderr, "    SpaMetaHeader:\n");
        fprintf (stderr, "      flags:      %08x\n", h->flags);
        fprintf (stderr, "      seq:        %u\n", h->seq);
        fprintf (stderr, "      pts:        %"PRIi64"\n", h->pts);
        fprintf (stderr, "      dts_offset: %"PRIi64"\n", h->dts_offset);
        break;
      }
      case SPA_META_TYPE_POINTER:
      {
        SpaMetaPointer *h = m->data;
        fprintf (stderr, "    SpaMetaPointer:\n");
        fprintf (stderr, "      ptr_type:   %s\n", h->ptr_type);
        fprintf (stderr, "      ptr:        %p\n", h->ptr);
        spa_debug_dump_mem (m->data, m->size);
        break;
      }
      case SPA_META_TYPE_VIDEO_CROP:
      {
        SpaMetaVideoCrop *h = m->data;
        fprintf (stderr, "    SpaMetaVideoCrop:\n");
        fprintf (stderr, "      x:      %d\n", h->x);
        fprintf (stderr, "      y:      %d\n", h->y);
        fprintf (stderr, "      width:  %d\n", h->width);
        fprintf (stderr, "      height: %d\n", h->height);
        break;
      }
      case SPA_META_TYPE_RINGBUFFER:
      {
        SpaMetaRingbuffer *h = m->data;
        fprintf (stderr, "    SpaMetaRingbuffer:\n");
        fprintf (stderr, "      readindex:   %d\n", h->readindex);
        fprintf (stderr, "      writeindex:  %d\n", h->writeindex);
        fprintf (stderr, "      size:        %d\n", h->size);
        fprintf (stderr, "      size_mask:   %d\n", h->size_mask);
        break;
      }
      default:
        spa_debug_dump_mem (m->data, m->size);
        break;
    }
  }
  fprintf (stderr, " n_datas: \t%u (at %p)\n", buffer->n_datas, buffer->datas);
  for (i = 0; i < buffer->n_datas; i++) {
    SpaData *d = &buffer->datas[i];
    fprintf (stderr, "   type:    %d (%s)\n", d->type, DATA_TYPE_NAME (d->type));
    fprintf (stderr, "   data:    %p\n", d->data);
    fprintf (stderr, "   offset:  %zd\n", d->offset);
    fprintf (stderr, "   size:    %zd\n", d->size);
    fprintf (stderr, "   maxsize: %zd\n", d->maxsize);
    fprintf (stderr, "   stride:  %zd\n", d->stride);
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

struct media_type_name {
  const char *name;
  unsigned int first;
  unsigned int last;
  unsigned int idx;
} media_type_names[] = {
  { "invalid", 0, 0, 0 },
  { "audio", SPA_MEDIA_SUBTYPE_AUDIO_FIRST, SPA_MEDIA_SUBTYPE_AUDIO_LAST, 16 },
  { "video", SPA_MEDIA_SUBTYPE_VIDEO_FIRST, SPA_MEDIA_SUBTYPE_VIDEO_LAST, 2 },
  { "image", 0, 0 },
};

struct media_subtype_name {
  const char *name;
} media_subtype_names[] = {
  { "invalid" },

  { "raw" },

  { "h264" },
  { "mjpg" },
  { "dv" },
  { "mpegts" },
  { "h263" },
  { "mpeg1" },
  { "mpeg2" },
  { "mpeg4" },
  { "xvid" },
  { "vc1" },
  { "vp8" },
  { "vp9" },
  { "jpeg" },
  { "bayer" },

  { "mp3" },
  { "aac" },
  { "vorbis" },
  { "wma" },
  { "ra" },
  { "sbc" },
  { "adpcm" },
  { "g723" },
  { "g726" },
  { "g729" },
  { "amr" },
  { "gsm" },
};

struct prop_type_name {
  const char *name;
  const char *CCName;
} prop_type_names[] = {
  { "invalid", "*Invalid*" },
  { "bool", "Boolean" },
  { "int8", "Int8" },
  { "uint8", "UInt8" },
  { "int16", "Int16" },
  { "uint16", "UInt16" },
  { "int32", "Int32" },
  { "uint32", "UInt32" },
  { "int64", "Int64" },
  { "uint64", "UInt64" },
  { "int", "Int" },
  { "uint", "UInt" },
  { "float", "Float" },
  { "double", "Double" },
  { "string", "String" },
  { "rectangle", "Rectangle" },
  { "fraction", "Fraction" },
  { "bitmask", "Bitmask" },
  { "pointer", "Pointer" },
};

static void
print_value (const SpaPropInfo *info, const SpaPropValue *val)
{
  SpaPropType type = info->type;
  bool enum_string = false;
  const void *enum_value;

  if (info->range_type == SPA_PROP_RANGE_TYPE_ENUM) {
    int i;

    for (i = 0; i < info->n_range_values; i++) {
      if (memcmp (info->range_values[i].val.value, val->value, val->size) == 0) {
        if (info->range_values[i].name) {
          enum_value = info->range_values[i].name;
          enum_string = true;
        }
      }
    }
  }

  switch (type) {
    case SPA_PROP_TYPE_INVALID:
      fprintf (stderr, "invalid");
      break;
    case SPA_PROP_TYPE_BOOL:
      fprintf (stderr, "%s", *(bool *)val->value ? "true" : "false");
      break;
    case SPA_PROP_TYPE_INT8:
      fprintf (stderr, "%" PRIi8, *(int8_t *)val->value);
      break;
    case SPA_PROP_TYPE_UINT8:
      fprintf (stderr, "%" PRIu8, *(uint8_t *)val->value);
      break;
    case SPA_PROP_TYPE_INT16:
      fprintf (stderr, "%" PRIi16, *(int16_t *)val->value);
      break;
    case SPA_PROP_TYPE_UINT16:
      fprintf (stderr, "%" PRIu16, *(uint16_t *)val->value);
      break;
    case SPA_PROP_TYPE_INT32:
      fprintf (stderr, "%" PRIi32, *(int32_t *)val->value);
      break;
    case SPA_PROP_TYPE_UINT32:
      fprintf (stderr, "%" PRIu32, *(uint32_t *)val->value);
      break;
    case SPA_PROP_TYPE_INT64:
      fprintf (stderr, "%" PRIi64 "\n", *(int64_t *)val->value);
      break;
    case SPA_PROP_TYPE_UINT64:
      fprintf (stderr, "%" PRIu64 "\n", *(uint64_t *)val->value);
      break;
    case SPA_PROP_TYPE_INT:
      fprintf (stderr, "%d", *(int *)val->value);
      break;
    case SPA_PROP_TYPE_UINT:
      fprintf (stderr, "%u", *(unsigned int *)val->value);
      break;
    case SPA_PROP_TYPE_FLOAT:
      fprintf (stderr, "%f", *(float *)val->value);
      break;
    case SPA_PROP_TYPE_DOUBLE:
      fprintf (stderr, "%g", *(double *)val->value);
      break;
    case SPA_PROP_TYPE_STRING:
      fprintf (stderr, "\"%s\"", (char *)val->value);
      break;
    case SPA_PROP_TYPE_RECTANGLE:
    {
      const SpaRectangle *r = val->value;
      fprintf (stderr, "%"PRIu32"x%"PRIu32, r->width, r->height);
      break;
    }
    case SPA_PROP_TYPE_FRACTION:
    {
      const SpaFraction *f = val->value;
      fprintf (stderr, "%"PRIu32"/%"PRIu32, f->num, f->denom);
      break;
    }
    case SPA_PROP_TYPE_BITMASK:
      break;
    case SPA_PROP_TYPE_POINTER:
      fprintf (stderr, "%p", val->value);
      break;
    default:
      break;
  }
  if (enum_string)
    fprintf (stderr, " (%s)", (char *)enum_value);
}

SpaResult
spa_debug_props (const SpaProps *props, bool print_ranges)
{
  SpaResult res;
  const SpaPropInfo *info;
  int i, j;

  if (props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  fprintf (stderr, "Properties (%d items):\n", props->n_prop_info);
  for (i = 0; i < props->n_prop_info; i++) {
    SpaPropValue value;

    info = &props->prop_info[i];

    fprintf (stderr, "  %-20s: %s\n", info->name, info->description);
    fprintf (stderr, "%-23.23s flags: ", "");
    if (info->flags & SPA_PROP_FLAG_READABLE)
      fprintf (stderr, "readable ");
    if (info->flags & SPA_PROP_FLAG_WRITABLE)
      fprintf (stderr, "writable ");
    if (info->flags & SPA_PROP_FLAG_OPTIONAL)
      fprintf (stderr, "optional ");
    if (info->flags & SPA_PROP_FLAG_DEPRECATED)
      fprintf (stderr, "deprecated ");
    fprintf (stderr, "\n");

    fprintf (stderr, "%-23.23s %s. ", "", prop_type_names[info->type].CCName);

    res = spa_props_get_value (props, i, &value);

    fprintf (stderr, "Current: ");
    if (res == SPA_RESULT_OK)
      print_value (info, &value);
    else if (res == SPA_RESULT_PROPERTY_UNSET)
      fprintf (stderr, "Unset");
    else
      fprintf (stderr, "Error %d", res);
    fprintf (stderr, ".\n");

    if (!print_ranges)
      continue;

    if (info->range_type != SPA_PROP_RANGE_TYPE_NONE) {
      fprintf (stderr, "%-23.23s ", "");
      switch (info->range_type) {
        case SPA_PROP_RANGE_TYPE_MIN_MAX:
          fprintf (stderr, "Range");
          break;
        case SPA_PROP_RANGE_TYPE_STEP:
          fprintf (stderr, "Step");
          break;
        case SPA_PROP_RANGE_TYPE_ENUM:
          fprintf (stderr, "Enum");
          break;
        case SPA_PROP_RANGE_TYPE_FLAGS:
          fprintf (stderr, "Flags");
          break;
        default:
          fprintf (stderr, "Unknown");
          break;
      }
      fprintf (stderr, ".\n");

      for (j = 0; j < info->n_range_values; j++) {
        const SpaPropRangeInfo *rinfo = &info->range_values[j];
        fprintf (stderr, "%-23.23s   ", "");
        print_value (info, &rinfo->val);
        fprintf (stderr, "\t: %-12s - %s \n", rinfo->name, rinfo->description);
      }
    }
    if (info->tags) {
      fprintf (stderr, "Tags: ");
      for (j = 0; info->tags[j]; j++) {
        fprintf (stderr, "\"%s\" ", info->tags[j]);
      }
      fprintf (stderr, "\n");
    }
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_debug_format (const SpaFormat *format)
{
  const SpaProps *props;
  int i, first, last, idx;
  const char *media_type;
  const char *media_subtype;

  if (format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  props = &format->props;

  if (format->media_type > 0 && format->media_type < SPA_N_ELEMENTS (media_type_names)) {
    media_type = media_type_names[format->media_type].name;
    first = media_type_names[format->media_type].first;
    last = media_type_names[format->media_type].last;
    idx = media_type_names[format->media_type].idx;
  }
  else {
    media_type = "unknown";
    idx = first = last = -1;
  }

  if (format->media_subtype >= SPA_MEDIA_SUBTYPE_ANY_FIRST &&
      format->media_subtype <= SPA_MEDIA_SUBTYPE_ANY_LAST) {
    media_subtype = media_subtype_names[format->media_subtype].name;
  } else if (format->media_subtype >= first && format->media_subtype <= last)
    media_subtype = media_subtype_names[format->media_subtype - first + idx].name;
  else
    media_subtype = "unknown";

  fprintf (stderr, "%-6s %s/%s\n", "", media_type, media_subtype);

  for (i = 0; i < props->n_prop_info; i++) {
    const SpaPropInfo *info = &props->prop_info[i];
    SpaPropValue value;
    SpaResult res;

    res = spa_props_get_value (props, i, &value);

    if (info->flags & SPA_PROP_FLAG_INFO)
      continue;
    if (res == SPA_RESULT_PROPERTY_UNSET && info->flags & SPA_PROP_FLAG_OPTIONAL)
      continue;

    fprintf (stderr, "  %20s : (%s) ", info->name, prop_type_names[info->type].name);
    if (res == SPA_RESULT_OK) {
      print_value (info, &value);
    } else if (res == SPA_RESULT_PROPERTY_UNSET) {
      int j;
      const char *ssep, *esep, *sep;

      switch (info->range_type) {
        case SPA_PROP_RANGE_TYPE_MIN_MAX:
        case SPA_PROP_RANGE_TYPE_STEP:
          ssep = "[ ";
          sep = ", ";
          esep = " ]";
          break;
        default:
        case SPA_PROP_RANGE_TYPE_ENUM:
        case SPA_PROP_RANGE_TYPE_FLAGS:
          ssep = "{ ";
          sep = ", ";
          esep = " }";
          break;
      }

      fprintf (stderr, ssep);
      for (j = 0; j < info->n_range_values; j++) {
        const SpaPropRangeInfo *rinfo = &info->range_values[j];
        print_value (info, &rinfo->val);
        fprintf (stderr, "%s", j + 1 < info->n_range_values ? sep : "");
      }
      fprintf (stderr, esep);
    } else {
      fprintf (stderr, "*Error*");
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
