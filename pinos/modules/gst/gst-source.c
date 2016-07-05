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
#include <gst/gst.h>
#include <gio/gio.h>

#include <gst/net/net.h>

#include "gst-source.h"

#define PINOS_GST_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_GST_SOURCE, PinosGstSourcePrivate))

typedef struct {
  PinosGstSource *source;

  PinosServerPort *port;

  GstElement *convert;
  GstElement *sink;

  GstPad *peerpad;
  GstPad *sinkpad;
} SourcePortData;

struct _PinosGstSourcePrivate
{
  gchar *convert_name;

  GstElement *pipeline;
  GstElement *element;
  GstElement *splitter;

  GList *ports;
  GstCaps *possible_formats;

  GstNetTimeProvider *provider;
};

enum {
  PROP_0,
  PROP_ELEMENT,
  PROP_POSSIBLE_FORMATS,
  PROP_SPLITTER,
  PROP_CONVERT_NAME
};

G_DEFINE_TYPE (PinosGstSource, pinos_gst_source, PINOS_TYPE_SERVER_NODE);

static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosNode *node = user_data;
  PinosGstSourcePrivate *priv = PINOS_GST_SOURCE (node)->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_warning ("got error %s (%s)\n", error->message, debug);
      g_free (debug);

      pinos_node_report_error (node, error);
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;
    }
    case GST_MESSAGE_NEW_CLOCK:
    {
      GstClock *clock;
      PinosProperties *props;

      gst_message_parse_new_clock (message, &clock);
      GST_INFO ("got new clock %s", GST_OBJECT_NAME (clock));

      g_object_get (node, "properties", &props, NULL);
      pinos_properties_set (props, "gst.pipeline.clock", GST_OBJECT_NAME (clock));
      g_object_set (node, "properties", props, NULL);
      pinos_properties_free (props);
      break;
    }
    case GST_MESSAGE_CLOCK_LOST:
    {
      GstClock *clock;
      PinosProperties *props;

      gst_message_parse_new_clock (message, &clock);
      GST_INFO ("clock lost %s", GST_OBJECT_NAME (clock));

      g_object_get (node, "properties", &props, NULL);
      pinos_properties_remove (props, "gst.pipeline.clock");
      g_object_set (node, "properties", props, NULL);
      pinos_properties_free (props);

      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static gboolean
setup_pipeline (PinosGstSource *source, GError **error)
{
  PinosGstSourcePrivate *priv = source->priv;
  GstBus *bus;

  g_debug ("gst-source %p: setup pipeline", source);
  priv->pipeline = gst_pipeline_new (NULL);
  gst_pipeline_set_latency (GST_PIPELINE_CAST (priv->pipeline), 0);

  gst_bin_add (GST_BIN (priv->pipeline), priv->element);

  if (priv->splitter) {
    gst_bin_add (GST_BIN (priv->pipeline), priv->splitter);
    gst_element_link (priv->element, priv->splitter);
  }

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  gst_bus_add_watch (bus, bus_handler, source);
  gst_object_unref (bus);

  return TRUE;
}

#if 0
static gboolean
start_pipeline (PinosGstSource *source, GError **error)
{
  PinosGstSourcePrivate *priv = source->priv;
  GstCaps *caps;
  GstQuery *query;
  GstStateChangeReturn ret;
  gchar *str;

  g_debug ("gst-source %p: starting pipeline", source);

  ret = gst_element_set_state (priv->pipeline, GST_STATE_READY);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto ready_failed;

  query = gst_query_new_caps (NULL);
  if (gst_element_query (priv->element, query)) {
    gst_query_parse_caps_result (query, &caps);
    gst_caps_replace (&priv->possible_formats, caps);
    str = gst_caps_to_string (caps);
    g_debug ("gst-source %p: updated possible formats %s", source, str);
    g_free (str);
  }
  gst_query_unref (query);

  return TRUE;

  /* ERRORS */
ready_failed:
  {
    GST_ERROR_OBJECT (source, "failed state change to READY");
    gst_element_set_state (priv->pipeline, GST_STATE_NULL);
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to start pipeline");
    return FALSE;
  }
}
#endif

static void
stop_pipeline (PinosGstSource *source)
{
  PinosGstSourcePrivate *priv = source->priv;

  g_debug ("gst-source %p: stopping pipeline", source);
  gst_element_set_state (priv->pipeline, GST_STATE_NULL);
  g_clear_object (&priv->provider);
}

static void
destroy_pipeline (PinosGstSource *source)
{
  PinosGstSourcePrivate *priv = source->priv;

  g_debug ("gst-source %p: destroy pipeline", source);
  stop_pipeline (source);
  g_clear_object (&priv->pipeline);
}


static gboolean
set_state (PinosNode      *node,
           PinosNodeState  state)
{
  PinosGstSourcePrivate *priv = PINOS_GST_SOURCE (node)->priv;

  g_debug ("gst-source %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_SUSPENDED:
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      gst_element_set_state (priv->pipeline, GST_STATE_READY);
      break;

    case PINOS_NODE_STATE_IDLE:
      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      break;

    case PINOS_NODE_STATE_RUNNING:
    {
      GstQuery *query;
      GstClock *clock;
      gchar *address;
      gint port;
      PinosProperties *props;
      GstClockTime base_time;
      gboolean live;
      GstClockTime min_latency, max_latency;

      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      gst_element_get_state (priv->pipeline, NULL, NULL, -1);
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      gst_element_get_state (priv->pipeline, NULL, NULL, -1);

      clock = gst_pipeline_get_clock (GST_PIPELINE (priv->pipeline));
      base_time = gst_clock_get_time (clock);

      if (priv->provider)
        g_object_unref (priv->provider);
      priv->provider = gst_net_time_provider_new (clock, NULL, 0);

      g_object_get (priv->provider, "address", &address, "port", &port, NULL);

      g_object_get (node, "properties", &props, NULL);

      pinos_properties_set (props, "pinos.clock.type", "gst.net.time.provider");
      pinos_properties_set (props, "pinos.clock.source", GST_OBJECT_NAME (clock));
      pinos_properties_set (props, "pinos.clock.address", address);
      pinos_properties_setf (props, "pinos.clock.port", "%d", port);
      pinos_properties_setf (props, "pinos.clock.base-time", "%"G_GUINT64_FORMAT, base_time);

      g_free (address);
      gst_object_unref (clock);

      query = gst_query_new_latency ();
      if (gst_element_query (GST_ELEMENT_CAST (priv->pipeline), query)) {
        gst_query_parse_latency (query, &live, &min_latency, &max_latency);
      } else {
        live = FALSE;
        min_latency = 0;
        max_latency = -1;
      }
      gst_query_unref (query);

      GST_DEBUG_OBJECT (priv->pipeline,
          "got min latency %" GST_TIME_FORMAT ", max latency %"
          GST_TIME_FORMAT ", live %d", GST_TIME_ARGS (min_latency),
          GST_TIME_ARGS (max_latency), live);

      pinos_properties_setf (props, "pinos.latency.is-live", "%d", live);
      pinos_properties_setf (props, "pinos.latency.min", "%"G_GUINT64_FORMAT, min_latency);
      pinos_properties_setf (props, "pinos.latency.max", "%"G_GUINT64_FORMAT, max_latency);

      g_object_set (node, "properties", props, NULL);
      pinos_properties_free (props);
      break;
    }
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
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_ELEMENT:
      g_value_set_object (value, priv->element);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    case PROP_SPLITTER:
      g_value_set_object (value, priv->splitter);
      break;

    case PROP_CONVERT_NAME:
      g_value_set_string (value, priv->convert_name);
      break;

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
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_ELEMENT:
      priv->element = g_value_dup_object (value);
      break;

    case PROP_POSSIBLE_FORMATS:
      priv->possible_formats = g_value_dup_boxed (value);
      break;

    case PROP_SPLITTER:
      priv->splitter = g_value_dup_object (value);
      break;

    case PROP_CONVERT_NAME:
      priv->convert_name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
on_linked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  SourcePortData *data = user_data;
  PinosGstSource *source = data->source;
  PinosGstSourcePrivate *priv = source->priv;
  guint n_links;

  pinos_port_get_links (port, &n_links);
  g_debug ("port %p: linked, now %d", port, n_links);
  if (n_links > 0)
    return TRUE;

  if (priv->splitter) {
    data->peerpad = gst_element_get_request_pad (priv->splitter, "src_%u");
  } else {
    data->peerpad = gst_element_get_static_pad (priv->element, "src");
  }
  if (gst_pad_link (data->peerpad, data->sinkpad) != GST_PAD_LINK_OK) {
    g_clear_object (&data->peerpad);
    return FALSE;
  }

  pinos_node_report_busy (PINOS_NODE (source));

  if (data->convert) {
    gst_element_set_state (data->convert, GST_STATE_PLAYING);
  }
  gst_element_set_state (data->sink, GST_STATE_PLAYING);

  return TRUE;
}

static void
on_unlinked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  SourcePortData *data = user_data;
  PinosGstSource *source = data->source;
  PinosGstSourcePrivate *priv = source->priv;
  guint n_links;

  pinos_port_get_links (port, &n_links);
  g_debug ("port %p: unlinked, now %d", port, n_links);
  if (n_links > 0)
    return;

  if (data->convert) {
    gst_element_set_state (data->convert, GST_STATE_NULL);
  }
  gst_element_set_state (data->sink, GST_STATE_NULL);

  gst_pad_unlink (data->peerpad, data->sinkpad);
  if (priv->splitter)
    gst_element_release_request_pad (priv->splitter, data->peerpad);
  g_clear_object (&data->peerpad);
}

static void
free_source_port_data (SourcePortData *data)
{
  PinosGstSource *source = data->source;
  PinosGstSourcePrivate *priv = source->priv;

  gst_element_set_state (data->sink, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (priv->pipeline), data->sink);

  if (data->convert) {
    gst_element_set_state (data->convert, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (priv->pipeline), data->convert);
  }
  if (data->peerpad)
    gst_element_release_request_pad (priv->splitter, data->peerpad);

  g_clear_object (&data->peerpad);
  g_clear_object (&data->sinkpad);

  g_slice_free (SourcePortData, data);
}

static void
remove_port (PinosNode       *node,
             PinosPort       *port)
{
  PinosGstSource *source = PINOS_GST_SOURCE (node);
  PinosGstSourcePrivate *priv = source->priv;
  GList *walk;

  for (walk = priv->ports; walk; walk = g_list_next (walk)) {
    SourcePortData *data = walk->data;

    if (data->port == PINOS_SERVER_PORT_CAST (port)) {
      free_source_port_data (data);
      priv->ports = g_list_delete_link (priv->ports, walk);
      break;
    }
  }
  if (priv->ports == NULL)
    pinos_node_report_idle (node);
}

static void
source_constructed (GObject * object)
{
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;

  G_OBJECT_CLASS (pinos_gst_source_parent_class)->constructed (object);

  if (priv->element)
    setup_pipeline (source, NULL);
}

static void
source_finalize (GObject * object)
{
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;

  g_debug ("gst-source %p: dispose", source);
  destroy_pipeline (source);
  g_clear_pointer (&priv->possible_formats, gst_caps_unref);

  G_OBJECT_CLASS (pinos_gst_source_parent_class)->finalize (object);
}

static gboolean
factory_filter (GstPluginFeature * feature, gpointer data)
{
  guint rank;
  const gchar *klass;

  if (!GST_IS_ELEMENT_FACTORY (feature))
    return FALSE;

  rank = gst_plugin_feature_get_rank (feature);
  if (rank < 1)
    return FALSE;

  klass = gst_element_factory_get_metadata (GST_ELEMENT_FACTORY (feature),
      GST_ELEMENT_METADATA_KLASS);
  if (g_strcmp0 (klass, "Source/Video") && g_strcmp0 (klass, "Source/Audio"))
    return FALSE;

  return TRUE;
}

static GstElement *
create_best_element (GstCaps *caps)
{
  GstElement *element = NULL;
  GList *list, *item;

  /* get factories from registry */
  list = gst_registry_feature_filter (gst_registry_get (),
                                      (GstPluginFeatureFilter) factory_filter,
                                      FALSE, NULL);
  list = g_list_sort (list,
                      (GCompareFunc) gst_plugin_feature_rank_compare_func);

  /* loop through list and try to find factory that best matches caps,
   * following the pattern from GstAutoDetect */
  for (item = list; item != NULL; item = item->next) {
    GstElementFactory *f = GST_ELEMENT_FACTORY (item->data);
    GstElement *el;
    GstPad *el_pad;
    GstCaps *el_caps = NULL;
    gboolean match = FALSE;
    GstStateChangeReturn ret;

    if ((el = gst_element_factory_create (f, NULL))) {
      el_pad = gst_element_get_static_pad (el, "src");
      el_caps = gst_pad_query_caps (el_pad, NULL);
      gst_object_unref (el_pad);
      match = gst_caps_can_intersect (caps, el_caps);
      gst_caps_unref (el_caps);

      if (!match) {
        gst_object_unref (el);
        continue;
      }
    }

    ret = gst_element_set_state (el, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_SUCCESS) {
      element = el;
      g_debug ("element %p selected", element);
      break;
    }

    gst_element_set_state (el, GST_STATE_NULL);
    gst_object_unref (el);
  }

  return element;
}

static PinosServerPort *
create_port_sync (PinosServerNode *node,
                  PinosDirection   direction,
                  const gchar     *name,
                  GBytes          *possible_formats,
                  PinosProperties *props)
{
  PinosGstSource *source = PINOS_GST_SOURCE (node);
  PinosGstSourcePrivate *priv = source->priv;
  SourcePortData *data;

  if (priv->element == NULL) {
    GstCaps *caps;

    caps = gst_caps_from_string (g_bytes_get_data (possible_formats, NULL));
    priv->element = create_best_element (caps);
    gst_caps_unref (caps);

    if (priv->element) {
      if (props == NULL)
        props = pinos_properties_new (NULL, NULL);
      pinos_properties_set (props, "autoconnect", "0");
      setup_pipeline (source, NULL);
    }
  }

  data = g_slice_new0 (SourcePortData);
  data->source = source;

  data->port = PINOS_SERVER_NODE_CLASS (pinos_gst_source_parent_class)
                ->create_port_sync (node,
                                    direction,
                                    name,
                                    possible_formats,
                                    props);

  g_debug ("connecting signals");
  g_signal_connect (data->port, "linked", (GCallback) on_linked, data);
  g_signal_connect (data->port, "unlinked", (GCallback) on_unlinked, data);

  data->sink = gst_element_factory_make ("pinosportsink", NULL);
  g_object_set (data->sink, "sync", TRUE,
                            "enable-last-sample", FALSE,
                            "qos", FALSE,
                            "port", data->port,
                            NULL);
  gst_bin_add (GST_BIN (priv->pipeline), data->sink);

  if (priv->convert_name) {
    data->convert = gst_element_factory_make (priv->convert_name, NULL);
    gst_bin_add (GST_BIN (priv->pipeline), data->convert);
    gst_element_link (data->convert, data->sink);
    data->sinkpad = gst_element_get_static_pad (data->convert, "sink");
  } else {
    data->sinkpad = gst_element_get_static_pad (data->sink, "sink");
  }

  priv->ports = g_list_append (priv->ports, data);

  return data->port;
}

static void
pinos_gst_source_class_init (PinosGstSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);
  PinosServerNodeClass *server_node_class = PINOS_SERVER_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosGstSourcePrivate));

  gobject_class->constructed = source_constructed;
  gobject_class->finalize = source_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_ELEMENT,
                                   g_param_spec_object ("element",
                                                        "Element",
                                                        "The element",
                                                        GST_TYPE_ELEMENT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Formats",
                                                       "The possible formats",
                                                       GST_TYPE_CAPS,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SPLITTER,
                                   g_param_spec_object ("splitter",
                                                        "Splitter",
                                                        "The splitter element",
                                                        GST_TYPE_ELEMENT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_CONVERT_NAME,
                                   g_param_spec_string ("convert-name",
                                                        "Convert name",
                                                        "The converter element name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  node_class->set_state = set_state;
  node_class->remove_port = remove_port;

  server_node_class->create_port_sync = create_port_sync;
}

static void
pinos_gst_source_init (PinosGstSource * source)
{
  source->priv = PINOS_GST_SOURCE_GET_PRIVATE (source);
}

PinosServerNode *
pinos_gst_source_new (PinosDaemon *daemon,
                      const gchar *name,
                      PinosProperties *properties,
                      GstElement  *element,
                      GstCaps     *caps,
                      GstElement  *splitter,
                      const gchar *convert_name)
{
  PinosServerNode *node;

  node = g_object_new (PINOS_TYPE_GST_SOURCE,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "element", element,
                       "possible-formats", caps,
                       "splitter", splitter,
                       "convert-name", convert_name,
                       NULL);

  return node;
}
