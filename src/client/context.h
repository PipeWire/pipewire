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

#ifndef __PINOS_CONTEXT_H__
#define __PINOS_CONTEXT_H__

#include <glib-object.h>
#include <gio/gio.h>

#include <client/subscribe.h>

G_BEGIN_DECLS

#define PINOS_TYPE_CONTEXT                 (pinos_context_get_type ())
#define PINOS_IS_CONTEXT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_CONTEXT))
#define PINOS_IS_CONTEXT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_CONTEXT))
#define PINOS_CONTEXT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_CONTEXT, PinosContextClass))
#define PINOS_CONTEXT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_CONTEXT, PinosContext))
#define PINOS_CONTEXT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_CONTEXT, PinosContextClass))
#define PINOS_CONTEXT_CAST(obj)            ((PinosContext*)(obj))
#define PINOS_CONTEXT_CLASS_CAST(klass)    ((PinosContextClass*)(klass))

typedef struct _PinosContext PinosContext;
typedef struct _PinosContextClass PinosContextClass;
typedef struct _PinosContextPrivate PinosContextPrivate;

/**
 * PinosContextFlags:
 * @PINOS_CONTEXT_FLAGS_NONE: no flags
 * @PINOS_CONTEXT_FLAGS_NOAUTOSPAWN: disable autostart of the daemon
 * @PINOS_CONTEXT_FLAGS_NOFAIL: Don't fail if the daemon is not available,
 *      instead enter PINOS_CONTEXT_CONNECTING state and wait for the daemon
 *      to appear.
 *
 * Context flags passed to pinos_context_connect()
 */
typedef enum {
  PINOS_CONTEXT_FLAGS_NONE         = 0,
  PINOS_CONTEXT_FLAGS_NOAUTOSPAWN  = (1 << 0),
  PINOS_CONTEXT_FLAGS_NOFAIL       = (1 << 1)
} PinosContextFlags;

/**
 * PinosContextState:
 * @PINOS_CONTEXT_STATE_UNCONNECTED: not connected
 * @PINOS_CONTEXT_STATE_CONNECTING: connecting to daemon
 * @PINOS_CONTEXT_STATE_REGISTERING: registering with daemon
 * @PINOS_CONTEXT_STATE_READY: context is ready
 * @PINOS_CONTEXT_STATE_ERROR: context is in error
 *
 * The state of a #PinosContext
 */
typedef enum {
  PINOS_CONTEXT_STATE_ERROR        = -1,
  PINOS_CONTEXT_STATE_UNCONNECTED  = 0,
  PINOS_CONTEXT_STATE_CONNECTING   = 1,
  PINOS_CONTEXT_STATE_REGISTERING  = 2,
  PINOS_CONTEXT_STATE_READY        = 3,
} PinosContextState;

/**
 * PinosContext:
 *
 * Pinos context object class.
 */
struct _PinosContext {
  GObject object;

  PinosContextPrivate *priv;
};

/**
 * PinosContextClass:
 *
 * Pinos context object class.
 */
struct _PinosContextClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType             pinos_context_get_type              (void);

PinosContext *    pinos_context_new                   (GMainContext *ctx,
                                                       const gchar *name,
                                                       GVariant *properties);

gboolean          pinos_context_connect               (PinosContext *context, PinosContextFlags flags);
gboolean          pinos_context_disconnect            (PinosContext *context);

PinosContextState pinos_context_get_state             (PinosContext *context);
const GError *    pinos_context_get_error             (PinosContext *context);

G_END_DECLS

#endif /* __PINOS_CONTEXT_H__ */

