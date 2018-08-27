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

#include <spa/pod/pod.h>

struct spa_event_body {
	struct spa_pod_object_body body;
};

struct spa_event {
	struct spa_pod pod;
	struct spa_event_body body;
};

#define SPA_EVENT_TYPE(ev)	((ev)->body.body.type)
#define SPA_EVENT_ID(ev,type)	(SPA_EVENT_TYPE(ev) == type ? \
					(ev)->body.body.id : SPA_ID_INVALID)

#define SPA_EVENT_INIT(type,id) (struct spa_event)			\
	{ { sizeof(struct spa_event_body), SPA_TYPE_Object },		\
	  { { type, id } } }						\

#define SPA_EVENT_INIT_FULL(t,size,type,id,...) (t)			\
	{ { size, SPA_TYPE_OBJECT },					\
	  { { type, id }, ##__VA_ARGS__ } }				\

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_EVENT_H__ */
