/* Pulsevideo
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

#ifndef __PV_CONTEXT_H__
#define __PV_CONTEXT_H__

#include <glib-object.h>
#include <gio/gio.h>

#include <client/pv-source.h>
#include <client/pv-subscribe.h>

G_BEGIN_DECLS

#define PV_TYPE_CONTEXT                 (pv_context_get_type ())
#define PV_IS_CONTEXT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_CONTEXT))
#define PV_IS_CONTEXT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_CONTEXT))
#define PV_CONTEXT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_CONTEXT, PvContextClass))
#define PV_CONTEXT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_CONTEXT, PvContext))
#define PV_CONTEXT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_CONTEXT, PvContextClass))
#define PV_CONTEXT_CAST(obj)            ((PvContext*)(obj))
#define PV_CONTEXT_CLASS_CAST(klass)    ((PvContextClass*)(klass))

typedef struct _PvContext PvContext;
typedef struct _PvContextClass PvContextClass;
typedef struct _PvContextPrivate PvContextPrivate;

/**
 * PvContextFlags:
 * @PV_CONTEXT_FLAGS_NONE: no flags
 * @PV_CONTEXT_FLAGS_NOAUTOSPAWN: disable autostart of the daemon
 * @PV_CONTEXT_FLAGS_NOFAIL: Don't fail if the daemon is not available,
 *      instead enter PV_CONTEXT_CONNECTING state and wait for the daemon
 *      to appear.
 *
 * Context flags passed to pv_context_connect()
 */
typedef enum {
  PV_CONTEXT_FLAGS_NONE         = 0,
  PV_CONTEXT_FLAGS_NOAUTOSPAWN  = (1 << 0),
  PV_CONTEXT_FLAGS_NOFAIL       = (1 << 1)
} PvContextFlags;

/**
 * PvContextState:
 * @PV_CONTEXT_STATE_UNCONNECTED: not connected
 * @PV_CONTEXT_STATE_CONNECTING: connecting to daemon
 * @PV_CONTEXT_STATE_REGISTERING: registering with daemon
 * @PV_CONTEXT_STATE_READY: context is ready
 * @PV_CONTEXT_STATE_ERROR: context is in error
 *
 * The state of a #PvContext
 */
typedef enum {
  PV_CONTEXT_STATE_UNCONNECTED  = 0,
  PV_CONTEXT_STATE_CONNECTING   = 1,
  PV_CONTEXT_STATE_REGISTERING  = 2,
  PV_CONTEXT_STATE_READY        = 3,
  PV_CONTEXT_STATE_ERROR        = 4
} PvContextState;

/**
 * PvContext:
 *
 * Pulsevideo context object class.
 */
struct _PvContext {
  GObject object;

  PvContextPrivate *priv;
};

/**
 * PvContextClass:
 *
 * Pulsevideo context object class.
 */
struct _PvContextClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType             pv_context_get_type              (void);

PvContext *       pv_context_new                   (const gchar *name, GVariant *properties);

gboolean          pv_context_set_subscribe         (PvContext *context, PvSubscribe *subscribe);

gboolean          pv_context_connect               (PvContext *context, PvContextFlags flags);

gboolean          pv_context_register_source       (PvContext *context, PvSource *source);
gboolean          pv_context_unregister_source     (PvContext *context, PvSource *source);

GDBusConnection * pv_context_get_connection        (PvContext *context);
GDBusProxy *      pv_context_get_client_proxy      (PvContext *context);
const gchar *     pv_context_get_client_path       (PvContext *context);

PvContextState    pv_context_get_state             (PvContext *context);

G_END_DECLS

#endif /* __PV_CONTEXT_H__ */

