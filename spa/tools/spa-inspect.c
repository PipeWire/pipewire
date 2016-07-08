/* Pinos
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#include <spa/node.h>

static void
print_value (const char *prefix, SpaPropType type, int size, const void *value)
{
  printf ("%s", prefix);
  switch (type) {
    case SPA_PROP_TYPE_INVALID:
      printf ("invalid");
      break;
    case SPA_PROP_TYPE_BOOL:
      printf ("%s", *(bool *)value ? "true" : "false");
      break;
    case SPA_PROP_TYPE_INT8:
      printf ("%" PRIi8, *(int8_t *)value);
      break;
    case SPA_PROP_TYPE_UINT8:
      printf ("%" PRIu8, *(uint8_t *)value);
      break;
    case SPA_PROP_TYPE_INT16:
      printf ("%" PRIi16, *(int16_t *)value);
      break;
    case SPA_PROP_TYPE_UINT16:
      printf ("%" PRIu16, *(uint16_t *)value);
      break;
    case SPA_PROP_TYPE_INT32:
      printf ("%" PRIi32, *(int32_t *)value);
      break;
    case SPA_PROP_TYPE_UINT32:
      printf ("%" PRIu32, *(uint32_t *)value);
      break;
    case SPA_PROP_TYPE_INT64:
      printf ("%" PRIi64 "\n", *(int64_t *)value);
      break;
    case SPA_PROP_TYPE_UINT64:
      printf ("%" PRIu64 "\n", *(uint64_t *)value);
      break;
    case SPA_PROP_TYPE_FLOAT:
      printf ("%f", *(float *)value);
      break;
    case SPA_PROP_TYPE_DOUBLE:
      printf ("%g", *(double *)value);
      break;
    case SPA_PROP_TYPE_STRING:
      printf ("%s", (char *)value);
      break;
    case SPA_PROP_TYPE_POINTER:
      printf ("%p", value);
      break;
    case SPA_PROP_TYPE_FRACTION:
      break;
    case SPA_PROP_TYPE_BITMASK:
      break;
    case SPA_PROP_TYPE_BYTES:
      break;
    default:
      break;
  }
  printf ("\n");
}

static void
print_props (const SpaProps *props, int print_ranges)
{
  SpaResult res;
  const SpaPropInfo *info;
  int i, j;

  for (i = 0; i < props->n_prop_info; i++) {
    SpaPropValue value;

    info = &props->prop_info[i];

    printf ("id:\t\t%d\n", info->id);
    printf ("name:\t\t%s\n", info->name);
    printf ("description:\t%s\n", info->description);
    printf ("flags:\t\t%d\n", info->flags);
    printf ("type:\t\t%d\n", info->type);
    printf ("maxsize:\t%zu\n", info->maxsize);

    res = props->get_prop (props, info->id, &value);
    if (res == SPA_RESULT_PROPERTY_UNSET)
      printf ("value:\t\tunset\n");
    else
      print_value ("value:\t\t", value.type, value.size, value.value);

    if (print_ranges) {
      if (info->default_value)
        print_value ("default:\t", info->type, info->default_size, info->default_value);
      else
        printf ("default:\tunset\n");

      printf ("range_type:\t%d\n", info->range_type);
      if (info->range_values) {
        for (j = 0; j < info->n_range_values; j++) {
          const SpaPropRangeInfo *rinfo = &info->range_values[j];
          printf ("  name:\t%s\n", rinfo->name);
          printf ("  description:\t%s\n", rinfo->description);
          print_value ("  value:\t", info->type, rinfo->size, rinfo->value);
        }
      }
    }
    if (info->tags) {
      for (j = 0; info->tags[j]; j++) {
        printf ("tag:\t%s\n", info->tags[j]);
      }
    }
  }
}

static void
print_format (const SpaFormat *format, int print_ranges)
{
  printf ("media-type:\t\t%d\n", format->media_type);
  printf ("media-subtype:\t\t%d\n", format->media_subtype);
  print_props (&format->props, print_ranges);
}

static void
inspect_node (const SpaNode *node, SpaHandle *handle)
{
  SpaResult res;
  SpaProps *props;
  unsigned int n_input, max_input, n_output, max_output, i;
  SpaFormat *format;

  if ((res = node->get_props (handle, &props)) < 0)
    printf ("can't get properties: %d\n", res);
  else
    print_props (props, 1);

  if ((res = node->get_n_ports (handle, &n_input, &max_input, &n_output, &max_output)) < 0)
    printf ("can't get n_ports: %d\n", res);
  else
    printf ("supported ports %d %d %d %d\n", n_input, max_input, n_output, max_output);

  for (i = 0; ; i++) {
    if ((res = node->port_enum_formats (handle, 0, i, &format)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("got error %d\n", res);
      break;
    }
    print_format (format, 1);
  }
  if ((res = node->port_get_props (handle, 0, &props)) < 0)
    printf ("port_get_props error: %d\n", res);
  else
    print_props (props, 1);
}

static void
inspect_factory (const SpaHandleFactory *factory)
{
  SpaResult res;
  unsigned int i;
  SpaHandle *handle;
  const void *interface;

  printf ("factory name:\t\t'%s'\n", factory->name);
  printf ("factory info:\n");
  if (factory->info)
    print_props (factory->info, 1);
  else
    printf ("  none\n");

  if ((res = factory->instantiate (factory, &handle)) < 0) {
    printf ("can't make factory instance: %d\n", res);
    return;
  }

  printf ("factory interfaces:\n");

  for (i = 0; ; i++) {
    const SpaInterfaceInfo *info;

    if ((res = factory->enum_interface_info (factory, i, &info)) < 0) {
      if (res == SPA_RESULT_ENUM_END)
        break;
      else
        printf ("can't enumerate interfaces: %d\n", res);
    }
    printf (" interface: %d, (%d) '%s' : '%s'\n", i, info->interface_id, info->name, info->description);

    if ((res = handle->get_interface (handle, info->interface_id, &interface)) < 0) {
      printf ("can't get interface: %d\n", res);
      continue;
    }

    switch (info->interface_id) {
      case SPA_INTERFACE_ID_NODE:
        inspect_node (interface, handle);
        break;
      default:
        printf ("skipping unknown interface\n");
        break;
    }
  }
}

int
main (int argc, char *argv[])
{
  SpaResult res;
  void *handle;
  SpaEnumHandleFactoryFunc enum_func;
  unsigned int i;

  if ((handle = dlopen (argv[1], RTLD_NOW)) == NULL) {
    printf ("can't load %s\n", argv[1]);
    return -1;
  }
  if ((enum_func = dlsym (handle, "spa_enum_handle_factory")) == NULL) {
    printf ("can't find function\n");
    return -1;
  }

  for (i = 0; ;i++) {
    const SpaHandleFactory *factory;

    if ((res = enum_func (i, &factory)) < 0) {
      if (res == SPA_RESULT_ENUM_END)
        break;
      else
        printf ("can't enumerate factories\n");
    }
    inspect_factory (factory);
  }

  return 0;
}
