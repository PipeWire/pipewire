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

#ifndef __SPA_COMMAND_H__
#define __SPA_COMMAND_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

struct spa_command_body {
	struct spa_pod_object_body body;
};

struct spa_command {
	struct spa_pod		pod;
	struct spa_command_body body;
};

#define SPA_COMMAND_TYPE(cmd)		((cmd)->body.body.type)
#define SPA_COMMAND_ID(cmd,type)	(SPA_COMMAND_TYPE(cmd) == type ? \
						(cmd)->body.body.id : SPA_ID_INVALID)

#define SPA_COMMAND_INIT(type,id) (struct spa_command)			\
	{ { sizeof(struct spa_command_body), SPA_TYPE_Object },		\
	  { { type, id } } }						\

#define SPA_COMMAND_INIT_FULL(t,size,type,id,...) (t)			\
	{ { size, SPA_TYPE_OBJECT },					\
	  { { type, id }, ##__VA_ARGS__ } }				\

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_COMMAND_H__ */
