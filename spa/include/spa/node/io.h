/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_IO_H
#define SPA_IO_H

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_node
 * \{
 */

/** IO areas
 *
 * IO information for a port on a node. This is allocated
 * by the host and configured on a node or all ports for which
 * IO is requested.
 *
 * The plugin will communicate with the host through the IO
 * areas.
 */

/** Different IO area types */
enum spa_io_type {
	SPA_IO_Invalid,
	SPA_IO_Buffers,		/**< area to exchange buffers, struct spa_io_buffers */
	SPA_IO_Range,		/**< expected byte range, struct spa_io_range (currently not used in PipeWire) */
	SPA_IO_Clock,		/**< area to update clock information, struct spa_io_clock */
	SPA_IO_Latency,		/**< latency reporting, struct spa_io_latency (currently not used in
				  * PipeWire). \see spa_param_latency */
	SPA_IO_Control,		/**< area for control messages, struct spa_io_sequence */
	SPA_IO_Notify,		/**< area for notify messages, struct spa_io_sequence */
	SPA_IO_Position,	/**< position information in the graph, struct spa_io_position */
	SPA_IO_RateMatch,	/**< rate matching between nodes, struct spa_io_rate_match */
	SPA_IO_Memory,		/**< memory pointer, struct spa_io_memory (currently not used in PipeWire) */
	SPA_IO_AsyncBuffers,	/**< async area to exchange buffers, struct spa_io_async_buffers */
};

/**
 * IO area to exchange buffers.
 *
 * A set of buffers should first be configured on the node/port.
 * Further references to those buffers will be made by using the
 * id of the buffer.
 *
 * If status is SPA_STATUS_OK, the host should ignore
 * the io area.
 *
 * If status is SPA_STATUS_NEED_DATA, the host should:
 * 1) recycle the buffer in buffer_id, if possible
 * 2) prepare a new buffer and place the id in buffer_id.
 *
 * If status is SPA_STATUS_HAVE_DATA, the host should consume
 * the buffer in buffer_id and set the state to
 * SPA_STATUS_NEED_DATA when new data is requested.
 *
 * If status is SPA_STATUS_STOPPED, some error occurred on the
 * port.
 *
 * If status is SPA_STATUS_DRAINED, data from the io area was
 * used to drain.
 *
 * Status can also be a negative errno value to indicate errors.
 * such as:
 * -EINVAL: buffer_id is invalid
 * -EPIPE: no more buffers available
 */
struct spa_io_buffers {
#define SPA_STATUS_OK			0
#define SPA_STATUS_NEED_DATA		(1<<0)
#define SPA_STATUS_HAVE_DATA		(1<<1)
#define SPA_STATUS_STOPPED		(1<<2)
#define SPA_STATUS_DRAINED		(1<<3)
	int32_t status;			/**< the status code */
	uint32_t buffer_id;		/**< a buffer id */
};

#define SPA_IO_BUFFERS_INIT  ((struct spa_io_buffers) { SPA_STATUS_OK, SPA_ID_INVALID, })

/**
 * IO area to exchange a memory region
 */
struct spa_io_memory {
	int32_t status;			/**< the status code */
	uint32_t size;			/**< the size of \a data */
	void *data;			/**< a memory pointer */
};
#define SPA_IO_MEMORY_INIT  ((struct spa_io_memory) { SPA_STATUS_OK, 0, NULL, })

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
 *
 * Driver nodes are supposed to update the contents of \ref SPA_IO_Clock before
 * signaling the start of a graph cycle.  These updated clock values become
 * visible to other nodes in \ref SPA_IO_Position. Non-driver nodes do
 * not need to update the contents of their \ref SPA_IO_Clock. Also
 * see \ref page_driver for further details.
 *
 * The host generally gives each node a separate \ref spa_io_clock in \ref
 * SPA_IO_Clock, so that updates made by the driver are not visible in the
 * contents of \ref SPA_IO_Clock of other nodes. Instead, \ref SPA_IO_Position
 * is used to look up the current graph time.
 *
 * A node is a driver when \ref spa_io_clock::id and the ID in
 * \ref spa_io_position.clock in \ref SPA_IO_Position are the same.
 *
 * The flags are set by the graph driver at the start of each cycle.
 */
struct spa_io_clock {
#define SPA_IO_CLOCK_FLAG_FREEWHEEL	(1u<<0) /**< Graph is freewheeling. It runs at the maximum
						  *  possible rate, only constrained by the processing
						  *  power of the machine it runs on. This can be useful
						  *  for offline processing, where processing in real
						  *  time is not desired. */
#define SPA_IO_CLOCK_FLAG_XRUN_RECOVER	(1u<<1) /**< A node's process callback did not complete within
						  *  the last cycle's deadline, resulting in an xrun.
						  *  This flag is not set for the entire graph. Instead,
						  *  it is set at the start of the current cycle before
						  *  a node that experienced an xrun has its process
						  *  callback invoked. After said callback finished, the
						  *  flag is cleared again. That way, the node knows that
						  *  during the last cycle it experienced an xrun. They
						  *  can use this information for example to resynchronize
						  *  or clear custom stale states. */
#define SPA_IO_CLOCK_FLAG_LAZY		(1u<<2) /**< The driver uses lazy scheduling. For details, see
						  *  \ref PW_KEY_NODE_SUPPORTS_LAZY . */
#define SPA_IO_CLOCK_FLAG_NO_RATE	(1u<<3) /**< The rate of the clock is only approximately.
						 *   It is recommended to use the nsec as a clock source.
						 *   The rate_diff contains the measured inaccuracy. */
#define SPA_IO_CLOCK_FLAG_DISCONT	(1u<<4) /**< The clock experienced a discontinuity in its
						 *   timestamps since the last cycle. If this is set,
						 *   nodes know that timestamps between the last and
						 *   the current cycle cannot be assumed to be
						 *   continuous. Nodes that synchronize playback against
						 *   clock timestamps should resynchronize (for example
						 *   by flushing buffers to avoid incorrect delays).
						 *   This differs from an xrun in that it is not necessariy
						 *   an error and that it is not caused by missed process
						 *   deadlines. If for example a custom network time
						 *   based driver starts to follow a different time
						 *   server, and the offset between that server and its
						 *   local clock consequently suddenly changes, then that
						 *   driver should set this flag. */
	uint32_t flags;			/**< Clock flags */
	uint32_t id;			/**< Unique clock id, set by host application */
	char name[64];			/**< Clock name prefixed with API, set by node when it receives
					  *  \ref SPA_IO_Clock. The clock name is unique per clock and
					  *  can be used to check if nodes share the same clock. */
	uint64_t nsec;			/**< Time in nanoseconds against monotonic clock
					  * (CLOCK_MONOTONIC). This fields reflects a real time instant
					  * in the past, when the current cycle started. The value may
					  * have jitter. */
	struct spa_fraction rate;	/**< Rate for position/duration/delay/xrun */
	uint64_t position;		/**< Current position, in samples @ \ref rate */
	uint64_t duration;		/**< Duration of current cycle, in samples @ \ref rate */
	int64_t delay;			/**< Delay between position and hardware, in samples @ \ref rate */
	double rate_diff;		/**< Rate difference between clock and monotonic time, as a ratio of
					  *  clock speeds. A value higher than 1.0 means that the driver's
					  *  internal clock is faster than the monotonic clock (by that
					  *  factor), and vice versa. */
	uint64_t next_nsec;		/**< Estimated next wakeup time in nanoseconds.
					  *  This time is a logical start time of the next cycle, and
					  *  is not necessarily in the future.
					  */

	struct spa_fraction target_rate;	/**< Target rate of next cycle */
	uint64_t target_duration;		/**< Target duration of next cycle */
	uint32_t target_seq;			/**< Seq counter. must be equal at start and
						  *  end of read and lower bit must be 0 */
	uint32_t cycle;			/**< incremented each time the graph is started */
	uint64_t xrun;			/**< Estimated accumulated xrun duration */
};

/* the size of the video in this cycle */
struct spa_io_video_size {
#define SPA_IO_VIDEO_SIZE_VALID		(1<<0)
	uint32_t flags;			/**< optional flags */
	uint32_t stride;		/**< video stride in bytes */
	struct spa_rectangle size;	/**< the video size */
	struct spa_fraction framerate;  /**< the minimum framerate, the cycle duration is
					  *  always smaller to ensure there is only one
					  *  video frame per cycle. */
	uint32_t padding[4];
};

/**
 * Latency reporting
 *
 * Currently not used in PipeWire. Instead, \see spa_param_latency
 */
struct spa_io_latency {
	struct spa_fraction rate;	/**< rate for min/max */
	uint64_t min;			/**< min latency */
	uint64_t max;			/**< max latency */
};

/** control stream, io area for SPA_IO_Control and SPA_IO_Notify */
struct spa_io_sequence {
	struct spa_pod_sequence sequence;	/**< sequence of timed events */
};

/** bar and beat segment */
struct spa_io_segment_bar {
#define SPA_IO_SEGMENT_BAR_FLAG_VALID		(1<<0)
	uint32_t flags;			/**< extra flags */
	uint32_t offset;		/**< offset in segment of this beat */
	float signature_num;		/**< time signature numerator */
	float signature_denom;		/**< time signature denominator */
	double bpm;			/**< beats per minute */
	double beat;			/**< current beat in segment */
	double bar_start_tick;
	double ticks_per_beat;
	uint32_t padding[4];
};

/** video frame segment */
struct spa_io_segment_video {
#define SPA_IO_SEGMENT_VIDEO_FLAG_VALID		(1<<0)
#define SPA_IO_SEGMENT_VIDEO_FLAG_DROP_FRAME	(1<<1)
#define SPA_IO_SEGMENT_VIDEO_FLAG_PULL_DOWN	(1<<2)
#define SPA_IO_SEGMENT_VIDEO_FLAG_INTERLACED	(1<<3)
	uint32_t flags;			/**< flags */
	uint32_t offset;		/**< offset in segment */
	struct spa_fraction framerate;
	uint32_t hours;
	uint32_t minutes;
	uint32_t seconds;
	uint32_t frames;
	uint32_t field_count;		/**< 0 for progressive, 1 and 2 for interlaced */
	uint32_t padding[11];
};

/**
 * A segment converts a running time to a segment (stream) position.
 *
 * The segment position is valid when the current running time is between
 * start and start + duration. The position is then
 * calculated as:
 *
 *   (running time - start) * rate + position;
 *
 * Support for looping is done by specifying the LOOPING flags with a
 * non-zero duration. When the running time reaches start + duration,
 * duration is added to start and the loop repeats.
 *
 * Care has to be taken when the running time + clock.duration extends
 * past the start + duration from the segment; the user should correctly
 * wrap around and partially repeat the loop in the current cycle.
 *
 * Extra information can be placed in the segment by setting the valid flags
 * and filling up the corresponding structures.
 */
struct spa_io_segment {
	uint32_t version;
#define SPA_IO_SEGMENT_FLAG_LOOPING	(1<<0)	/**< after the duration, the segment repeats */
#define SPA_IO_SEGMENT_FLAG_NO_POSITION	(1<<1)	/**< position is invalid. The position can be invalid
						  *  after a seek, for example, when the exact mapping
						  *  of the extra segment info (bar, video, ...) to
						  *  position has not been determined yet */
	uint32_t flags;				/**< extra flags */
	uint64_t start;				/**< value of running time when this
						  *  info is active. Can be in the future for
						  *  pending changes. It does not have to be in
						  *  exact multiples of the clock duration. */
	uint64_t duration;			/**< duration when this info becomes invalid expressed
						  *  in running time. If the duration is 0, this
						  *  segment extends to the next segment. If the
						  *  segment becomes invalid and the looping flag is
						  *  set, the segment repeats. */
	double rate;				/**< overall rate of the segment, can be negative for
						  *  backwards time reporting. */
	uint64_t position;			/**< The position when the running time == start.
						  *  can be invalid when the owner of the extra segment
						  *  information has not yet made the mapping. */

	struct spa_io_segment_bar bar;
	struct spa_io_segment_video video;
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
 * It is set on all nodes in \ref SPA_IO_Position, and the contents of \ref
 * spa_io_position.clock contain the clock updates made by the driving node in
 * the graph in its \ref SPA_IO_Clock.  Also, the ID in \ref spa_io_position.clock
 * will be the clock id of the driving node in the graph.
 *
 * The position clock indicates the logical start time of the current graph
 * cycle.
 *
 * The position information contains 1 or more segments that convert the
 * raw clock times to a stream time. They are sorted based on their
 * start times, and thus the order in which they will activate in
 * the future. This makes it possible to look ahead in the scheduled
 * segments and anticipate the changes in the timeline.
 */
struct spa_io_position {
	struct spa_io_clock clock;		/**< clock position of driver, always valid and
						  *  read only */
	struct spa_io_video_size video;		/**< size of the video in the current cycle */
	int64_t offset;				/**< an offset to subtract from the clock position
						  *  to get a running time. This is the time that
						  *  the state has been in the RUNNING state and the
						  *  time that should be used to compare the segment
						  *  start values against. */
	uint32_t state;				/**< one of enum spa_io_position_state */

	uint32_t n_segments;			/**< number of segments */
	struct spa_io_segment segments[SPA_IO_POSITION_MAX_SEGMENTS];	/**< segments */
};

/**
 * Rate matching.
 *
 * It is usually set on the nodes that process resampled data, by
 * the component (audioadapter) that handles resampling between graph
 * and node rates. The \a flags and \a rate fields may be modified by the node.
 *
 * The node can request a correction to the resampling rate in its process(), by setting
 * \ref SPA_IO_RATE_MATCH_FLAG_ACTIVE on \a flags, and setting \a rate to the desired rate
 * correction.  Usually the rate is obtained from DLL or other adaptive mechanism that
 * e.g. drives the node buffer fill level toward a specific value.
 *
 * When resampling to (graph->node) direction, the number of samples produced
 * by the resampler varies on each cycle, as the rates are not commensurate.
 *
 * When resampling to (node->graph) direction, the number of samples consumed by the
 * resampler varies. Node output ports in process() should produce \a size number of
 * samples to match what the resampler needs to produce one graph quantum of output
 * samples.
 *
 * Resampling filters introduce processing delay, given by \a delay and \a delay_frac, in
 * samples at node rate. The delay varies on each cycle e.g. when resampling between
 * noncommensurate rates.
 *
 * The first sample output (graph->node) or consumed (node->graph) by the resampler is
 * offset by \a delay + \a delay_frac / 1e9 node samples relative to the nominal graph
 * cycle start position:
 *
 * \code{.unparsed}
 * first_resampled_sample_nsec =
 *	first_original_sample_nsec
 *	- (rate_match->delay * SPA_NSEC_PER_SEC + rate_match->delay_frac) / node_rate
 * \endcode
 */
struct spa_io_rate_match {
	uint32_t delay;			/**< resampling delay, in samples at
					 * node rate */
	uint32_t size;			/**< requested input size for resampler */
	double rate;			/**< rate for resampler (set by node) */
#define SPA_IO_RATE_MATCH_FLAG_ACTIVE	(1 << 0)
	uint32_t flags;			/**< extra flags (set by node) */
	int32_t delay_frac;		/**< resampling delay fractional part,
					 * in units of nanosamples (1/10^9 sample) at node rate */
	uint32_t padding[6];
};

/** async buffers */
struct spa_io_async_buffers {
	struct spa_io_buffers buffers[2];	/**< async buffers, writers write to current (cycle+1)&1,
						  *  readers read from (cycle)&1 */
};

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_IO_H */
