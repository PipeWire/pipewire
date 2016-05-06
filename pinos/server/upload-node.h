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

#ifndef __PINOS_UPLOAD_NODE_H__
#define __PINOS_UPLOAD_NODE_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosUploadNode PinosUploadNode;
typedef struct _PinosUploadNodeClass PinosUploadNodeClass;
typedef struct _PinosUploadNodePrivate PinosUploadNodePrivate;

#include <pinos/server/node.h>

#define PINOS_TYPE_UPLOAD_NODE                 (pinos_upload_node_get_type ())
#define PINOS_IS_UPLOAD_NODE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_UPLOAD_NODE))
#define PINOS_IS_UPLOAD_NODE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_UPLOAD_NODE))
#define PINOS_UPLOAD_NODE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_UPLOAD_NODE, PinosUploadNodeClass))
#define PINOS_UPLOAD_NODE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_UPLOAD_NODE, PinosUploadNode))
#define PINOS_UPLOAD_NODE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_UPLOAD_NODE, PinosUploadNodeClass))
#define PINOS_UPLOAD_NODE_CAST(obj)            ((PinosUploadNode*)(obj))
#define PINOS_UPLOAD_NODE_CLASS_CAST(klass)    ((PinosUploadNodeClass*)(klass))

/**
 * PinosUploadNode:
 *
 * Pinos client source object class.
 */
struct _PinosUploadNode {
  PinosNode object;

  PinosUploadNodePrivate *priv;
};

/**
 * PinosUploadNodeClass:
 *
 * Pinos client source object class.
 */
struct _PinosUploadNodeClass {
  PinosNodeClass parent_class;
};

/* normal GObject stuff */
GType               pinos_upload_node_get_type         (void);

PinosNode *         pinos_upload_node_new              (PinosDaemon *daemon,
                                                        GBytes      *possible_formats);

PinosChannel *      pinos_upload_node_get_channel      (PinosUploadNode   *source,
                                                        const gchar       *client_path,
                                                        GBytes            *format_filter,
                                                        PinosProperties   *props,
                                                        GError            **error);
G_END_DECLS

#endif /* __PINOS_UPLOAD_NODE_H__ */
