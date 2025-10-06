/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdalign.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/param/profiler.h>

#define PW_API_PROFILER		SPA_EXPORT
#include <pipewire/private.h>
#include <pipewire/impl.h>
#include <pipewire/extensions/profiler.h>

/** \page page_module_profiler Profiler
 *
 * The profiler module provides a Profiler interface for applications that
 * can be used to receive profiling information.
 *
 * Use tools like pw-top and pw-profiler to collect profiling information
 * about the pipewire graph.
 *
 * ## Module Name
 *
 * `libpipewire-module-profiler`
 *
 * ## Module Options
 *
 * - `profile.interval.ms`: Can be used to avoid gathering profiling information
 *			    on every processing cycle. This allows trading off
 *			    CPU usage for profiling accuracy. Default 0
 *
 * ## Config override
 *
 * A `module.profiler.args` config section can be added
 * to override the module arguments.
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-profiler-args.conf
 *
 * module.profiler.args = {
 *     #profile.interval.ms = 10
 * }
 *\endcode
 *
 * ## Example configuration
 *
 * The module is usually added to the config file of the main pipewire daemon.
 *
 *\code{.unparsed}
 * context.modules = [
 * { name = libpipewire-module-profiler
 *   args = {
 *       #profile.interval.ms = 0
 *   }
 * }
 * ]
 *\endcode
 *
 * ## See also
 *
 * - `pw-top`: a tool to display realtime profiler data
 * - `pw-profiler`: a tool to collect and render profiler data
 */

#define NAME "profiler"

PW_LOG_TOPIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define TMP_BUFFER		(16 * 1024)
#define DATA_BUFFER		(32 * 1024)
#define FLUSH_BUFFER		(8 * 1024)

int pw_protocol_native_ext_profiler_init(struct pw_context *context);

#define pw_profiler_resource(r,m,v,...)      \
	pw_resource_call(r,struct pw_profiler_events,m,v,__VA_ARGS__)

#define pw_profiler_resource_profile(r,...)        \
        pw_profiler_resource(r,profile,0,__VA_ARGS__)

#define DEFAULT_INTERVAL	0

#define MODULE_USAGE	"( profile.interval.ms=<minimum interval for sampling data (in ms) ) "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Generate Profiling data" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct node {
	struct spa_list link;
	struct impl *impl;

	struct pw_impl_node *node;
	struct spa_hook node_rt_listener;

	int64_t count;
	struct spa_ringbuffer buffer;
	uint8_t tmp[TMP_BUFFER];
	uint8_t data[DATA_BUFFER];

	unsigned enabled:1;
};

struct impl {
	struct pw_context *context;
	struct pw_properties *properties;

	struct pw_loop *main_loop;

	struct spa_hook context_listener;
	struct spa_hook module_listener;

	struct pw_global *global;
	struct spa_hook global_listener;

	struct spa_list node_list;

	uint32_t busy;
	struct spa_source *flush_event;
	unsigned int listening:1;

	uint8_t *flush;
	size_t flush_size;

	uint32_t interval;
	uint64_t last_signal_time;
};

struct resource_data {
	struct impl *impl;

	struct pw_resource *resource;
	struct spa_hook resource_listener;
};

