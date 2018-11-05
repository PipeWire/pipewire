/* PipeWire
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

#ifndef __PIPEWIRE_PERMISSION_H__
#define __PIPEWIRE_PERMISSION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

/** \class pw_permission
 *
 * \brief a PipeWire permission
 *
 * Permissions are kept for a client and describe what the client is
 * allowed to do with an object.
 *
 * See \ref page_core_api
 */

#define PW_PERM_R	0400	/**< object can be seen and events can be received */
#define PW_PERM_W	0200	/**< methods can be called that modify the object */
#define PW_PERM_X	0100	/**< methods can be called on the object. The W flag must be
				  *  present in order to call methods that modify the object. */
#define PW_PERM_RWX	(PW_PERM_R|PW_PERM_W|PW_PERM_X)

#define PW_PERM_IS_R(p) (((p)&PW_PERM_R) == PW_PERM_R)
#define PW_PERM_IS_W(p) (((p)&PW_PERM_W) == PW_PERM_W)
#define PW_PERM_IS_X(p) (((p)&PW_PERM_X) == PW_PERM_X)

struct pw_permission {
	uint32_t id;		/**< id of object, SPA_ID_INVALID for default permission */
	uint32_t permissions;	/**< bitmask of above permissions */
};

#define PW_PERMISSION_INIT(id,p) (struct pw_permission){ (id), (p) }

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PERMISSION_H__ */
