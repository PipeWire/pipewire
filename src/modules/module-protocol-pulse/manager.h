/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_MANAGER_H
#define PIPEWIRE_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

#include <pipewire/pipewire.h>

struct client;
struct pw_manager_object;

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

	struct pw_core_info *info;

	uint32_t n_objects;
	struct spa_list object_list;
};

struct pw_manager_param {
	uint32_t id;
	int32_t seq;
	struct spa_list link;           /**< link in manager_object param_list */
	struct spa_pod *param;
};

struct pw_manager_object {
	struct spa_list link;           /**< link in manager object_list */
	uint64_t serial;
	uint32_t id;
	uint32_t permissions;
	const char *type;
	uint32_t version;
	uint32_t index;
	struct pw_properties *props;
	struct pw_proxy *proxy;
	char *message_object_path;
	int (*message_handler)(struct client *client, struct pw_manager_object *o,
	                       const char *message, const char *params, FILE *response);

	void *info;
	struct spa_param_info *params;
	uint32_t n_params;

#define PW_MANAGER_OBJECT_FLAG_SOURCE	(1<<0)
#define PW_MANAGER_OBJECT_FLAG_SINK	(1<<1)
	uint64_t change_mask;	/* object specific params change mask */
	struct spa_list param_list;
	unsigned int creating:1;
	unsigned int removing:1;
};

struct pw_manager *pw_manager_new(struct pw_core *core);

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

void *pw_manager_object_add_data(struct pw_manager_object *o, const char *key, size_t size);
void *pw_manager_object_get_data(struct pw_manager_object *obj, const char *key);
void *pw_manager_object_add_temporary_data(struct pw_manager_object *o, const char *key,
		size_t size, uint64_t lifetime_nsec);

bool pw_manager_object_is_client(struct pw_manager_object *o);
bool pw_manager_object_is_module(struct pw_manager_object *o);
bool pw_manager_object_is_card(struct pw_manager_object *o);
bool pw_manager_object_is_sink(struct pw_manager_object *o);
bool pw_manager_object_is_source(struct pw_manager_object *o);
bool pw_manager_object_is_monitor(struct pw_manager_object *o);
bool pw_manager_object_is_virtual(struct pw_manager_object *o);
bool pw_manager_object_is_network(struct pw_manager_object *o);
bool pw_manager_object_is_source_or_monitor(struct pw_manager_object *o);
bool pw_manager_object_is_sink_input(struct pw_manager_object *o);
bool pw_manager_object_is_source_output(struct pw_manager_object *o);
bool pw_manager_object_is_recordable(struct pw_manager_object *o);
bool pw_manager_object_is_link(struct pw_manager_object *o);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIPEWIRE_MANAGER_H */
