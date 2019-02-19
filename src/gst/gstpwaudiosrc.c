/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstpwaudiosrc.h"

GST_DEBUG_CATEGORY_STATIC (pw_audio_src_debug);
#define GST_CAT_DEFAULT pw_audio_src_debug

G_DEFINE_TYPE (GstPwAudioSrc, gst_pw_audio_src, GST_TYPE_AUDIO_BASE_SRC);

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
  PROP_FD
};

static GstStaticPadTemplate gst_pw_audio_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_AUDIO_CAPS_MAKE (GST_AUDIO_NE (F32))
                     ", layout = (string)\"interleaved\"")
);


static void
gst_pw_audio_src_init (GstPwAudioSrc * self)
{
  self->props.fd = -1;
}

static void
gst_pw_audio_src_finalize (GObject * object)
{
  GstPwAudioSrc *self = GST_PW_AUDIO_SRC (object);

  g_free (self->props.path);
  g_free (self->props.client_name);
  if (self->props.properties)
    gst_structure_free (self->props.properties);
}

static void
gst_pw_audio_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPwAudioSrc *self = GST_PW_AUDIO_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (self->props.path);
      self->props.path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (self->props.client_name);
      self->props.client_name = g_value_dup_string (value);
      break;

    case PROP_STREAM_PROPERTIES:
      if (self->props.properties)
        gst_structure_free (self->props.properties);
      self->props.properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_FD:
      self->props.fd = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pw_audio_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPwAudioSrc *self = GST_PW_AUDIO_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, self->props.path);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, self->props.client_name);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, self->props.properties);
      break;

    case PROP_FD:
      g_value_set_int (value, self->props.fd);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstAudioRingBuffer *
gst_pw_audio_src_create_ringbuffer (GstAudioBaseSrc * sink)
{
  GstPwAudioSrc *self = GST_PW_AUDIO_SRC (sink);
  GstAudioRingBuffer *buffer;

  GST_DEBUG_OBJECT (sink, "creating ringbuffer");
  buffer = g_object_new (GST_TYPE_PW_AUDIO_RING_BUFFER,
      "element", sink,
      "direction", PW_DIRECTION_INPUT,
      "props", &self->props,
      NULL);
  GST_DEBUG_OBJECT (sink, "created ringbuffer @%p", buffer);

  return buffer;
}

static void
gst_pw_audio_src_class_init (GstPwAudioSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstAudioBaseSrcClass *gstaudiobsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstaudiobsrc_class = (GstAudioBaseSrcClass *) klass;

  gobject_class->finalize = gst_pw_audio_src_finalize;
  gobject_class->set_property = gst_pw_audio_src_set_property;
  gobject_class->get_property = gst_pw_audio_src_get_property;

  gstaudiobsrc_class->create_ringbuffer = gst_pw_audio_src_create_ringbuffer;

  g_object_class_install_property (gobject_class, PROP_PATH,
      g_param_spec_string ("path", "Path",
          "The sink path to connect to (NULL = default)", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CLIENT_NAME,
      g_param_spec_string ("client-name", "Client Name",
          "The client name to use (NULL = default)", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class, PROP_STREAM_PROPERTIES,
      g_param_spec_boxed ("stream-properties", "Stream properties",
          "List of PipeWire stream properties", GST_TYPE_STRUCTURE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class, PROP_FD,
      g_param_spec_int ("fd", "Fd", "The fd to connect with", -1, G_MAXINT, -1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire Audio source", "Source/Audio",
      "Receive audio from PipeWire",
      "George Kiagiadakis <george.kiagiadakis@collabora.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pw_audio_src_template));

  GST_DEBUG_CATEGORY_INIT (pw_audio_src_debug, "pwaudiosrc", 0,
      "PipeWire Audio Src");
}

