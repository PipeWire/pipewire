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

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/include/spa/node.h>

#include "spa-audiotestsrc.h"

#define PINOS_SPA_AUDIOTESTSRC_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_AUDIOTESTSRC, PinosSpaAudioTestSrcPrivate))

struct _PinosSpaAudioTestSrcPrivate
{
  PinosRingbuffer *ringbuffer;
};

enum {
  PROP_0,
};

G_DEFINE_TYPE (PinosSpaAudioTestSrc, pinos_spa_audiotestsrc, PINOS_TYPE_NODE);

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
    if ((res = spa_handle_factory_init (factory, handle, NULL)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    if ((res = spa_handle_get_interface (handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      g_error ("can't get interface %d", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void
setup_node (PinosSpaAudioTestSrc *this)
{
#if 0
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = spa_node_get_props (node->node, &props)) < 0)
    g_debug ("got get_props error %d", res);

  if ((res = spa_node_set_props (node->node, props)) < 0)
    g_debug ("got set_props error %d", res);
#endif
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
src_constructed (GObject * object)
{
  PinosSpaAudioTestSrc *src = PINOS_SPA_AUDIOTESTSRC (object);

  setup_node (src);

  G_OBJECT_CLASS (pinos_spa_audiotestsrc_parent_class)->constructed (object);
}

static void
src_finalize (GObject * object)
{
  PinosNode *node = PINOS_NODE (object);
  PinosSpaAudioTestSrc *src = PINOS_SPA_AUDIOTESTSRC (object);

  g_debug ("audiotestsrc %p: dispose", src);
  spa_handle_clear (node->node->handle);
  g_free (node->node->handle);

  G_OBJECT_CLASS (pinos_spa_audiotestsrc_parent_class)->finalize (object);
}

static void
pinos_spa_audiotestsrc_class_init (PinosSpaAudioTestSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaAudioTestSrcPrivate));

  gobject_class->constructed = src_constructed;
  gobject_class->finalize = src_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;
}

static void
pinos_spa_audiotestsrc_init (PinosSpaAudioTestSrc * this)
{
  this->priv = PINOS_SPA_AUDIOTESTSRC_GET_PRIVATE (this);
}

PinosNode *
pinos_spa_audiotestsrc_new (PinosDaemon *daemon,
                            const gchar *name,
                            PinosProperties *properties)
{
  PinosNode *node;
  SpaNode *n;
  SpaResult res;

  if ((res = make_node (&n,
                        "spa/build/plugins/audiotestsrc/libspa-audiotestsrc.so",
                        "audiotestsrc")) < 0) {
    g_error ("can't create audiotestsrc: %d", res);
    return NULL;
  }

  node = g_object_new (PINOS_TYPE_SPA_AUDIOTESTSRC,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "node", n,
                       NULL);

  return node;
}
