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

#include <gio/gio.h>

#include <spa/include/spa/node.h>
#include <spa/include/spa/audio/format.h>

#include "spa-alsa-sink.h"

#define PINOS_SPA_ALSA_SINK_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_ALSA_SINK, PinosSpaAlsaSinkPrivate))

struct _PinosSpaAlsaSinkPrivate
{
  PinosPort *input;

  PinosProperties *props;
  PinosRingbuffer *ringbuffer;

  SpaHandle *sink;
  const SpaNode *sink_node;
};

enum {
  PROP_0,
  PROP_POSSIBLE_FORMATS
};

G_DEFINE_TYPE (PinosSpaAlsaSink, pinos_spa_alsa_sink, PINOS_TYPE_SERVER_NODE);

static SpaResult
make_node (SpaHandle **handle, const SpaNode **node, const char *lib, const char *name)
{
  SpaResult res;
  void *hnd;
  SpaEnumHandleFactoryFunc enum_func;
  unsigned int i;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    g_error ("can't load %s: %s", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    g_error ("can't find enum function");
    return SPA_RESULT_ERROR;
  }

  for (i = 0; ;i++) {
    const SpaHandleFactory *factory;
    const void *iface;

    if ((res = enum_func (i, &factory)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        g_error ("can't enumerate factories: %d", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    if ((res = factory->instantiate (factory, handle)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    if ((res = (*handle)->get_interface (*handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      g_error ("can't get interface %d", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void
on_sink_event (SpaHandle *handle, SpaEvent *event, void *user_data)
{
  PinosSpaAlsaSink *this = user_data;
  PinosSpaAlsaSinkPrivate *priv = this->priv;

  switch (event->type) {
    case SPA_EVENT_TYPE_PULL_INPUT:
    {
      SpaBuffer *buf;
      SpaInputInfo iinfo;
      SpaOutputInfo oinfo;
      SpaResult res;
      PinosRingbufferArea areas[2];
      uint8_t *data;
      size_t size, towrite, total;

      buf = event->data;

      oinfo.port_id = 0;
      oinfo.flags = SPA_OUTPUT_FLAG_PULL;
      oinfo.buffer = buf;
      oinfo.event = NULL;

      g_debug ("pull ringbuffer %p", buf);

      size = buf->size;
      data = buf->datas[0].data;

      pinos_ringbuffer_get_read_areas (priv->ringbuffer, areas);

      total = MIN (size, areas[0].len + areas[1].len);
      g_debug ("total read %zd %zd", total, areas[0].len + areas[1].len);
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

      buf->size = total;

      iinfo.port_id = event->port_id;
      iinfo.flags = SPA_INPUT_FLAG_NONE;
      iinfo.buffer = oinfo.buffer;
      iinfo.event = oinfo.event;

      g_debug ("push sink %p", iinfo.buffer);
      if ((res = priv->sink_node->push_port_input (priv->sink, 1, &iinfo)) < 0)
        g_debug ("got error %d", res);
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

  if ((res = make_node (&priv->sink, &priv->sink_node, "spa/build/plugins/alsa/libspa-alsa.so", "alsa-sink")) < 0) {
    g_error ("can't create alsa-sink: %d", res);
    return;
  }
  priv->sink_node->set_event_callback (priv->sink, on_sink_event, this);

  if ((res = priv->sink_node->get_props (priv->sink, &props)) < 0)
    g_debug ("got get_props error %d", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.size = strlen ("hw:1")+1;
  value.value = "hw:1";
  props->set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = priv->sink_node->set_props (priv->sink, props)) < 0)
    g_debug ("got set_props error %d", res);
}

static void
start_pipeline (PinosSpaAlsaSink *sink)
{
  PinosSpaAlsaSinkPrivate *priv = sink->priv;
  SpaResult res;
  SpaCommand cmd;

  g_debug ("spa-alsa-sink %p: starting pipeline", sink);

  cmd.type = SPA_COMMAND_START;
  if ((res = priv->sink_node->send_command (priv->sink, &cmd)) < 0)
    g_debug ("got error %d", res);
}

static void
stop_pipeline (PinosSpaAlsaSink *sink)
{
  PinosSpaAlsaSinkPrivate *priv = sink->priv;
  SpaResult res;
  SpaCommand cmd;

  g_debug ("spa-alsa-sink %p: stopping pipeline", sink);

  cmd.type = SPA_COMMAND_STOP;
  if ((res = priv->sink_node->send_command (priv->sink, &cmd)) < 0)
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
  PinosSpaAlsaSinkPrivate *priv = this->priv;

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
      //start_pipeline (this);
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
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;

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
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
on_linked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  PinosNode *node = user_data;
  guint n_links;

  g_debug ("port %p: linked", port);

  pinos_port_get_links (port, &n_links);
  if (n_links == 1)
    pinos_node_report_busy (node);
}

static void
on_unlinked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  PinosNode *node = user_data;
  guint n_links;

  g_debug ("port %p: unlinked", port);
  pinos_port_get_links (port, &n_links);
  if (n_links == 1)
    pinos_node_report_idle (node);
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

  if ((res = priv->sink_node->enum_port_formats (priv->sink, 0, 0, &format)) < 0)
    return res;

  props = &format->props;

  value.type = SPA_PROP_TYPE_UINT32;
  value.size = sizeof (uint32_t);
  value.value = &val;

  val = SPA_AUDIO_FORMAT_S16LE;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_FORMAT), &value)) < 0)
    return res;
  val = SPA_AUDIO_LAYOUT_INTERLEAVED;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_LAYOUT), &value)) < 0)
    return res;
  val = 44100;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_RATE), &value)) < 0)
    return res;
  val = 2;
  if ((res = props->set_prop (props, spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_CHANNELS), &value)) < 0)
    return res;

  if ((res = priv->sink_node->set_port_format (priv->sink, 0, 0, format)) < 0)
    return res;

  priv->ringbuffer = pinos_ringbuffer_new (PINOS_RINGBUFFER_MODE_READ, 64 * 1024);

  g_object_set (priv->input, "ringbuffer", priv->ringbuffer, NULL);

  return SPA_RESULT_OK;
}

