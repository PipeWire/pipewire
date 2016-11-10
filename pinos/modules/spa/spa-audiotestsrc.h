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

#ifndef __PINOS_SPA_AUDIOTESTSRC_H__
#define __PINOS_SPA_AUDIOTESTSRC_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/node.h>

G_BEGIN_DECLS

PinosNode *       pinos_spa_audiotestsrc_new      (PinosCore       *core,
                                                   const gchar     *name,
                                                   PinosProperties *properties);

G_END_DECLS

#endif /* __PINOS_SPA_AUDIOTESTSRC_H__ */