static void do_flush_event(void *data, uint64_t count)
{
	struct impl *impl = data;
	struct pw_resource *resource;
	struct node *n;
	uint32_t total = 0;
	struct spa_pod_struct *p;

	p = (struct spa_pod_struct *)impl->flush;

	spa_list_for_each(n, &impl->node_list, link) {
		int32_t avail;
		uint32_t idx;

		avail = spa_ringbuffer_get_read_index(&n->buffer, &idx);

		pw_log_trace("%p: avail %d", impl, avail);

		if (avail > 0) {
			size_t size = total + avail + sizeof(struct spa_pod_struct);
			if (size > impl->flush_size) {
				uint8_t *flush;
				flush = realloc(impl->flush, size);
				if (flush == NULL) {
					pw_log_warn("%p: failed to realloc flush size %zu", impl, impl->flush_size);
					continue;
				}
				impl->flush = flush;
				impl->flush_size = size;
				pw_log_debug("%p: new flush buffer size %zu", impl, impl->flush_size);
				p = (struct spa_pod_struct *)impl->flush;
			}
			spa_ringbuffer_read_data(&n->buffer, n->data, DATA_BUFFER,
					idx % DATA_BUFFER,
					SPA_PTROFF(p, sizeof(struct spa_pod_struct) + total, void),
					avail);
			total += avail;
			spa_ringbuffer_read_update(&n->buffer, idx + avail);
		}
	}

	*p = SPA_POD_INIT_Struct(total);

	spa_list_for_each(resource, &impl->global->resource_list, link)
		pw_profiler_resource_profile(resource, &p->pod);
}

static void update_denom(struct spa_fraction *frac, uint32_t denom)
{
	if (frac->denom != 0)
		frac->num = frac->num * denom / frac->denom;
	frac->denom = denom;
}

