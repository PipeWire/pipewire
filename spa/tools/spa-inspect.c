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

struct media_type_name {
  const char *name;
} media_type_names[] = {
  { "unknown" },
  { "audio" },
  { "video" },
};

struct media_subtype_name {
  const char *name;
} media_subtype_names[] = {
  { "unknown" },
  { "raw" },
  { "h264" },
  { "mjpg" },
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
  { "double", "Double" },
  { "string", "String" },
  { "rectangle", "Rectangle" },
  { "fraction", "Fraction" },
  { "bitmask", "Bitmask" },
  { "pointer", "Pointer" },
};

static void
print_value (const SpaPropInfo *info, int size, const void *value)
{
  SpaPropType type = info->type;
  bool enum_string = false;

  if (info->range_type == SPA_PROP_RANGE_TYPE_ENUM) {
    int i;

    for (i = 0; i < info->n_range_values; i++) {
      if (memcmp (info->range_values[i].value, value, size) == 0) {
        if (info->range_values[i].name) {
          type = SPA_PROP_TYPE_STRING;
          value = info->range_values[i].name;
          enum_string = true;
        }
      }
    }
  }

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
    case SPA_PROP_TYPE_INT:
      printf ("%d", *(int *)value);
      break;
    case SPA_PROP_TYPE_UINT:
      printf ("%u", *(unsigned int *)value);
      break;
    case SPA_PROP_TYPE_FLOAT:
      printf ("%f", *(float *)value);
      break;
    case SPA_PROP_TYPE_DOUBLE:
      printf ("%g", *(double *)value);
      break;
    case SPA_PROP_TYPE_STRING:
      if (enum_string)
        printf ("%s", (char *)value);
      else
        printf ("\"%s\"", (char *)value);
      break;
    case SPA_PROP_TYPE_RECTANGLE:
    {
      const SpaRectangle *r = value;
      printf ("%"PRIu32"x%"PRIu32, r->width, r->height);
      break;
    }
    case SPA_PROP_TYPE_FRACTION:
    {
      const SpaFraction *f = value;
      printf ("%"PRIu32"/%"PRIu32, f->num, f->denom);
      break;
    }
    case SPA_PROP_TYPE_BITMASK:
      break;
    case SPA_PROP_TYPE_POINTER:
      printf ("%p", value);
      break;
    default:
      break;
  }
}

static void
print_props (const SpaProps *props, int print_ranges)
{
  SpaResult res;
  const SpaPropInfo *info;
  int i, j;

  printf ("Properties (%d items):\n", props->n_prop_info);
  for (i = 0; i < props->n_prop_info; i++) {
    SpaPropValue value;

    info = &props->prop_info[i];

    printf ("  %-20s: %s\n", info->name, info->description);
    printf ("%-23.23s flags: ", "");
    if (info->flags & SPA_PROP_FLAG_READABLE)
      printf ("readable ");
    if (info->flags & SPA_PROP_FLAG_WRITABLE)
      printf ("writable ");
    if (info->flags & SPA_PROP_FLAG_OPTIONAL)
      printf ("optional ");
    if (info->flags & SPA_PROP_FLAG_DEPRECATED)
      printf ("deprecated ");
    printf ("\n");

    printf ("%-23.23s %s. ", "", prop_type_names[info->type].CCName);

    printf ("Default: ");
    if (info->default_value)
      print_value (info, info->default_size, info->default_value);
    else
      printf ("None");

    res = props->get_prop (props, i, &value);

    printf (". Current: ");
    if (res == SPA_RESULT_OK)
      print_value (info, value.size, value.value);
    else if (res == SPA_RESULT_PROPERTY_UNSET)
      printf ("Unset");
    else
      printf ("Error %d", res);
    printf (".\n");

    if (!print_ranges)
      continue;

    if (info->range_type != SPA_PROP_RANGE_TYPE_NONE) {
      printf ("%-23.23s ", "");
      switch (info->range_type) {
        case SPA_PROP_RANGE_TYPE_MIN_MAX:
          printf ("Range");
          break;
        case SPA_PROP_RANGE_TYPE_STEP:
          printf ("Step");
          break;
        case SPA_PROP_RANGE_TYPE_ENUM:
          printf ("Enum");
          break;
        case SPA_PROP_RANGE_TYPE_FLAGS:
          printf ("Flags");
          break;
        default:
          printf ("Unknown");
          break;
      }
      printf (".\n");

      for (j = 0; j < info->n_range_values; j++) {
        const SpaPropRangeInfo *rinfo = &info->range_values[j];
        printf ("%-23.23s   ", "");
        print_value (info, rinfo->size, rinfo->value);
        printf ("\t: %-12s - %s \n", rinfo->name, rinfo->description);
      }
    }
    if (info->tags) {
      printf ("Tags: ");
      for (j = 0; info->tags[j]; j++) {
        printf ("\"%s\" ", info->tags[j]);
      }
      printf ("\n");
    }
  }
}

