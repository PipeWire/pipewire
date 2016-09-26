/* Pinos
 * Copyright (C) 2016 Axis Communications AB
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

#include <dlfcn.h>

#include <spa/include/spa/node.h>
#include <spa/include/spa/video/format.h>

#include "spa-videotestsrc.h"

#define PINOS_SPA_VIDEOTESTSRC_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_VIDEOTESTSRC, PinosSpaVideoTestSrcPrivate))

struct _PinosSpaVideoTestSrcPrivate
{
  gint dummy;
};

enum {
  PROP_0,
};

G_DEFINE_TYPE (PinosSpaVideoTestSrc, pinos_spa_videotestsrc, PINOS_TYPE_NODE);

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
    if ((res = factory->init (factory, handle, NULL)) < 0) {
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
source_finalize (GObject * object)
{
  PinosNode *node = PINOS_NODE (object);
  PinosSpaVideoTestSrc *source = PINOS_SPA_VIDEOTESTSRC (object);

  g_debug ("spa-source %p: dispose", source);
  spa_handle_clear (node->node->handle);
  g_free (node->node->handle);

  G_OBJECT_CLASS (pinos_spa_videotestsrc_parent_class)->finalize (object);
}

static void
pinos_spa_videotestsrc_class_init (PinosSpaVideoTestSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaVideoTestSrcPrivate));

  gobject_class->finalize = source_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;
}

static void
pinos_spa_videotestsrc_init (PinosSpaVideoTestSrc * source)
{
  source->priv = PINOS_SPA_VIDEOTESTSRC_GET_PRIVATE (source);
}

PinosNode *
pinos_spa_videotestsrc_new (PinosDaemon *daemon,
                            const gchar *name,
                            PinosProperties *properties)
{
  PinosNode *node;
  SpaNode *n;
  SpaResult res;

  if ((res = make_node (&n,
                        "build/spa/plugins/videotestsrc/libspa-videotestsrc.so",
                        "videotestsrc")) < 0) {
    g_error ("can't create videotestsrc: %d", res);
    return NULL;
  }

  node = g_object_new (PINOS_TYPE_SPA_VIDEOTESTSRC,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "node", n,
                       NULL);

  return node;
}
