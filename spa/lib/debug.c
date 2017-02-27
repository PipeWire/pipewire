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

#include "debug.h"

static const struct meta_type_name {
  const char *name;
} meta_type_names[] = {
  { "invalid" },
  { "SpaMetaHeader" },
  { "SpaMetaPointer" },
  { "SpaMetaVideoCrop" },
  { "SpaMetaRingbuffer" },
  { "SpaMetaShared" },
  { "invalid" },
};
#define META_TYPE_NAME(t)  meta_type_names[SPA_CLAMP(t,0,SPA_N_ELEMENTS(meta_type_names)-1)].name

static const struct data_type_name {
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
        switch (p->type) {
          case SPA_META_TYPE_RINGBUFFER:
          {
            SpaAllocParamMetaEnableRingbuffer *rb = (SpaAllocParamMetaEnableRingbuffer *)p;
            fprintf (stderr, "    minsize: \t\t%zd\n", rb->minsize);
            fprintf (stderr, "    stride: \t\t%zd\n", rb->stride);
            fprintf (stderr, "    blocks: \t\t%zd\n", rb->blocks);
            fprintf (stderr, "    align: \t\t%d\n", rb->align);
            break;
          }
          default:
            break;
        }
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
        fprintf (stderr, "      readindex:   %zd\n", h->ringbuffer.readindex);
        fprintf (stderr, "      writeindex:  %zd\n", h->ringbuffer.writeindex);
        fprintf (stderr, "      size:        %zd\n", h->ringbuffer.size);
        fprintf (stderr, "      mask:        %zd\n", h->ringbuffer.mask);
        fprintf (stderr, "      mask2:       %zd\n", h->ringbuffer.mask2);
        break;
      }
      case SPA_META_TYPE_SHARED:
      {
        SpaMetaShared *h = m->data;
        fprintf (stderr, "    SpaMetaShared:\n");
        fprintf (stderr, "      type:   %d\n", h->type);
        fprintf (stderr, "      flags:  %d\n", h->flags);
        fprintf (stderr, "      fd:     %d\n", h->fd);
        fprintf (stderr, "      offset: %zd\n", h->offset);
        fprintf (stderr, "      size:   %zd\n", h->size);
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
    fprintf (stderr, "   flags:   %d\n", d->flags);
    fprintf (stderr, "   data:    %p\n", d->data);
    fprintf (stderr, "   fd:      %d\n", d->fd);
    fprintf (stderr, "   offset:  %zd\n", d->mapoffset);
    fprintf (stderr, "   maxsize: %zu\n", d->maxsize);
    fprintf (stderr, "   chunk:   %p\n", d->chunk);
    fprintf (stderr, "    offset: %zd\n", d->chunk->offset);
    fprintf (stderr, "    size:   %zu\n", d->chunk->size);
    fprintf (stderr, "    stride: %zd\n", d->chunk->stride);
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
spa_debug_props (const SpaProps *props, bool print_ranges)
{
  return SPA_RESULT_OK;
}

static const char* media_audio_prop_names[] = {
  "info",
  "format",
  "flags",
  "layout",
  "rate",
  "channels",
  "channel-mask",
};

static const char* media_video_prop_names[] = {
  "info",
  "format",
  "size",
  "framerate",
  "max-framerate",
  "views",
  "interlace-mode",
  "pixel-aspect-ratio",
  "multiview-mode",
  "multiview-flags",
  "chroma-site",
  "color-range",
  "color-matrix",
  "transfer-function",
  "color-primaries",
  "profile",
  "stream-format",
  "alignment",
};

static const struct media_type_name {
  const char *name;
  unsigned int first;
  unsigned int last;
  unsigned int idx;
  const char **prop_names;
} media_type_names[] = {
  { "invalid", 0, 0, 0 },
  { "audio", SPA_MEDIA_SUBTYPE_AUDIO_FIRST, SPA_MEDIA_SUBTYPE_AUDIO_LAST, 16, media_audio_prop_names },
  { "video", SPA_MEDIA_SUBTYPE_VIDEO_FIRST, SPA_MEDIA_SUBTYPE_VIDEO_LAST, 2, media_video_prop_names },
  { "image", 0, 0 },
};


static const struct media_subtype_name {
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

struct pod_type_name {
  const char *name;
  const char *CCName;
} pod_type_names[] = {
  { "invalid", "*Invalid*" },
  { "bool", "Bool" },
  { "int", "Int" },
  { "long", "Long" },
  { "float", "Float" },
  { "double", "Double" },
  { "string", "String" },
  { "rectangle", "Rectangle" },
  { "fraction", "Fraction" },
  { "bitmask", "Bitmask" },
  { "array", "Array" },
  { "struct", "Struct" },
  { "object", "Object" },
  { "prop", "Prop" },
  { "bytes", "Bytes" },
};

static void
print_pod_value (uint32_t size, uint32_t type, void *body, int prefix)
{
  switch (type) {
    case SPA_POD_TYPE_BOOL:
      printf ("%-*sBool %d\n", prefix, "", *(int32_t *) body);
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
      printf ("%-*sDouble %g\n", prefix, "", *(double *) body);
      break;
    case SPA_POD_TYPE_STRING:
      printf ("%-*sString %s\n", prefix, "", (char *) body);
      break;
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
        print_pod_value (b->child.size, b->child.type, p, prefix + 2);
      break;
    }
    case SPA_POD_TYPE_STRUCT:
    {
      SpaPOD *b = body, *p;
      printf ("%-*sStruct: size %d\n", prefix, "", size);
      SPA_POD_STRUCT_BODY_FOREACH (b, size, p)
        print_pod_value (p->size, p->type, SPA_POD_BODY (p), prefix + 2);
      break;
    }
    case SPA_POD_TYPE_OBJECT:
    {
      SpaPODObjectBody *b = body;
      SpaPODProp *p;

      printf ("%-*sObject: size %d\n", prefix, "", size);
      SPA_POD_OBJECT_BODY_FOREACH (b, size, p)
        print_pod_value (p->pod.size, p->pod.type, SPA_POD_BODY (p), prefix + 6);
      break;
    }
    case SPA_POD_TYPE_PROP:
    {
      SpaPODPropBody *b = body;
      void *alt;
      int i;

      printf ("%-*sProp: key %d, flags %d\n", prefix + 2, "", b->key, b->flags);
      if (b->flags & SPA_POD_PROP_FLAG_UNSET)
        printf ("%-*sUnset (Default):\n", prefix + 4, "");
      else
        printf ("%-*sValue:\n", prefix + 4, "");
      print_pod_value (b->value.size, b->value.type, SPA_POD_BODY (&b->value), prefix + 6);

      i = 0;
      SPA_POD_PROP_ALTERNATIVE_FOREACH (b, size, alt) {
        if (i == 0)
          printf ("%-*sAlternatives:\n", prefix + 4, "");
        print_pod_value (b->value.size, b->value.type, alt, prefix + 6);
        i++;
      }
      break;
    }
    case SPA_POD_TYPE_FORMAT:
    {
      SpaFormatBody *b = body;
      SpaPODProp *p;

      printf ("%-*sFormat: size %d\n", prefix, "", size);
      printf ("%-*s  Media Type: %d / %d\n", prefix, "", b->media_type, b->media_subtype);
      SPA_FORMAT_BODY_FOREACH (b, size, p)
        print_pod_value (p->pod.size, p->pod.type, SPA_POD_BODY (p), prefix + 6);
      break;
    }
  }
}


SpaResult
spa_debug_pod (const SpaPOD *pod)
{
  print_pod_value (pod->size, pod->type, SPA_POD_BODY (pod), 0);
  return SPA_RESULT_OK;
}

static void
print_format_value (uint32_t size, uint32_t type, void *body)
{
  switch (type) {
    case SPA_POD_TYPE_BOOL:
      fprintf (stderr, "%s", *(int32_t *) body ? "true" : "false");
      break;
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
    default:
      break;
  }
}

SpaResult
spa_debug_format (const SpaFormat *format)
{
  int i, first, last, idx;
  const char *media_type;
  const char *media_subtype;
  const char **prop_names;
  SpaPODProp *prop;

  if (format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (format->body.media_type > 0 && format->body.media_type < SPA_N_ELEMENTS (media_type_names)) {
    media_type = media_type_names[format->body.media_type].name;
    first = media_type_names[format->body.media_type].first;
    last = media_type_names[format->body.media_type].last;
    idx = media_type_names[format->body.media_type].idx;
    prop_names = media_type_names[format->body.media_type].prop_names;
  }
  else {
    media_type = "unknown";
    idx = first = last = -1;
    prop_names = NULL;
  }

  if (format->body.media_subtype >= SPA_MEDIA_SUBTYPE_ANY_FIRST &&
      format->body.media_subtype <= SPA_MEDIA_SUBTYPE_ANY_LAST) {
    media_subtype = media_subtype_names[format->body.media_subtype].name;
  } else if (format->body.media_subtype >= first && format->body.media_subtype <= last)
    media_subtype = media_subtype_names[format->body.media_subtype - first + idx].name;
  else
    media_subtype = "unknown";

  fprintf (stderr, "%-6s %s/%s\n", "", media_type, media_subtype);

  SPA_POD_FOREACH (format, prop) {
    if ((prop->body.flags & SPA_POD_PROP_FLAG_UNSET) &&
        (prop->body.flags & SPA_POD_PROP_FLAG_OPTIONAL))
      continue;

    fprintf (stderr, "  %20s : (%s) ", prop_names[prop->body.key - SPA_PROP_ID_MEDIA_CUSTOM_START], pod_type_names[prop->body.value.type].name);
    if (!(prop->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
      print_format_value (prop->body.value.size, prop->body.value.type, SPA_POD_BODY (&prop->body.value));
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

      fprintf (stderr, ssep);

      i = 0;
      SPA_POD_PROP_ALTERNATIVE_FOREACH (&prop->body, prop->pod.size, alt) {
        if (i > 0)
          fprintf (stderr, "%s", sep);
        print_format_value (prop->body.value.size, prop->body.value.type, alt);
        i++;
      }
      fprintf (stderr, esep);
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