static void
print_format (const SpaFormat *format)
{
  const SpaProps *props = &format->props;
  int i;

  printf ("%-6s %s/%s\n", "", media_type_names[format->media_type].name,
                        media_subtype_names[format->media_subtype].name);

  for (i = 0; i < props->n_prop_info; i++) {
    const SpaPropInfo *info = &props->prop_info[i];
    SpaPropValue value;
    SpaResult res;

    res = props->get_prop (props, i, &value);

    if (res == SPA_RESULT_PROPERTY_UNSET && info->flags & SPA_PROP_FLAG_OPTIONAL)
      continue;

    printf ("  %20s : (%s) ", info->name, prop_type_names[info->type].name);
    if (res == SPA_RESULT_OK) {
      print_value (info, value.size, value.value);
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

      printf (ssep);
      for (j = 0; j < info->n_range_values; j++) {
        const SpaPropRangeInfo *rinfo = &info->range_values[j];
        print_value (info, rinfo->size, rinfo->value);
        printf ("%s", j + 1 < info->n_range_values ? sep : "");
      }
      printf (esep);
    } else {
      printf ("*Error*");
    }
    printf ("\n");
  }
}

static void
inspect_node (SpaNode *node)
{
  SpaResult res;
  SpaProps *props;
  unsigned int n_input, max_input, n_output, max_output;
  SpaFormat *format;
  void *state = NULL;

  if ((res = node->get_props (node, &props)) < 0)
    printf ("can't get properties: %d\n", res);
  else
    print_props (props, 1);

  if ((res = node->get_n_ports (node, &n_input, &max_input, &n_output, &max_output)) < 0)
    printf ("can't get n_ports: %d\n", res);
  else
    printf ("supported ports %d %d %d %d\n", n_input, max_input, n_output, max_output);

  while (true) {
    if ((res = node->port_enum_formats (node, 0, &format, NULL, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("got error %d\n", res);
      break;
    }
    if (format)
      print_format (format);
  }
  if ((res = node->port_get_props (node, 0, &props)) < 0)
    printf ("port_get_props error: %d\n", res);
  else
    print_props (props, 1);
}

static void
inspect_factory (const SpaHandleFactory *factory)
{
  SpaResult res;
  SpaHandle *handle;
  void *interface;
  void *state = NULL;

  printf ("factory name:\t\t'%s'\n", factory->name);
  printf ("factory info:\n");
  if (factory->info)
    print_props (factory->info, 1);
  else
    printf ("  none\n");

  handle = calloc (1, factory->size);
  if ((res = factory->init (factory, handle)) < 0) {
    printf ("can't make factory instance: %d\n", res);
    return;
  }

  printf ("factory interfaces:\n");

  while (true) {
    const SpaInterfaceInfo *info;

    if ((res = factory->enum_interface_info (factory, &info, &state)) < 0) {
      if (res == SPA_RESULT_ENUM_END)
        break;
      else
        printf ("can't enumerate interfaces: %d\n", res);
    }
    printf (" interface: (%d) '%s' : '%s'\n", info->interface_id, info->name, info->description);

    if ((res = handle->get_interface (handle, info->interface_id, &interface)) < 0) {
      printf ("can't get interface: %d\n", res);
      continue;
    }

    switch (info->interface_id) {
      case SPA_INTERFACE_ID_NODE:
        inspect_node (interface);
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
  void *state = NULL;

  if ((handle = dlopen (argv[1], RTLD_NOW)) == NULL) {
    printf ("can't load %s\n", argv[1]);
    return -1;
  }
  if ((enum_func = dlsym (handle, "spa_enum_handle_factory")) == NULL) {
    printf ("can't find function\n");
    return -1;
  }

  while (true) {
    const SpaHandleFactory *factory;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        printf ("can't enumerate factories: %d\n", res);
      break;
    }
    inspect_factory (factory);
  }

  return 0;
}
