/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_DEVICE_PROVIDER_H__
#define __GST_PIPEWIRE_DEVICE_PROVIDER_H__

#include "config.h"

#include <gst/gst.h>

#include <pipewire/pipewire.h>
#include <gst/gstpipewirecore.h>

G_BEGIN_DECLS

typedef struct _GstPipeWireDevice GstPipeWireDevice;
typedef struct _GstPipeWireDeviceClass GstPipeWireDeviceClass;

#define GST_TYPE_PIPEWIRE_DEVICE                 (gst_pipewire_device_get_type())
#define GST_IS_PIPEWIRE_DEVICE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_PIPEWIRE_DEVICE))
#define GST_IS_PIPEWIRE_DEVICE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_PIPEWIRE_DEVICE))
#define GST_PIPEWIRE_DEVICE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_PIPEWIRE_DEVICE, GstPipeWireDeviceClass))
#define GST_PIPEWIRE_DEVICE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_PIPEWIRE_DEVICE, GstPipeWireDevice))
#define GST_PIPEWIRE_DEVICE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_DEVICE, GstPipeWireDeviceClass))
#define GST_PIPEWIRE_DEVICE_CAST(obj)            ((GstPipeWireDevice *)(obj))

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

struct _GstPipeWireDeviceClass {
  GstDeviceClass    parent_class;
};

GType        gst_pipewire_device_get_type (void);

typedef struct _GstPipeWireDeviceProvider GstPipeWireDeviceProvider;
typedef struct _GstPipeWireDeviceProviderClass GstPipeWireDeviceProviderClass;

#define GST_TYPE_PIPEWIRE_DEVICE_PROVIDER                 (gst_pipewire_device_provider_get_type())
#define GST_IS_PIPEWIRE_DEVICE_PROVIDER(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_PIPEWIRE_DEVICE_PROVIDER))
#define GST_IS_PIPEWIRE_DEVICE_PROVIDER_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_PIPEWIRE_DEVICE_PROVIDER))
#define GST_PIPEWIRE_DEVICE_PROVIDER_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_PIPEWIRE_DEVICE_PROVIDER, GstPipeWireDeviceProviderClass))
#define GST_PIPEWIRE_DEVICE_PROVIDER(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_PIPEWIRE_DEVICE_PROVIDER, GstPipeWireDeviceProvider))
#define GST_PIPEWIRE_DEVICE_PROVIDER_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_DEVICE_PROVIDER, GstPipeWireDeviceProviderClass))
#define GST_PIPEWIRE_DEVICE_PROVIDER_CAST(obj)            ((GstPipeWireDeviceProvider *)(obj))

struct _GstPipeWireDeviceProvider {
  GstDeviceProvider         parent;

  gchar *client_name;
  int fd;

  GstPipeWireCore *core;
  struct spa_hook core_listener;
  struct pw_registry *registry;
  struct spa_hook registry_listener;
  struct spa_list nodes;
  int seq;

  int error;
  gboolean end;
  gboolean list_only;
  GList *devices;
};

struct _GstPipeWireDeviceProviderClass {
  GstDeviceProviderClass    parent_class;
};

GType        gst_pipewire_device_provider_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_DEVICE_PROVIDER_H__ */
