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

typedef uint16_t jack_int_t;  // Internal type for ports and refnum

typedef enum {
	NotTriggered,
	Triggered,
	Running,
	Finished,
} jack_client_state_t;

PRE_PACKED_STRUCTURE
struct jack_client_timing {
	jack_time_t signaled_at;
	jack_time_t awake_at;
	jack_time_t finished_at;
	jack_client_state_t status;
} POST_PACKED_STRUCTURE;

#define JACK_CLIENT_TIMING_INIT (struct jack_client_timing) { 0, 0, 0, NotTriggered }

PRE_PACKED_STRUCTURE
struct jack_port {
	int type_id;
	enum JackPortFlags flags;
	char name[REAL_JACK_PORT_NAME_SIZE];
	char alias1[REAL_JACK_PORT_NAME_SIZE];
	char alias2[REAL_JACK_PORT_NAME_SIZE];
	int ref_num;

	jack_nframes_t latency;
	jack_nframes_t total_latency;
	jack_latency_range_t playback_latency;
	jack_latency_range_t capture_latency;
	uint8_t monitor_requests;

	bool in_use;
	jack_port_id_t tied;
	jack_default_audio_sample_t buffer[BUFFER_SIZE_MAX + 8];
} POST_PACKED_STRUCTURE;

#define MAKE_FIXED_ARRAY(size)		\
PRE_PACKED_STRUCTURE			\
struct {				\
	jack_int_t table[size];		\
        uint32_t counter;		\
} POST_PACKED_STRUCTURE

#define MAKE_FIXED_ARRAY1(size)		\
PRE_PACKED_STRUCTURE			\
struct {				\
	MAKE_FIXED_ARRAY(size) array;	\
        bool used;		\
} POST_PACKED_STRUCTURE

#define MAKE_FIXED_MATRIX(size)		\
PRE_PACKED_STRUCTURE			\
struct {				\
	jack_int_t table[size][size];	\
} POST_PACKED_STRUCTURE

PRE_PACKED_STRUCTURE
struct jack_activation_count {
	int32_t value;
	int32_t count;
} POST_PACKED_STRUCTURE;

#define MAKE_LOOP_FEEDBACK(size)	\
PRE_PACKED_STRUCTURE			\
struct {				\
	int table[size][3];		\
} POST_PACKED_STRUCTURE

PRE_PACKED_STRUCTURE
struct jack_connection_manager {
	MAKE_FIXED_ARRAY(CONNECTION_NUM_FOR_PORT) connections[PORT_NUM_MAX];
	MAKE_FIXED_ARRAY1(PORT_NUM_FOR_CLIENT) input_port[CLIENT_NUM];
	MAKE_FIXED_ARRAY(PORT_NUM_FOR_CLIENT) output_port[CLIENT_NUM];
	MAKE_FIXED_MATRIX(CLIENT_NUM) connection_ref;
	struct jack_activation_count input_counter[CLIENT_NUM];
	MAKE_LOOP_FEEDBACK(CONNECTION_NUM_FOR_PORT) loop_feedback;
} POST_PACKED_STRUCTURE;

PRE_PACKED_STRUCTURE
struct jack_atomic_counter {
	union {
		struct {
			uint16_t short_val1;  // Cur
			uint16_t short_val2;  // Next
		} scounter;
		uint32_t long_val;
	} info;
} POST_PACKED_STRUCTURE;

#define MAKE_ATOMIC_STATE(type)				\
PRE_PACKED_STRUCTURE					\
struct {						\
	type state[2];					\
	volatile struct jack_atomic_counter counter;	\
        int32_t call_write_counter;			\
} POST_PACKED_STRUCTURE

PRE_PACKED_STRUCTURE
struct jack_atomic_array_counter {
	union {
		struct {
			unsigned char byte_val[4];
		} scounter;
		uint32_t long_val;
	} info;
} POST_PACKED_STRUCTURE;

#define MAKE_ATOMIC_ARRAY_STATE(type)				\
PRE_PACKED_STRUCTURE						\
struct {							\
	type state[3];						\
	volatile struct jack_atomic_array_counter counter;	\
} POST_PACKED_STRUCTURE

PRE_PACKED_STRUCTURE
struct jack_graph_manager {
	jack_shm_info_t info;
	MAKE_ATOMIC_STATE(struct jack_connection_manager) state;
	unsigned int port_max;
        struct jack_client_timing client_timing[CLIENT_NUM];
        struct jack_port port_array[0];
} POST_PACKED_STRUCTURE;

typedef enum {
    TransportCommandNone = 0,
    TransportCommandStart = 1,
    TransportCommandStop = 2,
} transport_command_t;

