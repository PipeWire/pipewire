/* Pinos
 * Copyright (C) 2016 Axis Communications AB
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

#ifndef __PINOS_SPA_VIDEOTESTSRC_H__
#define __PINOS_SPA_VIDEOTESTSRC_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/node.h>

typedef struct _PinosSpaVideoTestSrc PinosSpaVideoTestSrc;

G_BEGIN_DECLS

struct _PinosSpaVideoTestSrc {
  PinosNode *node;
};

PinosSpaVideoTestSrc * pinos_spa_videotestsrc_new      (PinosCore       *core,
                                                        const gchar     *name,
                                                        PinosProperties *properties);
void                   pinos_spa_videotestsrc_destroy  (PinosSpaVideoTestSrc *src);

G_END_DECLS

#endif /* __PINOS_SPA_VIDEOTESTSRC_H__ */
