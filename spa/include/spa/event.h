/* Simple Plugin API
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

#ifndef __SPA_EVENT_H__
#define __SPA_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaEvent SpaEvent;

#include <spa/defs.h>
#include <spa/pod.h>

#define SPA_TYPE__Event            "Spa:Object:Event"
#define SPA_TYPE_EVENT_BASE        SPA_TYPE__Event ":"

typedef struct {
  SpaPODObjectBody body;
} SpaEventBody;

struct _SpaEvent {
  SpaPOD           pod;
  SpaEventBody body;
};

#define SPA_EVENT_TYPE(ev)   ((ev)->body.body.type)

#define SPA_EVENT_INIT(type)                            \
  { { sizeof (SpaEventBody), SPA_POD_TYPE_OBJECT },     \
    { { 0, type } } }                                   \

#define SPA_EVENT_INIT_COMPLEX(size,type,...)           \
  { { size, SPA_POD_TYPE_OBJECT },                      \
    { { 0, type }, __VA_ARGS__ } }                      \

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_EVENT_H__ */
