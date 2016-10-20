/* GStreamer
 * Copyright (C) 2012 Olivier Crete <olivier.crete@collabora.com>
 *           (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * pinosdeviceprovider.h: Device probing and monitoring
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


#ifndef __GST_PINOS_DEVICE_PROVIDER_H__
#define __GST_PINOS_DEVICE_PROVIDER_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pinos/client/pinos.h>

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstPinosDevice GstPinosDevice;
typedef struct _GstPinosDeviceClass GstPinosDeviceClass;

#define GST_TYPE_PINOS_DEVICE                 (gst_pinos_device_get_type())
#define GST_IS_PINOS_DEVICE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_PINOS_DEVICE))
#define GST_IS_PINOS_DEVICE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_PINOS_DEVICE))
#define GST_PINOS_DEVICE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_PINOS_DEVICE, GstPinosDeviceClass))
#define GST_PINOS_DEVICE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_PINOS_DEVICE, GstPinosDevice))
#define GST_PINOS_DEVICE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_DEVICE, GstPinosDeviceClass))
#define GST_PINOS_DEVICE_CAST(obj)            ((GstPinosDevice *)(obj))

typedef enum {
  GST_PINOS_DEVICE_TYPE_SOURCE,
  GST_PINOS_DEVICE_TYPE_SINK,
} GstPinosDeviceType;

struct _GstPinosDevice {
  GstDevice           parent;

  GstPinosDeviceType  type;
  gpointer            id;
  gchar              *path;
  const gchar        *element;
};

struct _GstPinosDeviceClass {
  GstDeviceClass    parent_class;
};

GType        gst_pinos_device_get_type (void);

typedef struct _GstPinosDeviceProvider GstPinosDeviceProvider;
typedef struct _GstPinosDeviceProviderClass GstPinosDeviceProviderClass;

#define GST_TYPE_PINOS_DEVICE_PROVIDER                 (gst_pinos_device_provider_get_type())
#define GST_IS_PINOS_DEVICE_PROVIDER(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_PINOS_DEVICE_PROVIDER))
#define GST_IS_PINOS_DEVICE_PROVIDER_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_PINOS_DEVICE_PROVIDER))
#define GST_PINOS_DEVICE_PROVIDER_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_PINOS_DEVICE_PROVIDER, GstPinosDeviceProviderClass))
#define GST_PINOS_DEVICE_PROVIDER(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_PINOS_DEVICE_PROVIDER, GstPinosDeviceProvider))
#define GST_PINOS_DEVICE_PROVIDER_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_DEVICE_PROVIDER, GstPinosDeviceProviderClass))
#define GST_PINOS_DEVICE_PROVIDER_CAST(obj)            ((GstPinosDeviceProvider *)(obj))

struct _GstPinosDeviceProvider {
  GstDeviceProvider         parent;

  gchar *client_name;

  GMainContext *maincontext;
  PinosThreadMainLoop *loop;

  PinosContext *context;
};

struct _GstPinosDeviceProviderClass {
  GstDeviceProviderClass    parent_class;
};

GType        gst_pinos_device_provider_get_type (void);

G_END_DECLS

#endif /* __GST_PINOS_DEVICE_PROVIDER_H__ */
