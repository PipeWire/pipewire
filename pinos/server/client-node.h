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

#ifndef __PINOS_CLIENT_NODE_H__
#define __PINOS_CLIENT_NODE_H__

#include <glib-object.h>

#include <pinos/server/node.h>

G_BEGIN_DECLS

#define PINOS_CLIENT_NODE_URI                            "http://pinos.org/ns/client-node"
#define PINOS_CLIENT_NODE_PREFIX                         PINOS_CLIENT_NODE_URI "#"

typedef struct _PinosClientNode PinosClientNode;

/**
 * PinosClientNode:
 *
 * Pinos client node interface
 */
struct _PinosClientNode {
  PinosNode *node;

  int  (*get_ctrl_socket)      (PinosClientNode  *node,
                                GError          **error);
  int  (*get_data_socket)      (PinosClientNode  *node,
                                GError          **error);
};

PinosObject *      pinos_client_node_new                  (PinosCore       *core,
                                                           PinosClient     *client,
                                                           const gchar     *name,
                                                           PinosProperties *properties);

#define pinos_client_node_get_ctrl_socket(n)   (n)->get_ctrl_socket(n,__VA_ARGS__)
#define pinos_client_node_get_data_socket(n)   (n)->get_data_socket(n,__VA_ARGS__)

G_END_DECLS

#endif /* __PINOS_CLIENT_NODE_H__ */
