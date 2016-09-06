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

#ifndef __PINOS_LINK_H__
#define __PINOS_LINK_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosLink PinosLink;
typedef struct _PinosLinkClass PinosLinkClass;
typedef struct _PinosLinkPrivate PinosLinkPrivate;

#include <pinos/server/daemon.h>

#define PINOS_TYPE_LINK             (pinos_link_get_type ())
#define PINOS_IS_LINK(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_LINK))
#define PINOS_IS_LINK_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_LINK))
#define PINOS_LINK_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_LINK, PinosLinkClass))
#define PINOS_LINK(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_LINK, PinosLink))
#define PINOS_LINK_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_LINK, PinosLinkClass))
#define PINOS_LINK_CAST(obj)        ((PinosLink*)(obj))
#define PINOS_LINK_CLASS_CAST(klass)((PinosLinkClass*)(klass))

/**
 * PinosLink:
 *
 * Pinos link object class.
 */
struct _PinosLink {
  GObject object;

  PinosNode *output_node;
  guint      output_port;
  PinosNode *input_node;
  guint      input_port;

  PinosLinkPrivate *priv;
};

/**
 * PinosLinkClass:
 *
 * Pinos link object class.
 */
struct _PinosLinkClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType               pinos_link_get_type             (void);

void                pinos_link_remove               (PinosLink *link);

PinosProperties *   pinos_link_get_properties       (PinosLink *link);

const gchar *       pinos_link_get_object_path      (PinosLink *link);

G_END_DECLS

#endif /* __PINOS_LINK_H__ */
