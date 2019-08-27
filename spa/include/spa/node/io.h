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

#ifndef SPA_IO_H
#define SPA_IO_H

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
	SPA_IO_RateMatch,	/**< rate matching between nodes */
};

struct spa_io_buffers {
#define SPA_STATUS_OK			0
#define SPA_STATUS_NEED_BUFFER		(1<<0)
#define SPA_STATUS_HAVE_BUFFER		(1<<1)
#define SPA_STATUS_STOPPED		(1<<2)
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

/**
 * Absolute time reporting.
 *
 * Nodes that can report clocking information will receive this io block.
 * The application sets the id. This is usually set as part of the
 * position information but can also be set separately.
 *
 * The clock counts the elapsed time according to the clock provider
 * since the provider was last started.
 */
struct spa_io_clock {
	uint32_t id;			/**< unique clock id, set by application */
	uint32_t flags;			/**< clock flags */
	uint64_t nsec;			/**< time in nanoseconds */
	uint64_t count;			/**< a media specific counter. Can be used to detect
					  *  gaps in the media. It usually represents the amount
					  *  of processed media units (packets, frames,
					  *  samples, ...) */
	struct spa_fraction rate;	/**< rate for position/duration/delay */
	uint64_t position;		/**< current position */
	uint64_t duration;		/**< duration of current cycle */
	int64_t delay;			/**< delay between position and hardware,
					  *  positive for capture, negative for playback */
	double rate_diff;		/**< rate difference between clock and monotonic time */
	uint64_t next_nsec;		/**< extimated next wakup time in nanoseconds */
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

/** bar and beat segment */
struct spa_io_segment_bar {
	uint32_t offset;		/**< offset in samples of this beat */
	float signature_num;		/**< time signature numerator */
	float signature_denom;		/**< time signature denominator */
	double bpm;			/**< beats per minute */
	double beat;			/**< current beat in segment */
	uint32_t padding[16];
};

/** video frame segment */
struct spa_io_segment_video {
	uint32_t offset;		/**< offset of frame against current segment */
	struct spa_fraction framerate;
#define SPA_IO_SEGMENT_VIDEO_FLAG_DROP_FRAME	(1<<0)
#define SPA_IO_SEGMENT_VIDEO_FLAG_PULL_DOWN	(1<<1)
#define SPA_IO_SEGMENT_VIDEO_FLAG_INTERLACED	(1<<2)
	uint32_t flags;			/**< flags */
	uint32_t hours;
	uint32_t minutes;
	uint32_t seconds;
	uint32_t frames;
	uint32_t field_count;		/**< 0 for progressive, 1 and 2 for interlaced */
	uint32_t padding[16];
};

/**
 * A segment converts a raw clock time to a segment (stream) position.
 *
 * The segment position is valid when the current clock position is between
 * clock_start and clock_start + clock_duration. The position is then
 * calculated as:
 *
 *   (clock_start - clock.position) * rate + position;
 *
 * Support for looping is done by specifying a non-zero duration. When the
 * clock reaches clock_start + clock_duration, clock_duration is added to
 * clock_start and the loop repeats.
 *
 * Care has to be taken when the clock.duration extends past the
 * clock_start + clock_duration from the segment; the user should correctly
 * wrap around and partially repeat the loop in the current cycle.
 *
 * Extra information can be placed in the segment by setting the valid flags
 * and filling up the corresponding structures.
 */
struct spa_io_segment {
#define SPA_IO_SEGMENT_FLAG_LOOPING	(1<<0)	/**< after the duration, the segment repeats */
	uint32_t flags;				/**< extra flags */
#define SPA_IO_SEGMENT_VALID_POSITION	(1<<0)
#define SPA_IO_SEGMENT_VALID_BAR	(1<<1)
#define SPA_IO_SEGMENT_VALID_VIDEO	(1<<2)
	uint32_t valid;				/**< indicates what fields are valid below */
	uint64_t clock_start;			/**< position against clock position when this
						  *  info is active. Can be in the future for
						  *  pending changes. It does not have to be in
						  *  exact multiples of the clock duration. */
	uint64_t clock_duration;		/**< duration when this info becomes invalid. If
						  *  the duration is 0, this segment extends to the
						  *  next segment. If the segment becomes invalid and
						  *  the looping flag is set, the segment is repeats. */
	uint64_t position;			/**< The position when the clock == clock_start. */
	double rate;				/**< overal rate of the graph, can be negative for
						  *  backwards time reporting. */

	struct spa_io_segment_bar bar;		/**< when valid & SPA_IO_SEGMENT_VALID_BAR */
	struct spa_io_segment_video video;	/**< when valid & SPA_IO_SEGMENT_VALID_VIDEO */
};

enum spa_io_position_state {
	SPA_IO_POSITION_STATE_STOPPED,
	SPA_IO_POSITION_STATE_STARTING,
	SPA_IO_POSITION_STATE_RUNNING,
};

/** the maximum number of segments visible in the future */
#define SPA_IO_POSITION_MAX_SEGMENTS	8

/**
 * The position information adds extra meaning to the raw clock times.
 *
 * It is set on all nodes and the clock id will contain the clock of the
 * master node in the graph.
 *
 * The position information contains 1 or more segments that convert the
 * raw clock times to a stream time. They are sorted based on their
 * clock_start times, and thus the order in which they will activate in
 * the future. This makes it possible to look ahead in the scheduled
 * segments and anticipate the changes in the timeline.
 */
struct spa_io_position {
	struct spa_io_clock clock;		/**< clock position of driver, always valid and
						  *  read only */
	uint32_t state;				/**< one of enum spa_io_position_state */

	uint32_t n_segments;			/**< number of segments */
	struct spa_io_segment segments[SPA_IO_POSITION_MAX_SEGMENTS];	/**< segments */
};

/** rate matching */
struct spa_io_rate_match {
	uint32_t delay;			/**< extra delay in samples for resampler */
	uint32_t size;			/**< requested input size for resampler */
	double rate;			/**< rate for resampler */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_IO_H */
