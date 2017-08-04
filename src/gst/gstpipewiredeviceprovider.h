/* GStreamer
 * Copyright (C) 2012 Olivier Crete <olivier.crete@collabora.com>
 *           (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * pipewiredeviceprovider.h: Device probing and monitoring
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifndef __GST_PIPEWIRE_DEVICE_PROVIDER_H__
#define __GST_PIPEWIRE_DEVICE_PROVIDER_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pipewire/pipewire.h>

#include <gst/gst.h>

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
  const gchar        *element;
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

  struct pw_loop *loop;
  struct pw_thread_loop *main_loop;

  struct pw_core *core;
  struct pw_type *type;
  struct pw_remote *remote;
  struct pw_core_proxy *core_proxy;
  struct pw_registry_proxy *registry;

  gboolean end;
  gboolean list_only;
  GList **devices;
  struct pw_callback_info remote_callbacks;
};

struct _GstPipeWireDeviceProviderClass {
  GstDeviceProviderClass    parent_class;
};

GType        gst_pipewire_device_provider_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_DEVICE_PROVIDER_H__ */
