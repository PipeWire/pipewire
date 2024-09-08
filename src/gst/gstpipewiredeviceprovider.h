/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_DEVICE_PROVIDER_H__
#define __GST_PIPEWIRE_DEVICE_PROVIDER_H__

#include "config.h"

#include <gst/gst.h>

#include <gst/gstpipewirecore.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_DEVICE (gst_pipewire_device_get_type())
#define GST_PIPEWIRE_DEVICE_CAST(obj) ((GstPipeWireDevice *)(obj))
G_DECLARE_FINAL_TYPE (GstPipeWireDevice, gst_pipewire_device, GST, PIPEWIRE_DEVICE, GstDevice)

typedef enum {
  GST_PIPEWIRE_DEVICE_TYPE_UNKNOWN,
  GST_PIPEWIRE_DEVICE_TYPE_SOURCE,
  GST_PIPEWIRE_DEVICE_TYPE_SINK,
} GstPipeWireDeviceType;

struct _GstPipeWireDevice {
  GstDevice           parent;

  GstPipeWireDeviceType  type;
  uint32_t            id;
  uint64_t            serial;
  int                 fd;
  const gchar        *element;
  int                 priority;
};

#define GST_TYPE_PIPEWIRE_DEVICE_PROVIDER (gst_pipewire_device_provider_get_type())
#define GST_PIPEWIRE_DEVICE_PROVIDER_CAST(obj) ((GstPipeWireDeviceProvider *)(obj))
G_DECLARE_FINAL_TYPE (GstPipeWireDeviceProvider, gst_pipewire_device_provider, GST, PIPEWIRE_DEVICE_PROVIDER, GstDeviceProvider)

struct _GstPipeWireDeviceProvider {
  GstDeviceProvider         parent;

  gchar *client_name;
  int fd;

  GstPipeWireCore *core;
  struct spa_hook core_listener;
  struct pw_registry *registry;
  struct spa_hook registry_listener;

  struct pw_metadata *metadata;
  struct spa_hook metadata_listener;

  gchar *default_audio_source_name;
  gchar *default_audio_sink_name;
  gchar *default_video_source_name;

  struct spa_list nodes;
  int seq;

  int error;
  gboolean end;
  gboolean list_only;
  GList *devices;
};

G_END_DECLS

#endif /* __GST_PIPEWIRE_DEVICE_PROVIDER_H__ */
