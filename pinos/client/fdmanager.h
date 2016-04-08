/* GStreamer
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PINOS_FD_MANAGER_H__
#define __PINOS_FD_MANAGER_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosFdManager PinosFdManager;
typedef struct _PinosFdManagerClass PinosFdManagerClass;
typedef struct _PinosFdManagerPrivate PinosFdManagerPrivate;

#define PINOS_TYPE_FD_MANAGER          (pinos_fd_manager_get_type())
#define PINOS_FD_MANAGER(obj)          (G_TYPE_CHECK_INSTANCE_CAST((obj),PINOS_TYPE_FD_MANAGER,PinosFdManager))
#define PINOS_FD_MANAGER_CLASS(klass)  (G_TYPE_CHECK_CLASS_CAST((klass),PINOS_TYPE_FD_MANAGER,PinosFdManagerClass))
#define PINOS_IS_FD_MANAGER(obj)       (G_TYPE_CHECK_INSTANCE_TYPE((obj),PINOS_TYPE_FD_MANAGER))
#define PINOS_IS_FD_MANAGER_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass),PINOS_TYPE_FD_MANAGER))

/**
 * PinosFdManager:
 *
 * Object to manager fds
 */
struct _PinosFdManager
{
  GObject    parent;

  PinosFdManagerPrivate *priv;
};

struct _PinosFdManagerClass
{
  GObjectClass parent_class;
};

GType pinos_fd_manager_get_type (void);

#define PINOS_FD_MANAGER_DEFAULT    "default"

PinosFdManager *      pinos_fd_manager_get           (const gchar *type);

guint32               pinos_fd_manager_get_id        (PinosFdManager *manager);

gboolean              pinos_fd_manager_add           (PinosFdManager *manager,
                                                      const gchar *client,
                                                      guint32 id,
                                                      gpointer object,
                                                      GDestroyNotify notify);
gboolean              pinos_fd_manager_remove        (PinosFdManager *manager,
                                                      const gchar *client,
                                                      guint32 id);
gboolean              pinos_fd_manager_remove_all    (PinosFdManager *manager,
                                                      const gchar *client);


G_END_DECLS

#endif /* __PINOS_FD_MANAGER_H__ */