static void
on_received_buffer (PinosPort  *port,
                    gpointer    user_data)
{
  PinosSpaAlsaSink *this = user_data;
  PinosSpaAlsaSinkPrivate *priv = this->priv;
  PinosBuffer *pbuf;
  PinosBufferIter it;

  pbuf = pinos_port_peek_buffer (port);

  pinos_buffer_iter_init (&it, pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_HEADER:
      {
        PinosPacketHeader hdr;

        if (!pinos_buffer_iter_parse_header  (&it, &hdr))
          break;

        break;
      }

      case PINOS_PACKET_TYPE_FD_PAYLOAD:
      {
        PinosPacketFDPayload p;
        int fd;
        PinosRingbufferArea areas[2];
        uint8_t *data, *d;
        size_t size, towrite, total;

        if (!pinos_buffer_iter_parse_fd_payload  (&it, &p))
          break;

        g_debug ("got fd payload id %d", p.id);
        fd = pinos_buffer_get_fd (pbuf, p.fd_index);
        if (fd == -1)
          break;

        d = data = mmap (NULL, p.size, PROT_READ, MAP_PRIVATE, fd, p.offset);
        size = p.size;

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

        munmap (d, p.size);
        break;
      }
      case PINOS_PACKET_TYPE_FORMAT_CHANGE:
      {
        PinosPacketFormatChange change;

        if (!pinos_buffer_iter_parse_format_change  (&it, &change))
          break;
        g_debug ("got format change %d %s", change.id, change.format);

        negotiate_formats (this);
        start_pipeline (this);
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_iter_end (&it);
}

static void
on_input_port_created (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  PinosNode *node = PINOS_NODE (source_object);
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (node);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;

  priv->input = pinos_node_create_port_finish (node, res, NULL);

  pinos_port_set_received_buffer_cb (priv->input, on_received_buffer, sink, NULL);

  g_signal_connect (priv->input, "linked", (GCallback) on_linked, node);
  g_signal_connect (priv->input, "unlinked", (GCallback) on_unlinked, node);

  create_pipeline (sink);
}

static void
sink_constructed (GObject * object)
{
  PinosServerNode *node = PINOS_SERVER_NODE (object);
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;

  G_OBJECT_CLASS (pinos_spa_alsa_sink_parent_class)->constructed (object);

  pinos_node_create_port (PINOS_NODE (node),
                          PINOS_DIRECTION_INPUT,
                          "input",
                          NULL,
                          NULL,
			  NULL,
                          on_input_port_created,
                          node);
}

static void
sink_finalize (GObject * object)
{
  PinosServerNode *node = PINOS_SERVER_NODE (object);
  PinosSpaAlsaSink *sink = PINOS_SPA_ALSA_SINK (object);
  PinosSpaAlsaSinkPrivate *priv = sink->priv;

  destroy_pipeline (sink);
  pinos_node_remove_port (PINOS_NODE (node), priv->input);
  pinos_properties_free (priv->props);

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
}

static void
pinos_spa_alsa_sink_init (PinosSpaAlsaSink * sink)
{
  PinosSpaAlsaSinkPrivate *priv;

  priv = sink->priv = PINOS_SPA_ALSA_SINK_GET_PRIVATE (sink);
  priv->props = pinos_properties_new (NULL, NULL);
}

PinosServerNode *
pinos_spa_alsa_sink_new (PinosDaemon *daemon,
                         const gchar *name,
                         PinosProperties *properties)
{
  PinosServerNode *node;

  node = g_object_new (PINOS_TYPE_SPA_ALSA_SINK,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       NULL);

  return node;
}