PRE_PACKED_STRUCTURE
struct jack_transport_engine {
	MAKE_ATOMIC_ARRAY_STATE(jack_position_t) state;
	jack_transport_state_t transport_state;
        volatile transport_command_t transport_cmd;
        transport_command_t previous_cmd;               /* previous transport_cmd */
        jack_time_t sync_timeout;
        int sync_time_left;
        int time_base_master;
        bool pending_pos;
        bool network_sync;
        bool conditionnal;
        int32_t write_counter;
} POST_PACKED_STRUCTURE;

PRE_PACKED_STRUCTURE
struct jack_timer {
	jack_nframes_t frames;
	jack_time_t current_wakeup;
	jack_time_t current_callback;
	jack_time_t next_wakeup;
	float period_usecs;
	float filter_omega; /* set once, never altered */
	bool initialized;
} POST_PACKED_STRUCTURE;

PRE_PACKED_STRUCTURE
struct jack_frame_timer {
	MAKE_ATOMIC_STATE(struct jack_timer) state;
        bool first_wakeup;
} POST_PACKED_STRUCTURE;

#ifdef JACK_MONITOR
PRE_PACKED_STRUCTURE
struct jack_timing_measure_client {
	int ref_num;
	jack_time_t signaled_at;
	jack_time_t awake_at;
	jack_time_t finished_at;
	jack_client_state_t status;
} POST_PACKED_STRUCTURE;

PRE_PACKED_STRUCTURE
struct jack_timing_client_interval {
	int ref_num;
	char name[JACK_CLIENT_NAME_SIZE+1];
	int begin_interval;
	int end_interval;
} POST_PACKED_STRUCTURE;

PRE_PACKED_STRUCTURE
struct jack_timing_measure {
	unsigned int audio_cycle;
	jack_time_t period_usecs;
	jack_time_t cur_cycle_begin;
	jack_time_t prev_cycle_end;
	struct jack_timing_measure_client client_table[CLIENT_NUM];
} POST_PACKED_STRUCTURE;

PRE_PACKED_STRUCTURE
struct jack_engine_profiling {
	struct jack_timing_measure profile_table[TIME_POINTS];
	struct jack_timing_client_interval interval_table[MEASURED_CLIENTS];

	unsigned int audio_cycle;
	unsigned int measured_client;
} POST_PACKED_STRUCTURE;
#endif

PRE_PACKED_STRUCTURE
struct jack_engine_control {
	jack_shm_info_t info;
	jack_nframes_t buffer_size;
	jack_nframes_t sample_rate;
	bool sync_node;
	bool temporary;
	jack_time_t period_usecs;
	jack_time_t timeout_usecs;
	float max_delayed_usecs;
	float xrun_delayed_usecs;
	bool timeout;
	bool real_time;
	bool saved_real_time;
	int server_priority;
	int client_priority;
	int max_client_priority;
	char server_name[JACK_SERVER_NAME_SIZE];
	struct jack_transport_engine transport;
	jack_timer_type_t clock_source;
	int driver_num;
	bool verbose;

	// CPU Load
	jack_time_t prev_cycle_time;
	jack_time_t cur_cycle_time;
	jack_time_t spare_usecs;
	jack_time_t max_usecs;
	jack_time_t rolling_client_usecs[JACK_ENGINE_ROLLING_COUNT];
	unsigned int rolling_client_usecs_cnt;
	int rolling_client_usecs_index;
	int rolling_interval;
	float CPU_load;

	// For OSX thread
	uint64_t period;
	uint64_t computation;
	uint64_t constraint;

	// Timer
	struct jack_frame_timer frame_timer;

#ifdef JACK_MONITOR
	struct jack_engine_profiling profiler;
#endif
} POST_PACKED_STRUCTURE;

PRE_PACKED_STRUCTURE
struct jack_client_control {
	jack_shm_info_t info;
	char name[JACK_CLIENT_NAME_SIZE+1];
	bool callback[jack_notify_max];
	volatile jack_transport_state_t transport_state;
	volatile bool transport_sync;
	volatile bool transport_timebase;
	int ref_num;
	int PID;
	bool active;

	int session_ID;
	char session_command[JACK_SESSION_COMMAND_SIZE];
	jack_session_flags_t session_flags;
} POST_PACKED_STRUCTURE;


static inline int jack_shm_alloc(size_t size, jack_shm_info_t *info, int num)
{
	char name[64];

	snprintf(name, sizeof(name), "/jack_shared%d", num);

	if (jack_shmalloc(name, size, info)) {
		pw_log_error("Cannot create shared memory segment of size = %zd (%s)", size, strerror(errno));
		return -1;
	}

	if (jack_attach_shm(info)) {
		jack_error("Cannot attach shared memory segment name = %s err = %s", name, strerror(errno));
		jack_destroy_shm(info);
		return -1;
	}
	info->size = size;
	return 0;
}