static void context_do_profile(void *data)
{
	struct node *n = data;
	struct pw_impl_node *node = n->node;
	struct impl *impl = n->impl;
	struct spa_pod_builder b;
	struct spa_pod_frame f[2];
	uint32_t id = node->info.id;
	struct pw_node_activation *a = node->rt.target.activation;
	struct spa_io_position *pos = &a->position;
	struct pw_node_target *t;
	int32_t filled;
	uint32_t idx, avail;

	if (SPA_FLAG_IS_SET(pos->clock.flags, SPA_IO_CLOCK_FLAG_FREEWHEEL))
		return;

	if (a->signal_time - impl->last_signal_time < impl->interval)
		goto done;

	impl->last_signal_time = a->signal_time;

	spa_pod_builder_init(&b, n->tmp, sizeof(n->tmp));
	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_Profiler, 0);

	spa_pod_builder_prop(&b, SPA_PROFILER_info, 0);
	spa_pod_builder_add_struct(&b,
			SPA_POD_Long(n->count),
			SPA_POD_Float(a->cpu_load[0]),
			SPA_POD_Float(a->cpu_load[1]),
			SPA_POD_Float(a->cpu_load[2]),
			SPA_POD_Int(a->xrun_count));

	spa_pod_builder_prop(&b, SPA_PROFILER_clock, 0);
	spa_pod_builder_add_struct(&b,
			SPA_POD_Int(pos->clock.flags),
			SPA_POD_Int(pos->clock.id),
			SPA_POD_String(pos->clock.name),
			SPA_POD_Long(pos->clock.nsec),
			SPA_POD_Fraction(&pos->clock.rate),
			SPA_POD_Long(pos->clock.position),
			SPA_POD_Long(pos->clock.duration),
			SPA_POD_Long(pos->clock.delay),
			SPA_POD_Double(pos->clock.rate_diff),
			SPA_POD_Long(pos->clock.next_nsec),
			SPA_POD_Int(pos->state),
			SPA_POD_Int(pos->clock.cycle),
			SPA_POD_Long(pos->clock.xrun));

	spa_pod_builder_prop(&b, SPA_PROFILER_driverBlock, 0);
	spa_pod_builder_add_struct(&b,
			SPA_POD_Int(id),
			SPA_POD_String(node->name),
			SPA_POD_Long(a->prev_signal_time),
			SPA_POD_Long(a->signal_time),
			SPA_POD_Long(a->awake_time),
			SPA_POD_Long(a->finish_time),
			SPA_POD_Int(a->status),
			SPA_POD_Fraction(&node->latency),
			SPA_POD_Int(a->xrun_count));

	spa_list_for_each(t, &node->rt.target_list, link) {
		struct pw_impl_node *tn = t->node;
		struct pw_node_activation *ta = t->activation;
		struct spa_fraction latency;
		bool async;
		int64_t prev_signal_time;

		if (t->id == id)
			continue;

		if (tn != NULL) {
			latency = tn->latency;
			if (tn->force_quantum != 0)
				latency.num = tn->force_quantum;
			if (tn->force_rate != 0)
				update_denom(&latency, tn->force_rate);
			else if (tn->rate.denom != 0)
				update_denom(&latency, tn->rate.denom);
			async = tn->async;
			prev_signal_time = tn->rt.target.activation->prev_signal_time;
		} else {
			spa_zero(latency);
			async = false;
			prev_signal_time = ta->prev_signal_time;
		}

		spa_pod_builder_prop(&b, SPA_PROFILER_followerBlock, 0);
		spa_pod_builder_add_struct(&b,
			SPA_POD_Int(t->id),
			SPA_POD_String(t->name),
			SPA_POD_Long(prev_signal_time),
			SPA_POD_Long(async ? ta->prev_signal_time : ta->signal_time),
			SPA_POD_Long(async ? ta->prev_awake_time : ta->awake_time),
			SPA_POD_Long(async ? ta->prev_finish_time : ta->finish_time),
			SPA_POD_Int(ta->status),
			SPA_POD_Fraction(&latency),
			SPA_POD_Int(ta->xrun_count),
			SPA_POD_Bool(async));

		if (tn && tn->driver) {
			struct spa_io_position *tpos = &tn->rt.target.activation->position;
			spa_pod_builder_prop(&b, SPA_PROFILER_followerClock, 0);
			spa_pod_builder_add_struct(&b,
				SPA_POD_Int(tpos->clock.id),
				SPA_POD_String(tpos->clock.name),
				SPA_POD_Long(tpos->clock.nsec),
				SPA_POD_Fraction(&tpos->clock.rate),
				SPA_POD_Long(tpos->clock.position),
				SPA_POD_Long(tpos->clock.duration),
				SPA_POD_Long(tpos->clock.delay),
				SPA_POD_Double(tpos->clock.rate_diff),
				SPA_POD_Long(tpos->clock.next_nsec),
				SPA_POD_Long(tpos->clock.xrun));
		}
	}
	spa_pod_builder_pop(&b, &f[0]);

	if (b.state.offset > sizeof(n->tmp))
		goto done;

	filled = spa_ringbuffer_get_write_index(&n->buffer, &idx);
	if (filled < 0 || filled > DATA_BUFFER) {
		pw_log_warn("%p: queue xrun %d", impl, filled);
		goto done;
	}
	avail = DATA_BUFFER - filled;
	if (avail < b.state.offset) {
		pw_log_warn("%p: queue full %d < %d", impl, avail, b.state.offset);
		goto done;
	}
	spa_ringbuffer_write_data(&n->buffer,
			n->data, DATA_BUFFER,
			idx % DATA_BUFFER,
			b.data, b.state.offset);
	spa_ringbuffer_write_update(&n->buffer, idx + b.state.offset);

	pw_loop_signal_event(impl->main_loop, impl->flush_event);
done:
	n->count++;
}

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.complete = context_do_profile,
	.incomplete = context_do_profile,
};

