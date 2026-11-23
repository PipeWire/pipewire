/* PipeWire */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <time.h>

#include "pwtest.h"

#include <spa/support/plugin.h>

#include <pipewire/pipewire.h>

#define TIMEOUT_MS	5000
#define N_NODES		2

struct data {
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	char name[64];
	uint32_t id;
	bool removed;
	int sync_seq;
	bool synced;
	int error;
};

static void core_done(void *userdata, uint32_t id, int seq)
{
	struct data *d = userdata;
	if (id == PW_ID_CORE && seq == d->sync_seq)
		d->synced = true;
}

static void core_error(void *userdata, uint32_t id, int seq, int res, const char *message)
{
	struct data *d = userdata;
	fprintf(stderr, "core error id:%u res:%d %s\n", id, res, message);
	d->error = res;
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done,
	.error = core_error,
};

static void registry_global(void *userdata, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct data *d = userdata;
	const char *name;

	if (spa_streq(type, PW_TYPE_INTERFACE_Node) && props != NULL &&
	    (name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) != NULL &&
	    spa_streq(name, d->name))
		d->id = id;
}

static void registry_global_remove(void *userdata, uint32_t id)
{
	struct data *d = userdata;
	if (id == d->id)
		d->removed = true;
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool node_added(struct data *d) { return d->id != SPA_ID_INVALID || d->error < 0; }
static bool node_removed(struct data *d) { return d->removed || d->error < 0; }
static bool synced(struct data *d) { return d->synced || d->error < 0; }

static void iterate_until(struct data *d, bool (*cond)(struct data *d))
{
	uint64_t end = now_ms() + TIMEOUT_MS;

	while (!cond(d) && now_ms() < end)
		pw_loop_iterate(d->loop, 10);
}

/* Destroying a videotestsrc node clears the plugin handle and its timer.
 * The daemon must survive that and create the node again. */
PWTEST(videotestsrc_destroy_and_recreate)
{
	struct data d = { 0 };

	pw_init(0, NULL);
	d.loop = pw_loop_new(NULL);
	pwtest_ptr_notnull(d.loop);
	d.context = pw_context_new(d.loop, NULL, 0);
	pwtest_ptr_notnull(d.context);
	d.core = pw_context_connect(d.context, NULL, 0);
	pwtest_ptr_notnull(d.core);
	pw_core_add_listener(d.core, &d.core_listener, &core_events, &d);

	pw_loop_enter(d.loop);

	d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(d.registry, &d.registry_listener, &registry_events, &d);

	for (int i = 0; i < N_NODES; i++) {
		struct pw_properties *props;
		struct pw_proxy *proxy;

		snprintf(d.name, sizeof(d.name), "videotestsrc-destroy-%d", i);
		d.id = SPA_ID_INVALID;
		d.removed = false;
		d.synced = false;

		props = pw_properties_new(
				SPA_KEY_FACTORY_NAME, "videotestsrc",
				SPA_KEY_LIBRARY_NAME, "videotestsrc/libspa-videotestsrc",
				PW_KEY_NODE_NAME, d.name,
				NULL);
		proxy = pw_core_create_object(d.core, "spa-node-factory",
				PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0);
		pw_properties_free(props);
		pwtest_ptr_notnull(proxy);

		iterate_until(&d, node_added);
		pwtest_int_eq(d.error, 0);
		pwtest_int_ne(d.id, SPA_ID_INVALID);

		pw_registry_destroy(d.registry, d.id);
		iterate_until(&d, node_removed);

		/* the daemon must still answer after the node is gone */
		d.sync_seq = pw_core_sync(d.core, PW_ID_CORE, 0);
		iterate_until(&d, synced);

		fprintf(stderr, "node:%d id:%u removed:%d synced:%d error:%d\n",
				i, d.id, d.removed, d.synced, d.error);
		pwtest_int_eq(d.error, 0);
		pwtest_bool_true(d.removed);
		pwtest_bool_true(d.synced);

		pw_proxy_destroy(proxy);
	}

	pw_proxy_destroy((struct pw_proxy *)d.registry);
	pw_loop_leave(d.loop);
	pw_core_disconnect(d.core);
	pw_context_destroy(d.context);
	pw_loop_destroy(d.loop);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST_SUITE(videotestsrc)
{
	pwtest_add(videotestsrc_destroy_and_recreate, PWTEST_ARG_DAEMON);

	return PWTEST_PASS;
}
