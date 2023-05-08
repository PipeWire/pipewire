/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_WEAK_JACK_H
#define PIPEWIRE_WEAK_JACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

#include <dlfcn.h>

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/midiport.h>

struct weakjack {
	jack_nframes_t (*cycle_wait) (jack_client_t* client);
	void (*cycle_signal) (jack_client_t* client, int status);

	jack_nframes_t (*frame_time) (const jack_client_t *);
	int (*get_cycle_times) (const jack_client_t *client,
			jack_nframes_t *current_frames,
			jack_time_t    *current_usecs,
			jack_time_t    *next_usecs,
			float          *period_usecs);
	jack_transport_state_t (*transport_query) (const jack_client_t *client,
                                             jack_position_t *pos);

	jack_client_t * (*client_open) (const char *client_name,
                                  jack_options_t options,
                                  jack_status_t *status, ...);
	int (*client_close) (jack_client_t *client);


	int (*activate) (jack_client_t *client);
	int (*deactivate) (jack_client_t *client);

	jack_nframes_t (*get_sample_rate) (jack_client_t *);

	int (*recompute_total_latencies) (jack_client_t *client);

	jack_port_t * (*port_register) (jack_client_t *client,
			const char *port_name,
			const char *port_type,
			unsigned long flags,
			unsigned long buffer_size);
	int (*port_unregister) (jack_client_t *client, jack_port_t *port);
	void * (*port_get_buffer) (jack_port_t *port, jack_nframes_t);
	const char * (*port_name) (const jack_port_t *port);

	void (*port_get_latency_range) (jack_port_t *port,
			jack_latency_callback_mode_t mode,
			jack_latency_range_t *range);
	void (*port_set_latency_range) (jack_port_t *port,
			jack_latency_callback_mode_t mode,
			jack_latency_range_t *range);


	int (*connect) (jack_client_t *client,
			const char *source_port,
			const char *destination_port);
	int (*disconnect) (jack_client_t *client,
			const char *source_port,
			const char *destination_port);

	const char ** (*get_ports) (jack_client_t *client,
			const char *port_name_pattern,
			const char *type_name_pattern,
			unsigned long flags);
	void (*free) (void* ptr);

	int (*set_process_thread) (jack_client_t* client,
			JackThreadCallback thread_callback, void *arg);
	int (*set_xrun_callback) (jack_client_t *client,
			JackXRunCallback xrun_callback, void *arg);
	void (*on_info_shutdown) (jack_client_t *client,
			JackInfoShutdownCallback shutdown_callback, void *arg);
	int (*set_latency_callback) (jack_client_t *client,
			JackLatencyCallback latency_callback, void *arg);

	void (*midi_clear_buffer) (void *port_buffer);
	int (*midi_event_write) (void *port_buffer,
			jack_nframes_t time,
			const jack_midi_data_t *data,
			size_t data_size);
	uint32_t (*midi_get_event_count) (void* port_buffer);
	int (*midi_event_get) (jack_midi_event_t *event, void *port_buffer,
			uint32_t event_index);

};


static inline int weakjack_load_by_path(struct weakjack *jack, const char *path)
{
	void *hnd;

	hnd = dlopen(path, RTLD_NOW);
	if (hnd == NULL)
		return -errno;

	pw_log_info("opened libjack: %s", path);

#define LOAD_SYM(name) ({					\
	if ((jack->name =  dlsym(hnd, "jack_"#name )) == NULL)	\
		return -ENOSYS;					\
})
	spa_zero(*jack);
	LOAD_SYM(cycle_wait);
	LOAD_SYM(cycle_signal);
	LOAD_SYM(frame_time);
	LOAD_SYM(get_cycle_times);
	LOAD_SYM(transport_query);

	LOAD_SYM(client_open);
	LOAD_SYM(client_close);

	LOAD_SYM(activate);
	LOAD_SYM(deactivate);

	LOAD_SYM(get_sample_rate);

	LOAD_SYM(recompute_total_latencies);

	LOAD_SYM(port_register);
	LOAD_SYM(port_unregister);
	LOAD_SYM(port_get_buffer);
	LOAD_SYM(port_name);

	LOAD_SYM(port_get_latency_range);
	LOAD_SYM(port_set_latency_range);

	LOAD_SYM(connect);
	LOAD_SYM(disconnect);

	LOAD_SYM(get_ports);
	LOAD_SYM(free);

	LOAD_SYM(set_process_thread);
	LOAD_SYM(set_xrun_callback);
	LOAD_SYM(on_info_shutdown);
	LOAD_SYM(set_latency_callback);

	LOAD_SYM(midi_clear_buffer);
	LOAD_SYM(midi_event_write);
	LOAD_SYM(midi_get_event_count);
	LOAD_SYM(midi_event_get);
#undef LOAD_SYM

	return 0;
}

static inline int weakjack_load(struct weakjack *jack, const char *lib)
{
	int res = -ENOENT;

	if (lib[0] != '/') {
		const char *search_dirs, *p, *state = NULL;
		char path[PATH_MAX];
		size_t len;

		search_dirs = getenv("LIBJACK_PATH");
		if (!search_dirs)
			search_dirs = PREFIX "/lib64/:" PREFIX "/lib/:"
				"/usr/lib64/:/usr/lib/:" LIBDIR;

		while ((p = pw_split_walk(search_dirs, ":", &len, &state))) {
			int pathlen;

			if (len >= sizeof(path)) {
				res = -ENAMETOOLONG;
				continue;
			}
			pathlen = snprintf(path, sizeof(path), "%.*s/%s", (int) len, p, lib);
			if (pathlen < 0 || (size_t) pathlen >= sizeof(path)) {
				res = -ENAMETOOLONG;
				continue;
			}
			if ((res = weakjack_load_by_path(jack, path)) == 0)
				break;
		}
	} else {
		res = weakjack_load_by_path(jack, lib);
	}
	return res;
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_WEAK_JACK_H */
