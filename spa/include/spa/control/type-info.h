/* Simple Plugin API
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_CONTROL_TYPES_H__
#define __SPA_CONTROL_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/type-info.h>
#include <spa/control/control.h>

/* base for parameter object enumerations */
#define SPA_TYPE__Control		SPA_TYPE_ENUM_BASE "Control"
#define SPA_TYPE_CONTROL_BASE		SPA_TYPE__Control ":"

static const struct spa_type_info spa_type_control[] = {
	{ SPA_CONTROL_Invalid, SPA_TYPE_CONTROL_BASE "Invalid", SPA_TYPE_Int, },
	{ SPA_CONTROL_Properties, SPA_TYPE_CONTROL_BASE "Properties", SPA_TYPE_Int, },
	{ SPA_CONTROL_Midi, SPA_TYPE_CONTROL_BASE "Midi", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_CONTROL_TYPES_H__ */
