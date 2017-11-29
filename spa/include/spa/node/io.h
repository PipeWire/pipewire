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

#ifndef __SPA_IO_H__
#define __SPA_IO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/type-map.h>

/** Base for IO structures to interface with node ports */
#define SPA_TYPE__IO			SPA_TYPE_POINTER_BASE "IO"
#define SPA_TYPE_IO_BASE		SPA_TYPE__IO ":"

/** Base for control structures */
#define SPA_TYPE_IO__Control		SPA_TYPE_IO_BASE "Control"
#define SPA_TYPE_IO_CONTROL_BASE	SPA_TYPE_IO__Control ":"

/** Base for controlable properties */
#define SPA_TYPE_IO__Prop		SPA_TYPE_IO_BASE "Prop"
#define SPA_TYPE_IO_PROP_BASE		SPA_TYPE_IO__Prop ":"

/** An io area to exchange buffers with a port */
#define SPA_TYPE_IO__Buffers		SPA_TYPE_IO_BASE "Buffers"

/** Buffers IO area
 *
 * IO information for a port on a node. This is allocated
 * by the host and configured on all ports for which IO is requested.
 */
struct spa_io_buffers {
#define SPA_STATUS_OK			0
#define SPA_STATUS_NEED_BUFFER		1
#define SPA_STATUS_HAVE_BUFFER		2
#define SPA_STATUS_FORMAT_CHANGED	3
#define SPA_STATUS_PORTS_CHANGED	4
#define SPA_STATUS_PARAM_CHANGED	5
	int32_t status;			/**< the status code */
	uint32_t buffer_id;		/**< a buffer id */
};

#define SPA_IO_BUFFERS_INIT  (struct spa_io_buffers) { SPA_STATUS_OK, SPA_ID_INVALID, }

/** Information about requested range */
#define SPA_TYPE_IO_CONTROL__Range	SPA_TYPE_IO_CONTROL_BASE "Range"

/** A range, suitable for input ports that can suggest a range to output ports */
struct spa_io_control_range {
	uint64_t offset;	/**< offset in range */
	uint32_t min_size;	/**< minimum size of data */
	uint32_t max_size;	/**< maximum size of data */
};

struct spa_type_io {
	uint32_t Buffers;
	uint32_t ControlRange;
	uint32_t Prop;
};

static inline void spa_type_io_map(struct spa_type_map *map, struct spa_type_io *type)
{
	if (type->Buffers == 0) {
		type->Buffers = spa_type_map_get_id(map, SPA_TYPE_IO__Buffers);
		type->ControlRange = spa_type_map_get_id(map, SPA_TYPE_IO_CONTROL__Range);
		type->Prop = spa_type_map_get_id(map, SPA_TYPE_IO__Prop);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_IO_H__ */
