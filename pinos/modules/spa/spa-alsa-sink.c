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
#include <spa/include/spa/audio/format.h>

#include "spa-alsa-sink.h"

#define PINOS_SPA_ALSA_SINK_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_ALSA_SINK, PinosSpaAlsaSinkPrivate))

typedef struct {
  PinosSpaAlsaSink *sink;

  guint id;
  PinosPort *port;
} SinkPortData;

typedef struct {
  guint32 id;
  guint32 type;
  int fd;
  guint64 offset;
  guint64 size;
  void *data;
} MemBlock;

struct _PinosSpaAlsaSinkPrivate
{
  SpaNode *sink;

  PinosProperties *props;
  PinosRingbuffer *ringbuffer;

  SpaPollFd fds[16];
  unsigned int n_fds;
  SpaPollItem poll;

  gboolean running;
  pthread_t thread;

  GHashTable *mem_ids;

  GList *ports;
};

enum {
  PROP_0,
  PROP_POSSIBLE_FORMATS
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

static void
on_sink_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  PinosSpaAlsaSink *this = user_data;
  PinosSpaAlsaSinkPrivate *priv = this->priv;

  switch (event->type) {
    case SPA_EVENT_TYPE_PULL_INPUT:
    {
      SpaInputInfo iinfo;
      SpaResult res;
      PinosRingbufferArea areas[2];
      uint8_t *data;
      size_t size, towrite, total;
      SpaEventPullInput *pi;

      pi = event->data;

      g_debug ("pull ringbuffer %zd", pi->size);

      size = pi->size;
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

      iinfo.port_id = event->port_id;
      iinfo.flags = SPA_INPUT_FLAG_NONE;
      iinfo.id = 0;
      iinfo.offset = 0;
      iinfo.size = total;

      g_debug ("push sink %d", iinfo.id);
      if ((res = spa_node_port_push_input (priv->sink, 1, &iinfo)) < 0)
        g_debug ("got error %d", res);
      break;
    }

    case SPA_EVENT_TYPE_ADD_POLL:
    {
      SpaPollItem *poll = event->data;

      g_debug ("add poll");
      priv->poll = *poll;
      priv->fds[0] = poll->fds[0];
      priv->n_fds = 1;
      priv->poll.fds = priv->fds;
      break;
    }

    default:
      g_debug ("got event %d", event->type);
      break;
  }
}

static void
create_pipeline (PinosSpaAlsaSink *this)
{
  PinosSpaAlsaSinkPrivate *priv = this->priv;
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  if ((res = make_node (&priv->sink, "spa/build/plugins/alsa/libspa-alsa.so", "alsa-sink")) < 0) {
    g_error ("can't create alsa-sink: %d", res);
    return;
  }
  spa_node_set_event_callback (priv->sink, on_sink_event, this);

  if ((res = spa_node_get_props (priv->sink, &props)) < 0)
    g_debug ("got get_props error %d", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.value = "hw:0";
  value.size = strlen (value.value)+1;
  spa_props_set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = spa_node_set_props (priv->sink, props)) < 0)
    g_debug ("got set_props error %d", res);
}

static void *
loop (void *user_data)
{
  PinosSpaAlsaSink *this = user_data;
  PinosSpaAlsaSinkPrivate *priv = this->priv;
  int r;

  g_debug ("spa-alsa-sink %p: enter thread", this);
  while (priv->running) {
    SpaPollNotifyData ndata;

    r = poll ((struct pollfd *) priv->fds, priv->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      g_debug ("spa-alsa-sink %p: select timeout", this);
      break;
    }
    if (priv->poll.after_cb) {
      ndata.fds = priv->poll.fds;
      ndata.n_fds = priv->poll.n_fds;
      ndata.user_data = priv->poll.user_data;
      priv->poll.after_cb (&ndata);
    }
  }
  g_debug ("spa-alsa-sink %p: leave thread", this);

  return NULL;
}

