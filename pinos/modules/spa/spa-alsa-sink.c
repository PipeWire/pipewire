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
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>

#include <gio/gio.h>

#include <spa/include/spa/node.h>
#include <spa/include/spa/memory.h>
#include <spa/include/spa/audio/format.h>

#include "spa-alsa-sink.h"

#define PINOS_SPA_ALSA_SINK_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_ALSA_SINK, PinosSpaAlsaSinkPrivate))

struct _PinosSpaAlsaSinkPrivate
{
  PinosRingbuffer *ringbuffer;
};

enum {
  PROP_0,
};

G_DEFINE_TYPE (PinosSpaAlsaSink, pinos_spa_alsa_sink, PINOS_TYPE_NODE);

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

#if 0
static void
on_sink_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  PinosSpaAlsaSink *this = user_data;
  PinosSpaAlsaSinkPrivate *priv = this->priv;

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
      SpaPortInputInfo iinfo;
      SpaResult res;
      PinosRingbufferArea areas[2];
      uint8_t *data;
      size_t size, towrite, total;
      SpaNodeEventNeedInput *ni = event->data;

      size = 0;
      data = NULL;

      pinos_ringbuffer_get_read_areas (priv->ringbuffer, areas);

      total = MIN (size, areas[0].len + areas[1].len);
      g_debug ("total read %zd %zd %zd", total, size, areas[0].len + areas[1].len);
      if (total < size) {
        g_warning ("underrun");
      }
      towrite = MIN (size, areas[0].len);
      memcpy (data, areas[0].data, towrite);
      size -= towrite;
      data += towrite;
      towrite = MIN (size, areas[1].len);
      memcpy (data, areas[1].data, towrite);

      pinos_ringbuffer_read_advance (priv->ringbuffer, total);

      iinfo.port_id = ni->port_id;
      iinfo.flags = SPA_PORT_INPUT_FLAG_NONE;
      iinfo.buffer_id = 0;

      g_debug ("push sink %d", iinfo.buffer_id);
      if ((res = spa_node_port_push_input (node, 1, &iinfo)) < 0)
        g_debug ("got error %d", res);
      break;
    }
    default:
      g_debug ("got event %d", event->type);
      break;
  }
}
#endif

static void
setup_node (PinosSpaAlsaSink *this)
{
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = spa_node_get_props (node->node, &props)) < 0)
    g_debug ("got get_props error %d", res);

  value.value = "hw:1";
  value.size = strlen (value.value)+1;
  spa_props_set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = spa_node_set_props (node->node, props)) < 0)
    g_debug ("got set_props error %d", res);
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

#if 0
static gboolean
on_received_buffer (PinosPort   *port,
                    uint32_t     buffer_id,
                    GError     **error,
                    gpointer     user_data)
{
  PinosSpaAlsaSink *this = user_data;
  PinosSpaAlsaSinkPrivate *priv = this->priv;
  unsigned int i;
  SpaBuffer *buffer = NULL; //port->buffers[buffer_id];

  for (i = 0; i < buffer->n_datas; i++) {
    SpaData *d = SPA_BUFFER_DATAS (buffer);
    SpaMemory *mem;
    PinosRingbufferArea areas[2];
    uint8_t *data;
    size_t size, towrite, total;

    mem = spa_memory_find (&d[i].mem.mem);

    size = d[i].mem.size;
    data = SPA_MEMBER (mem->ptr, d[i].mem.offset, uint8_t);

    pinos_ringbuffer_get_write_areas (priv->ringbuffer, areas);

    total = MIN (size, areas[0].len + areas[1].len);
    g_debug ("total write %zd %zd", total, areas[0].len + areas[1].len);
    towrite = MIN (size, areas[0].len);
    memcpy (areas[0].data, data, towrite);
    size -= towrite;
    data += towrite;
    towrite = MIN (size, areas[1].len);
    memcpy (areas[1].data, data, towrite);

    pinos_ringbuffer_write_advance (priv->ringbuffer, total);
  }

  return TRUE;
}
#endif

static void
sink_constructed (GObject * object)
{
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);

  setup_node (sink);

  G_OBJECT_CLASS (pinos_spa_alsa_sink_parent_class)->constructed (object);
}

static void
sink_finalize (GObject * object)
{
  PinosNode *node = PINOS_NODE (object);
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);

  g_debug ("alsa-sink %p: dispose", sink);
  spa_handle_clear (node->node->handle);
  g_free (node->node->handle);

  G_OBJECT_CLASS (pinos_spa_alsa_sink_parent_class)->finalize (object);
}

static void
pinos_spa_alsa_sink_class_init (PinosSpaAlsaSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaAlsaSinkPrivate));

  gobject_class->constructed = sink_constructed;
  gobject_class->finalize = sink_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;
}

static void
pinos_spa_alsa_sink_init (PinosSpaAlsaSink * sink)
{
  sink->priv = PINOS_SPA_ALSA_SINK_GET_PRIVATE (sink);
}

PinosNode *
pinos_spa_alsa_sink_new (PinosDaemon *daemon,
                         const gchar *name,
                         PinosProperties *properties)
{
  PinosNode *node;
  SpaNode *n;
  SpaResult res;

  if ((res = make_node (&n,
                        "spa/build/plugins/alsa/libspa-alsa.so",
                        "alsa-sink")) < 0) {
    g_error ("can't create v4l2-source: %d", res);
    return NULL;
  }

  node = g_object_new (PINOS_TYPE_SPA_ALSA_SINK,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "node", n,
                       NULL);

  return node;
}
