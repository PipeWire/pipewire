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

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

/** Buffers IO area
 *
 * IO information for a port on a node. This is allocated
 * by the host and configured on all ports for which IO is requested.
 */

/** Different IO area types */
enum spa_io_type {
	SPA_IO_Buffers,
	SPA_IO_ControlRange,
	SPA_IO_Clock,
	SPA_IO_Latency,
	SPA_IO_Events,
};

struct spa_io_buffers {
#define SPA_STATUS_OK			0
#define SPA_STATUS_NEED_BUFFER		(1<<0)
#define SPA_STATUS_HAVE_BUFFER		(1<<1)
#define SPA_STATUS_FORMAT_CHANGED	(1<<2)
#define SPA_STATUS_PORT_CHANGED		(1<<3)
#define SPA_STATUS_PARAM_CHANGED	(1<<4)
	int32_t status;			/**< the status code */
	uint32_t buffer_id;		/**< a buffer id */
};

#define SPA_IO_BUFFERS_INIT  (struct spa_io_buffers) { SPA_STATUS_OK, SPA_ID_INVALID, }

/** A range, suitable for input ports that can suggest a range to output ports */
struct spa_io_control_range {
	uint64_t offset;	/**< offset in range */
	uint32_t min_size;	/**< minimum size of data */
	uint32_t max_size;	/**< maximum size of data */
};

/** A time source */
struct spa_io_clock {
	uint64_t nsec;			/**< time in nanoseconds */
	struct spa_fraction rate;	/**< rate for position/delay */
	uint64_t position;		/**< current position */
	uint64_t delay;			/**< delay between position and hardware,
					     add to position for capture,
					     subtract for playback */
};

/** latency reporting */
struct spa_io_latency {
	struct spa_fraction rate;	/**< rate for min/max */
	uint64_t min;			/**< min latency */
	uint64_t max;			/**< max latency */
};

/** event stream */
struct spa_io_events {
	struct spa_pod_sequence sequence;	/**< sequence of timed events */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_IO_H__ */