static void
start_pipeline (PinosSpaAlsaSink *sink)
{
  PinosSpaAlsaSinkPrivate *priv = sink->priv;
  SpaResult res;
  SpaCommand cmd;
  int err;

  g_debug ("spa-alsa-sink %p: starting pipeline", sink);

  cmd.type = SPA_COMMAND_START;
  if ((res = spa_node_send_command (priv->sink, &cmd)) < 0)
    g_debug ("got error %d", res);

  priv->running = true;
  if ((err = pthread_create (&priv->thread, NULL, loop, sink)) != 0) {
    g_debug ("spa-v4l2-source %p: can't create thread", strerror (err));
    priv->running = false;
  }
}

static void
stop_pipeline (PinosSpaAlsaSink *sink)
{
  PinosSpaAlsaSinkPrivate *priv = sink->priv;
  SpaResult res;
  SpaCommand cmd;

  g_debug ("spa-alsa-sink %p: stopping pipeline", sink);

  if (priv->running) {
    priv->running = false;
    pthread_join (priv->thread, NULL);
  }

  cmd.type = SPA_COMMAND_STOP;
  if ((res = spa_node_send_command (priv->sink, &cmd)) < 0)
    g_debug ("got error %d", res);
}

static void
destroy_pipeline (PinosSpaAlsaSink *sink)
{
  g_debug ("spa-alsa-sink %p: destroy pipeline", sink);
}

static gboolean
set_state (PinosNode      *node,
           PinosNodeState  state)
{
  PinosSpaAlsaSink *this = PINOS_SPA_ALSA_SINK (node);

  g_debug ("spa-alsa-sink %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_SUSPENDED:
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      break;

    case PINOS_NODE_STATE_IDLE:
      stop_pipeline (this);
      break;

    case PINOS_NODE_STATE_RUNNING:
      start_pipeline (this);
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  pinos_node_update_state (node, state);
  return TRUE;
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
on_activate (PinosPort *port, gpointer user_data)
{
  SinkPortData *data = user_data;
  PinosNode *node = PINOS_NODE (data->sink);

  g_debug ("port %p: activate", port);

  pinos_node_report_busy (node);
}

static void
on_deactivate (PinosPort *port, gpointer user_data)
{
  SinkPortData *data = user_data;
  PinosNode *node = PINOS_NODE (data->sink);

  g_debug ("port %p: deactivate", port);
  pinos_node_report_idle (node);
}

static void
free_sink_port_data (SinkPortData *data)
{
  g_slice_free (SinkPortData, data);
}

static SpaResult
negotiate_formats (PinosSpaAlsaSink *this)
{
  PinosSpaAlsaSinkPrivate *priv = this->priv;
  SpaResult res;
  SpaFormat *format;
  SpaProps *props;
  uint32_t val;
  SpaPropValue value;
  void *state = NULL;

  if ((res = spa_node_port_enum_formats (priv->sink, 0, &format, NULL, &state)) < 0)
    return res;

  props = &format->props;

  value.type = SPA_PROP_TYPE_UINT32;
  value.size = sizeof (uint32_t);
  value.value = &val;

  val = SPA_AUDIO_FORMAT_S16LE;
  if ((res = spa_props_set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_FORMAT), &value)) < 0)
    return res;
  val = SPA_AUDIO_LAYOUT_INTERLEAVED;
  if ((res = spa_props_set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_LAYOUT), &value)) < 0)
    return res;
  val = 44100;
  if ((res = spa_props_set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_RATE), &value)) < 0)
    return res;
  val = 2;
  if ((res = spa_props_set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_CHANNELS), &value)) < 0)
    return res;

  if ((res = spa_node_port_set_format (priv->sink, 0, 0, format)) < 0)
    return res;

  priv->ringbuffer = pinos_ringbuffer_new (PINOS_RINGBUFFER_MODE_READ, 64 * 1024);

  return SPA_RESULT_OK;
}

static void
free_mem_block (MemBlock *b)
{
  munmap (b->data, b->size);
  g_slice_free (MemBlock, b);
}

