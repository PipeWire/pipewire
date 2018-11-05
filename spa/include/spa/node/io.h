/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __SPA_IO_H__
#define __SPA_IO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

/** IO areas
 *
 * IO information for a port on a node. This is allocated
 * by the host and configured on a node or all ports for which
 * IO is requested.
 */

/** Different IO area types */
enum spa_io_type {
	SPA_IO_Invalid,
	SPA_IO_Buffers,		/**< area to exchange buffers */
	SPA_IO_Range,		/**< expected byte range */
	SPA_IO_Clock,		/**< area to update clock information */
	SPA_IO_Latency,		/**< latency reporting */
	SPA_IO_Control,		/**< area for control messages */
	SPA_IO_Notify,		/**< area for notify messages */
	SPA_IO_Position,	/**< position information in the graph */
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
struct spa_io_range {
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

/** control stream */
struct spa_io_sequence {
	struct spa_pod_sequence sequence;	/**< sequence of timed events */
};

/** bar and beat position */
struct spa_io_position_bar {
	uint32_t size;			/**< size of this structure */
	uint32_t offset;		/**< offset of last bar in samples against current cycle */
	struct spa_fraction signature;	/**< time signature */
	double bpm;			/**< beats per minute */
	double bar;			/**< current bar in quarter notes */
	double last_bar;		/**< position of last bar in quarter notes */
	double cycle_start;		/**< cycle start in quarter notes */
	double cycle_end;		/**< cycle end in quarter notes */
	uint32_t padding[16];
};

/** video frame position */
struct spa_io_position_video {
	uint32_t size;			/**< size of this structure */
	uint32_t offset;		/**< offset of frame against current cycle */
	struct spa_fraction framerate;
#define SPA_IO_POSITION_VIDEO_FLAG_DROP_FRAME	(1<<0)
#define SPA_IO_POSITION_VIDEO_FLAG_PULL_DOWN	(1<<1)
#define SPA_IO_POSITION_VIDEO_FLAG_INTERLACED	(1<<2)
	uint32_t flags;			/**< flags */
	uint32_t hours;
	uint32_t minutes;
	uint32_t seconds;
	uint32_t frames;
	uint32_t field_count;		/**< 0 for progressive, 1 and 2 for interlaced */
	uint32_t padding[16];
};

/** position reporting */
struct spa_io_position {
	struct spa_io_clock clock;		/**< clock position of driver, always valid and
						  *  read only */
	uint32_t size;				/**< size of current cycle expressed in clock.rate */
	struct spa_fraction rate;		/**< overal rate of the graph */
#define SPA_IO_POSITION_FLAG_BAR	(1<<0)
#define SPA_IO_POSITION_FLAG_VIDEO	(1<<1)
	uint64_t flags;				/**< flags indicate what fields are valid */
	struct spa_io_position_bar bar;		/**< when mask & SPA_IO_POSITION_FLAG_BAR*/
	struct spa_io_position_video video;	/**< when mask & SPA_IO_POSITION_FLAG_VIDEO */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_IO_H__ */
