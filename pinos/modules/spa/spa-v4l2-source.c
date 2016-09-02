/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <gio/gio.h>

#include <spa/include/spa/node.h>
#include <spa/include/spa/video/format.h>

#include "spa-v4l2-source.h"

#define PINOS_SPA_V4L2_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_V4L2_SOURCE, PinosSpaV4l2SourcePrivate))

struct _PinosSpaV4l2SourcePrivate
{
  gint dummy;
};

enum {
  PROP_0,
};

G_DEFINE_TYPE (PinosSpaV4l2Source, pinos_spa_v4l2_source, PINOS_TYPE_NODE);

static SpaResult
make_node (SpaNode **node, const char *lib, const char *name)
{
  SpaHandle *handle;
  SpaResult res;
  void *hnd, *state = NULL;
  SpaEnumHandleFactoryFunc enum_func;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    g_error ("can't load %s: %s", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    g_error ("can't find enum function");
    return SPA_RESULT_ERROR;
  }

  while (true) {
    const SpaHandleFactory *factory;
    void *iface;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        g_error ("can't enumerate factories: %d", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    handle = calloc (1, factory->size);
    if ((res = factory->init (factory, handle)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    if ((res = handle->get_interface (handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      g_error ("can't get interface %d", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void
setup_node (PinosSpaV4l2Source *this)
{
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = spa_node_get_props (node->node, &props)) < 0)
    g_debug ("got get_props error %d", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.value = "/dev/video1";
  value.size = strlen (value.value)+1;
  spa_props_set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = spa_node_set_props (node->node, props)) < 0)
    g_debug ("got set_props error %d", res);
}

static void
destroy_pipeline (PinosSpaV4l2Source *this)
{
  g_debug ("spa-v4l2-source %p: destroy pipeline", this);
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
source_constructed (GObject * object)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (object);

  G_OBJECT_CLASS (pinos_spa_v4l2_source_parent_class)->constructed (object);

  setup_node (source);
}

static void
source_finalize (GObject * object)
{
  PinosNode *node = PINOS_NODE (object);
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (object);

  g_debug ("spa-source %p: dispose", source);
  destroy_pipeline (source);

  spa_handle_clear (node->node->handle);
  g_free (node->node->handle);

  G_OBJECT_CLASS (pinos_spa_v4l2_source_parent_class)->finalize (object);
}

static void
pinos_spa_v4l2_source_class_init (PinosSpaV4l2SourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaV4l2SourcePrivate));

  gobject_class->constructed = source_constructed;
  gobject_class->finalize = source_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;
}

static void
pinos_spa_v4l2_source_init (PinosSpaV4l2Source * source)
{
  source->priv = PINOS_SPA_V4L2_SOURCE_GET_PRIVATE (source);
}

PinosNode *
pinos_spa_v4l2_source_new (PinosDaemon *daemon,
                           const gchar *name,
                           PinosProperties *properties)
{
  PinosNode *node;
  SpaNode *n;
  SpaResult res;

  if ((res = make_node (&n,
                        "spa/build/plugins/v4l2/libspa-v4l2.so",
                        "v4l2-source")) < 0) {
    g_error ("can't create v4l2-source: %d", res);
    return NULL;
  }

  node = g_object_new (PINOS_TYPE_SPA_V4L2_SOURCE,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "node", n,
                       NULL);

  return node;
}