static gboolean
on_received_buffer (PinosPort   *port,
                    SpaBuffer   *buffer,
                    GError     **error,
                    gpointer     user_data)
{
  PinosSpaAlsaSink *this = user_data;
  PinosSpaAlsaSinkPrivate *priv = this->priv;
  unsigned int i;

  for (i = 0; i < buffer->n_datas; i++) {
    SpaData *d = &buffer->datas[i];
    PinosRingbufferArea areas[2];
    uint8_t *data;
    size_t size, towrite, total;

    if (d->type != SPA_DATA_TYPE_MEMPTR)
      continue;

    size = d->size;
    data = (guint8*)d->ptr + d->offset;

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
  spa_buffer_unref (buffer);

  return TRUE;
}

static void
on_format_change (GObject *obj,
                  GParamSpec *pspec,
                  gpointer user_data)
{
  SinkPortData *data = user_data;
  PinosNode *node = PINOS_NODE (data->sink);
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (node);
  GBytes *formats;

  g_object_get (obj, "format", &formats, NULL);
  if (formats) {
    g_debug ("port %p: format change %s", obj, (gchar*) g_bytes_get_data (formats, NULL));
    negotiate_formats (sink);
  }
}

static PinosPort *
add_port (PinosNode       *node,
          PinosDirection   direction,
          guint            id,
          GError         **error)
{
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (node);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;
  SinkPortData *data;
  GBytes *formats;

  data = g_slice_new0 (SinkPortData);
  data->sink = sink;
  data->id = id;
  data->port = PINOS_NODE_CLASS (pinos_spa_alsa_sink_parent_class)
                ->add_port (node, direction, id, error);

  pinos_port_set_received_buffer_cb (data->port, on_received_buffer, sink, NULL);

  formats = g_bytes_new ("ANY", strlen ("ANY") + 1);
  g_object_set (data->port, "possible-formats", formats, NULL);

  g_debug ("connecting signals");
  g_signal_connect (data->port, "activate", (GCallback) on_activate, data);
  g_signal_connect (data->port, "deactivate", (GCallback) on_deactivate, data);

  g_signal_connect (data->port, "notify::format", (GCallback) on_format_change, data);

  priv->ports = g_list_append (priv->ports, data);

  return data->port;
}

static gboolean
remove_port (PinosNode       *node,
             guint            id)
{
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (node);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;
  GList *walk;

  for (walk = priv->ports; walk; walk = g_list_next (walk)) {
    SinkPortData *data = walk->data;

    if (data->id == id) {
      free_sink_port_data (data);
      priv->ports = g_list_delete_link (priv->ports, walk);
      break;
    }
  }
  if (priv->ports == NULL)
    pinos_node_report_idle (node);

  return TRUE;
}

static void
sink_constructed (GObject * object)
{
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);

  create_pipeline (sink);

  G_OBJECT_CLASS (pinos_spa_alsa_sink_parent_class)->constructed (object);
}

static void
sink_finalize (GObject * object)
{
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;

  destroy_pipeline (sink);
  pinos_properties_free (priv->props);
  g_hash_table_unref (priv->mem_ids);

  G_OBJECT_CLASS (pinos_spa_alsa_sink_parent_class)->finalize (object);
}

static void
pinos_spa_alsa_sink_class_init (PinosSpaAlsaSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaAlsaSinkPrivate));

  gobject_class->constructed = sink_constructed;
  gobject_class->finalize = sink_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;

  node_class->set_state = set_state;
  node_class->add_port = add_port;
  node_class->remove_port = remove_port;
}

static void
pinos_spa_alsa_sink_init (PinosSpaAlsaSink * sink)
{
  PinosSpaAlsaSinkPrivate *priv;

  priv = sink->priv = PINOS_SPA_ALSA_SINK_GET_PRIVATE (sink);
  priv->props = pinos_properties_new (NULL, NULL);
  priv->mem_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) free_mem_block);
}

PinosNode *
pinos_spa_alsa_sink_new (PinosDaemon *daemon,
                         const gchar *name,
                         PinosProperties *properties)
{
  PinosNode *node;

  node = g_object_new (PINOS_TYPE_SPA_ALSA_SINK,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       NULL);

  return node;
}
