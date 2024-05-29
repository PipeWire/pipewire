/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2024 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#include "gstpipewirestream.h"

GST_DEBUG_CATEGORY_STATIC (pipewire_stream_debug);
#define GST_CAT_DEFAULT pipewire_stream_debug

G_DEFINE_TYPE (GstPipeWireStream, gst_pipewire_stream, GST_TYPE_OBJECT)

static void
gst_pipewire_stream_init (GstPipeWireStream * self)
{
  self->fd = -1;
  self->client_name = g_strdup (pw_get_client_name());
  self->pool = gst_pipewire_pool_new ();
}

static void
gst_pipewire_stream_finalize (GObject * object)
{
  GstPipeWireStream * self = GST_PIPEWIRE_STREAM (object);

  g_clear_object (&self->pool);
  g_free (self->path);
  g_free (self->target_object);
  g_free (self->client_name);
  gst_clear_structure (&self->client_properties);
  gst_clear_structure (&self->stream_properties);

  G_OBJECT_CLASS(gst_pipewire_stream_parent_class)->finalize (object);
}

void
gst_pipewire_stream_class_init (GstPipeWireStreamClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_pipewire_stream_finalize;

  GST_DEBUG_CATEGORY_INIT (pipewire_stream_debug, "pipewirestream", 0,
      "PipeWire Stream");
}

GstPipeWireStream *
gst_pipewire_stream_new (GstElement * element)
{
  GstPipeWireStream *stream;

  stream = g_object_new (GST_TYPE_PIPEWIRE_STREAM, NULL);
  stream->element = element;

  return stream;
}

static gboolean
copy_properties (GQuark field_id,
                 const GValue *value,
                 gpointer user_data)
{
  struct pw_properties *properties = user_data;
  GValue dst = { 0 };

  if (g_value_type_transformable (G_VALUE_TYPE(value), G_TYPE_STRING)) {
    g_value_init (&dst, G_TYPE_STRING);
    if (g_value_transform (value, &dst)) {
      pw_properties_set (properties,
                         g_quark_to_string (field_id),
                         g_value_get_string (&dst));
    }
    g_value_unset (&dst);
  }
  return TRUE;
}

gboolean
gst_pipewire_stream_open (GstPipeWireStream * self,
    const struct pw_stream_events * pwstream_events)
{
  struct pw_properties *props;

  g_return_val_if_fail (self->core == NULL, FALSE);

  GST_DEBUG_OBJECT (self, "open");

  /* acquire the core */
  self->core = gst_pipewire_core_get (self->fd);
  if (self->core == NULL)
      goto connect_error;

  pw_thread_loop_lock (self->core->loop);

  /* update the client properties */
  if (self->client_properties) {
    props = pw_properties_new (NULL, NULL);
    gst_structure_foreach (self->client_properties, copy_properties, props);
    pw_core_update_properties (self->core->core, &props->dict);
    pw_properties_free (props);
  }

  /* create stream */
  props = pw_properties_new (NULL, NULL);
  if (self->client_name) {
    pw_properties_set (props, PW_KEY_NODE_NAME, self->client_name);
    pw_properties_set (props, PW_KEY_NODE_DESCRIPTION, self->client_name);
  }
  if (self->stream_properties) {
    gst_structure_foreach (self->stream_properties, copy_properties, props);
  }

  if ((self->pwstream = pw_stream_new (self->core->core,
                                       self->client_name, props)) == NULL)
    goto no_stream;

  pw_stream_add_listener(self->pwstream,
                         &self->pwstream_listener,
                         pwstream_events,
                         self->element);

  /* create clock */
  self->clock = gst_pipewire_clock_new (self->pwstream, 0);

  self->pool->stream = self->pwstream;

  pw_thread_loop_unlock (self->core->loop);

  return TRUE;

  /* ERRORS */
connect_error:
  {
    GST_ELEMENT_ERROR (self->element, RESOURCE, FAILED,
        ("Failed to connect"), (NULL));
    return FALSE;
  }
no_stream:
  {
    GST_ELEMENT_ERROR (self->element, RESOURCE, FAILED,
        ("can't create stream"), (NULL));
    pw_thread_loop_unlock (self->core->loop);
    return FALSE;
  }
}

void
gst_pipewire_stream_close (GstPipeWireStream * self)
{
  GST_DEBUG_OBJECT (self, "close");

  self->pool->stream = NULL;

  /* destroy the clock */
  gst_element_post_message (GST_ELEMENT (self->element),
    gst_message_new_clock_lost (GST_OBJECT_CAST (self->element), self->clock));

  GST_PIPEWIRE_CLOCK (self->clock)->stream = NULL;
  g_clear_object (&self->clock);

  /* destroy the pw stream */
  pw_thread_loop_lock (self->core->loop);
  g_clear_pointer (&self->pwstream, pw_stream_destroy);
  pw_thread_loop_unlock (self->core->loop);

  /* release the core */
  g_clear_pointer (&self->core, gst_pipewire_core_release);
}
