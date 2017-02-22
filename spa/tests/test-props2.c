/* Spa
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>


#include <spa/pod.h>
#include <spa/pod-builder.h>

#include <spa/id-map.h>
#include <spa/log.h>
#include <spa/video/format.h>
#include <lib/debug.h>
#include <lib/mapper.h>

static void
print_value (uint32_t size, uint32_t type, void *body, int prefix, uint32_t flags)
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
        print_value (b->child.size, b->child.type, p, prefix + 2, flags);
      break;
    }
    case SPA_POD_TYPE_STRUCT:
    {
      SpaPOD *b = body, *p;
      printf ("%-*sStruct: size %d\n", prefix, "", size);
      SPA_POD_STRUCT_BODY_FOREACH (b, size, p)
        print_value (p->size, p->type, SPA_POD_BODY (p), prefix + 2, flags);
      break;
    }
    case SPA_POD_TYPE_OBJECT:
    {
      SpaPODObjectBody *b = body;
      SpaPODProp *p;
      void *alt;
      int i;

      printf ("%-*sObject: size %d\n", prefix, "", size);
      SPA_POD_OBJECT_BODY_FOREACH (b, size, p) {
        printf ("%-*sProp: key %d, flags %d\n", prefix + 2, "", p->body.key, p->body.flags);
        if (p->body.flags & SPA_POD_PROP_FLAG_UNSET)
          printf ("%-*sUnset (Default):\n", prefix + 4, "");
        else
          printf ("%-*sValue:\n", prefix + 4, "");
        print_value (p->body.value.size, p->body.value.type, SPA_POD_BODY (&p->body.value), prefix + 6, p->body.flags);

        i = 0;
        SPA_POD_PROP_ALTERNATIVE_FOREACH (&p->body, p->pod.size, alt) {
          if (i == 0)
            printf ("%-*sAlternatives:\n", prefix + 4, "");
          print_value (p->body.value.size, p->body.value.type, alt, prefix + 6, p->body.flags);
          i++;
        }
      }
      break;
    }
  }
}

int
main (int argc, char *argv[])
{
  SpaPODBuilder b = { NULL, };
  SpaPODFrame frame[4];
  uint8_t buffer[1024];
  SpaPOD *obj;

  b.data = buffer;
  b.size = 1024;

  obj = SPA_MEMBER (buffer, spa_pod_builder_push_object (&b, &frame[0], 0, 0), SpaPOD);

  uint32_t formats[] = { 1, 2 };
  spa_pod_builder_push_prop (&b, &frame[1],
                             1, SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_READWRITE);
  spa_pod_builder_int (&b, 1);
  spa_pod_builder_int (&b, formats[0]);
  spa_pod_builder_int (&b, formats[1]);
  spa_pod_builder_pop (&b, &frame[1]);

  spa_pod_builder_push_prop (&b, &frame[1],
                             2, SPA_POD_PROP_RANGE_NONE | SPA_POD_PROP_FLAG_READWRITE);
  spa_pod_builder_int (&b, 42);
  spa_pod_builder_pop (&b, &frame[1]);

  SpaRectangle def = { 320, 240 }, sizes[] = { { 0, 0 }, { 1024, 1024} };
  spa_pod_builder_push_prop (&b, &frame[1],
                             3, SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET | SPA_POD_PROP_FLAG_READWRITE);
  spa_pod_builder_rectangle (&b, &def);
  spa_pod_builder_raw (&b, sizes, sizeof (sizes), false);
  spa_pod_builder_pop (&b, &frame[1]);

  spa_pod_builder_push_prop (&b, &frame[1], 4, SPA_POD_PROP_RANGE_NONE | SPA_POD_PROP_FLAG_READABLE);
  spa_pod_builder_push_struct (&b, &frame[2]);
  spa_pod_builder_int (&b, 4);
  spa_pod_builder_long (&b, 6000);
  spa_pod_builder_float (&b, 4.0);
  spa_pod_builder_double (&b, 3.14);
  spa_pod_builder_string (&b, "test123", strlen ("test123"));
  spa_pod_builder_rectangle (&b, &def);
  SpaFraction f = { 25, 1 };
  spa_pod_builder_fraction (&b, &f);
  spa_pod_builder_push_array (&b, &frame[3]);
  spa_pod_builder_int (&b, 4);
  spa_pod_builder_int (&b, 5);
  spa_pod_builder_int (&b, 6);
  spa_pod_builder_pop (&b, &frame[3]);
  spa_pod_builder_pop (&b, &frame[2]);
  spa_pod_builder_pop (&b, &frame[1]);
  spa_pod_builder_pop (&b, &frame[0]);

  print_value (obj->size, obj->type, SPA_POD_BODY (obj), 0, 0);

  SpaPODProp *p = spa_pod_object_body_find_prop (SPA_POD_BODY (obj), obj->size, 4);
  printf ("%d %d\n", p->body.key, p->body.flags);
  print_value (p->body.value.size, p->body.value.type, SPA_POD_BODY (&p->body.value), 0, 0);

  return 0;
}
