/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_MANAGER_H
#define PIPEWIRE_MANAGER_H

#include <stdio.h>

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

struct client;
struct pw_manager_object;

#define PW_TYPE_INTERFACE_NODE_BASE    PW_TYPE_INTERFACE_Node ":"
#define PW_TYPE_INTERFACE_NODE_Stream  PW_TYPE_INTERFACE_NODE_BASE "Stream"

struct pw_manager_events {
#define PW_VERSION_MANAGER_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);

	void (*sync) (void *data);

	void (*added) (void *data, struct pw_manager_object *object);

	void (*updated) (void *data, struct pw_manager_object *object);

	void (*removed) (void *data, struct pw_manager_object *object);

	void (*metadata) (void *data, struct pw_manager_object *object,
			uint32_t subject, const char *key,
			const char *type, const char *value);

	void (*disconnect) (void *data);

	void (*object_data_timeout) (void *data, struct pw_manager_object *object,
			const char *key);
};

struct pw_manager {
	struct pw_core *core;
	struct pw_registry *registry;
};

struct pw_manager_param {
	uint32_t id;
	int32_t seq;
	struct spa_list link;           /**< link in manager_object param_list */
	struct spa_pod *param;
};

struct pw_manager_object {
	uint64_t serial;
	uint32_t id;
	uint32_t permissions;
	const char *type;
	uint32_t version;
	uint32_t index;
	struct pw_properties *props;

	char *message_object_path;
	int (*message_handler)(struct client *client, struct pw_manager_object *o,
	                       const char *message, const char *params, FILE *response);

	union {
		struct {
#define PW_MANAGER_STREAM_TYPE_NONE		(0)
#define PW_MANAGER_STREAM_TYPE_SINK		(1)
#define PW_MANAGER_STREAM_TYPE_SOURCE		(2)
#define PW_MANAGER_STREAM_TYPE_SINK_INPUT	(3)
#define PW_MANAGER_STREAM_TYPE_SOURCE_OUTPUT	(4)
			uint32_t type;
			bool monitor;
		} stream;
	} u;
};

struct pw_manager *pw_manager_new(struct pw_core *core);
uint32_t pw_manager_core_cookie(struct pw_manager *manager);

void pw_manager_add_listener(struct pw_manager *manager,
		struct spa_hook *listener,
		const struct pw_manager_events *events, void *data);

int pw_manager_sync(struct pw_manager *manager);

void pw_manager_destroy(struct pw_manager *manager);

int pw_manager_set_metadata(struct pw_manager *manager,
		struct pw_manager_object *metadata,
		uint32_t subject, const char *key, const char *type,
		const char *format, ...) SPA_PRINTF_FUNC(6,7);

int pw_manager_for_each_object(struct pw_manager *manager,
		int (*callback) (void *data, struct pw_manager_object *object),
		void *data);

uint32_t pw_manager_id_to_index(struct pw_manager *manager, uint32_t id);

void *pw_manager_object_add_data(struct pw_manager_object *o, const char *key, size_t size);
void *pw_manager_object_get_data(struct pw_manager_object *obj, const char *key);
void *pw_manager_object_add_temporary_data(struct pw_manager_object *o, const char *key,
		size_t size, uint64_t lifetime_nsec);

struct pw_proxy *pw_manager_object_get_proxy(struct pw_manager_object *obj);
struct spa_param_info *pw_manager_object_get_param_info(struct pw_manager_object *obj, uint32_t *n_params);
struct spa_list *pw_manager_object_get_param_list(struct pw_manager_object *obj);

bool pw_manager_object_is_client(struct pw_manager_object *o);
bool pw_manager_object_is_module(struct pw_manager_object *o);
bool pw_manager_object_is_card(struct pw_manager_object *o);
bool pw_manager_object_is_sink(struct pw_manager_object *o);
bool pw_manager_object_is_source(struct pw_manager_object *o);
bool pw_manager_object_is_virtual(struct pw_manager_object *o);
bool pw_manager_object_is_network(struct pw_manager_object *o);
bool pw_manager_object_is_sink_input(struct pw_manager_object *o);
bool pw_manager_object_is_source_output(struct pw_manager_object *o);
bool pw_manager_object_is_recordable(struct pw_manager_object *o);
bool pw_manager_object_is_link(struct pw_manager_object *o);

struct pw_manager_object *pw_manager_sink_get_monitor(struct pw_manager_object *o);
struct pw_manager_object *pw_manager_monitor_get_sink(struct pw_manager_object *o);

const char *pw_manager_module_name(struct pw_manager_object *o);
const char *pw_manager_module_args(struct pw_manager_object *o);
int pw_manager_node_state(struct pw_manager_object *o);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIPEWIRE_MANAGER_H */