static void enable_node_profiling(struct node *n, bool enabled)
{
	if (enabled && !n->enabled) {
		SPA_FLAG_SET(n->node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_add_rt_listener(n->node, &n->node_rt_listener, &node_rt_events, n);
	} else if (!enabled && n->enabled) {
		SPA_FLAG_CLEAR(n->node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER);
		pw_impl_node_remove_rt_listener(n->node, &n->node_rt_listener);
	}
	n->enabled = enabled;
}

static void enable_profiling(struct impl *impl, bool enabled)
{
	struct node *n;
	spa_list_for_each(n, &impl->node_list, link)
		enable_node_profiling(n, enabled);
}

static void context_driver_added(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		return;

	n->impl = impl;
	n->node = node;
	spa_list_append(&impl->node_list, &n->link);
	spa_ringbuffer_init(&n->buffer);

	if (impl->busy > 0)
		enable_node_profiling(n, true);
}

static struct node *find_node(struct impl *impl, struct pw_impl_node *node)
{
	struct node *n;
	spa_list_for_each(n, &impl->node_list, link) {
		if (n->node == node)
			return n;
	}
	return NULL;
}

static void context_driver_removed(void *data, struct pw_impl_node *node)
{
	struct impl *impl = data;
	struct node *n;

	n = find_node(impl, node);
	if (n == NULL)
		return;

	enable_node_profiling(n, false);
	spa_list_remove(&n->link);
	free(n);
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.driver_added = context_driver_added,
	.driver_removed = context_driver_removed,
};

static void stop_listener(struct impl *impl)
{
	if (impl->listening) {
		enable_profiling(impl, false);
		impl->listening = false;
	}
}

static void resource_destroy(void *data)
{
	struct impl *impl = data;
	if (--impl->busy == 0) {
		pw_log_info("%p: stopping profiler", impl);
		stop_listener(impl);
	}
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy,
};

static int
global_bind(void *object, struct pw_impl_client *client, uint32_t permissions,
            uint32_t version, uint32_t id)
{
	struct impl *impl = object;
	struct pw_global *global = impl->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions,
			PW_TYPE_INTERFACE_Profiler, version, sizeof(*data));
        if (resource == NULL)
                return -errno;

        data = pw_resource_get_user_data(resource);
        data->impl = impl;
        data->resource = resource;
	pw_global_add_resource(global, resource);

	pw_resource_add_listener(resource, &data->resource_listener,
			&resource_events, impl);

	if (++impl->busy == 1) {
		pw_log_info("%p: starting profiler", impl);
		enable_profiling(impl, true);
		impl->listening = true;
	}
	return 0;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	if (impl->global != NULL)
		pw_global_destroy(impl->global);

	spa_hook_remove(&impl->context_listener);
	spa_hook_remove(&impl->module_listener);

	pw_properties_free(impl->properties);

	pw_loop_destroy_source(impl->main_loop, impl->flush_event);

	free(impl->flush);
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void global_destroy(void *data)
{
	struct impl *impl = data;

	stop_listener(impl);

	spa_hook_remove(&impl->global_listener);
	impl->global = NULL;
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;
	impl->flush_size = FLUSH_BUFFER + sizeof(struct spa_pod_struct);
	impl->flush = malloc(impl->flush_size);
	if (impl->flush == NULL) {
		free(impl);
		return -errno;
	}

	spa_list_init(&impl->node_list);
	pw_protocol_native_ext_profiler_init(context);

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = pw_properties_new(NULL, NULL);

	impl->context = context;
	impl->properties = props;
	impl->main_loop = pw_context_get_main_loop(impl->context);

	pw_context_conf_update_props(context, "module."NAME".args", props);

	impl->interval = SPA_NSEC_PER_MSEC *
		pw_properties_get_uint32(props, "profile.interval.ms", DEFAULT_INTERVAL);
	impl->last_signal_time = 0;

	impl->global = pw_global_new(context,
			PW_TYPE_INTERFACE_Profiler,
			PW_VERSION_PROFILER,
			PW_PROFILER_PERM_MASK,
			pw_properties_copy(props),
			global_bind, impl);
	if (impl->global == NULL) {
		free(impl->flush);
		free(impl);
		return -errno;
	}
	pw_properties_setf(impl->properties, PW_KEY_OBJECT_ID, "%d", pw_global_get_id(impl->global));
	pw_properties_setf(impl->properties, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(impl->global));

	impl->flush_event = pw_loop_add_event(impl->main_loop, do_flush_event, impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	pw_context_add_listener(impl->context,
			&impl->context_listener,
			&context_events, impl);

	pw_global_register(impl->global);

	pw_global_add_listener(impl->global, &impl->global_listener, &global_events, impl);

	return 0;
}
