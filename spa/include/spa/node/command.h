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

#ifndef __SPA_COMMAND_NODE_H__
#define __SPA_COMMAND_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/pod/command.h>

/* object id of SPA_TYPE_COMMAND_Node */
enum spa_node_command {
	SPA_NODE_COMMAND_Suspend,
	SPA_NODE_COMMAND_Pause,
	SPA_NODE_COMMAND_Start,
	SPA_NODE_COMMAND_Enable,
	SPA_NODE_COMMAND_Disable,
	SPA_NODE_COMMAND_Flush,
	SPA_NODE_COMMAND_Drain,
	SPA_NODE_COMMAND_Marker,
};

#define SPA_NODE_COMMAND_ID(cmd)	SPA_COMMAND_ID(cmd, SPA_TYPE_COMMAND_Node)

#define SPA_NODE_COMMAND_INIT(id) (struct spa_command)			\
        { { sizeof(struct spa_command_body), SPA_TYPE_Object },		\
          { { SPA_TYPE_COMMAND_Node, id } } }				\

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* _SPA_COMMAND_NODE_H__ */
