/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <regex.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <fnmatch.h>

#include <pipewire/log.h>

#include <spa/support/cpu.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/support/plugin-loader.h>
#include <spa/node/utils.h>
#include <spa/utils/atomic.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/cleanup.h>
#include <spa/debug/types.h>

#include <pipewire/impl.h>
#include <pipewire/private.h>
#include <pipewire/thread.h>
#include <pipewire/conf.h>

#include <pipewire/extensions/protocol-native.h>

PW_LOG_TOPIC_EXTERN(log_context);
#define PW_LOG_TOPIC_DEFAULT log_context

#define MAX_LOOPS	64u

#define DEFAULT_DATA_LOOPS	1

#if !defined(FNM_EXTMATCH)
#define FNM_EXTMATCH 0
#endif

struct data_loop {
	struct pw_data_loop *impl;
	bool autostart;
	bool started;
	uint64_t last_used;
};

/** \cond */
struct impl {
	struct pw_context this;
	struct spa_handle *dbus_handle;
	struct spa_plugin_loader plugin_loader;
	unsigned int recalc:1;
	unsigned int recalc_pending:1;

	uint32_t cpu_count;

	uint32_t n_data_loops;
	struct data_loop data_loops[MAX_LOOPS];
};


struct factory_entry {
	regex_t regex;
	char *lib;
};
/** \endcond */

static void fill_properties(struct pw_context *context)
{
	struct pw_properties *properties = context->properties;

	if (!pw_properties_get(properties, PW_KEY_APP_NAME))
		pw_properties_set(properties, PW_KEY_APP_NAME, pw_get_client_name());

	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_BINARY))
		pw_properties_set(properties, PW_KEY_APP_PROCESS_BINARY, pw_get_prgname());

	if (!pw_properties_get(properties, PW_KEY_APP_LANGUAGE)) {
		pw_properties_set(properties, PW_KEY_APP_LANGUAGE, getenv("LANG"));
	}
	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_ID)) {
		pw_properties_setf(properties, PW_KEY_APP_PROCESS_ID, "%zd", (size_t) getpid());
	}
	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_USER))
		pw_properties_set(properties, PW_KEY_APP_PROCESS_USER, pw_get_user_name());

	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_HOST))
		pw_properties_set(properties, PW_KEY_APP_PROCESS_HOST, pw_get_host_name());

	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_SESSION_ID)) {
		pw_properties_set(properties, PW_KEY_APP_PROCESS_SESSION_ID,
				  getenv("XDG_SESSION_ID"));
	}
	if (!pw_properties_get(properties, PW_KEY_WINDOW_X11_DISPLAY)) {
		pw_properties_set(properties, PW_KEY_WINDOW_X11_DISPLAY,
				  getenv("DISPLAY"));
	}
}
static void fill_core_properties(struct pw_context *context)
{
	struct pw_properties *properties = context->properties;
	pw_properties_set(properties, PW_KEY_CORE_VERSION, context->core->info.version);
	pw_properties_set(properties, PW_KEY_CORE_NAME, context->core->info.name);
}

SPA_EXPORT
int pw_context_set_freewheel(struct pw_context *context, bool freewheel)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	struct spa_thread *thr;
	uint32_t i;
	int res = 0;

	if (context->freewheeling == freewheel)
		return 0;

	for (i = 0; i < impl->n_data_loops; i++) {
		if (impl->data_loops[i].impl == NULL ||
		    (thr = pw_data_loop_get_thread(impl->data_loops[i].impl)) == NULL)
			continue;

		if (freewheel) {
			pw_log_info("%p: enter freewheel", context);
			if (context->thread_utils)
				res = spa_thread_utils_drop_rt(context->thread_utils, thr);
		} else {
			pw_log_info("%p: exit freewheel", context);
			/* Use the priority as configured within the realtime module */
			if (context->thread_utils)
				res = spa_thread_utils_acquire_rt(context->thread_utils, thr, -1);
		}
		if (res < 0)
			pw_log_info("%p: freewheel error:%s", context, spa_strerror(res));
	}
	context->freewheeling = freewheel;

	return res;
}

static struct spa_handle *impl_plugin_loader_load(void *object, const char *factory_name, const struct spa_dict *info)
{
	struct impl *impl = object;

	if (impl == NULL || factory_name == NULL) {
		errno = EINVAL;
		return NULL;
	}

	return pw_context_load_spa_handle(&impl->this, factory_name, info);
}

static int impl_plugin_loader_unload(void *object, struct spa_handle *handle)
{
	spa_return_val_if_fail(object != NULL, -EINVAL);
	return pw_unload_spa_handle(handle);
}

static const struct spa_plugin_loader_methods impl_plugin_loader = {
        SPA_VERSION_PLUGIN_LOADER_METHODS,
        .load = impl_plugin_loader_load,
	.unload = impl_plugin_loader_unload,
};

static void init_plugin_loader(struct impl *impl)
{
	impl->plugin_loader.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_PluginLoader,
		SPA_VERSION_PLUGIN_LOADER,
		&impl_plugin_loader,
		impl);
}

static int do_data_loop_setup(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_context *this = user_data;
	const char *str;
	struct spa_cpu *cpu;

	cpu = spa_support_find(this->support, this->n_support, SPA_TYPE_INTERFACE_CPU);

	if ((str = pw_properties_get(this->properties, SPA_KEY_CPU_ZERO_DENORMALS)) != NULL &&
	    cpu != NULL) {
		pw_log_info("setting zero denormals: %s", str);
		spa_cpu_zero_denormals(cpu, spa_atob(str));
	}
	return 0;
}

static int setup_data_loops(struct impl *impl)
{
	struct pw_properties *pr;
	struct pw_context *this = &impl->this;
	const char *str, *lib_name;
	uint32_t i;
	int res = 0;

	pr = pw_properties_copy(this->properties);

	lib_name = pw_properties_get(this->properties, "context.data-loop." PW_KEY_LIBRARY_NAME_SYSTEM);

	if ((str = pw_properties_get(this->properties, "context.data-loops")) != NULL) {
		struct spa_json it[2];
		char key[512];
		int r, len = strlen(str);
		spa_autofree char *s = strndup(str, len);

		i = 0;
		if (spa_json_begin_array(&it[0], s, len) < 0) {
			pw_log_error("context.data-loops is not an array in '%s'", str);
			res = -EINVAL;
			goto exit;
		}
		while ((r = spa_json_enter_object(&it[0], &it[1])) > 0) {
			char *props = NULL;
			const char *val;
			int l;

			if (i >= MAX_LOOPS) {
				pw_log_warn("too many context.data-loops, using first %d",
					MAX_LOOPS);
				break;
			}

			pw_properties_clear(pr);
			pw_properties_update(pr, &this->properties->dict);
			pw_properties_set(pr, PW_KEY_LIBRARY_NAME_SYSTEM, lib_name);
			pw_properties_set(pr, "loop.prio-inherit",  "true");

			while ((l = spa_json_object_next(&it[1], key, sizeof(key), &val)) > 0) {
				if (spa_json_is_container(val, l))
					l = spa_json_container_len(&it[1], val, l);

				props = (char*)val;
				spa_json_parse_stringn(val, l, props, l+1);
				pw_properties_set(pr, key, props);
				pw_log_info("loop %d: \"%s\" = %s", i, key, props);
			}
			impl->data_loops[i].impl = pw_data_loop_new(&pr->dict);
			if (impl->data_loops[i].impl == NULL)  {
				res = -errno;
				goto exit;
			}
			i++;
		}
		impl->n_data_loops = i;
	} else {
		int32_t count = pw_properties_get_int32(pr, "context.num-data-loops",
				DEFAULT_DATA_LOOPS);
		if (count < 0)
			count = impl->cpu_count;

		impl->n_data_loops = count;
		if (impl->n_data_loops > MAX_LOOPS) {
			pw_log_warn("too many context.num-data-loops: %d, using %d",
					impl->n_data_loops, MAX_LOOPS);
			impl->n_data_loops = MAX_LOOPS;
		}
		for (i = 0; i < impl->n_data_loops; i++) {
			pw_properties_setf(pr, SPA_KEY_THREAD_NAME,  "data-loop.%d", i);
			pw_properties_set(pr, "loop.prio-inherit",  "true");
			impl->data_loops[i].impl = pw_data_loop_new(&pr->dict);
			if (impl->data_loops[i].impl == NULL)  {
				res = -errno;
				goto exit;
			}
			pw_log_info("created data loop '%s'", impl->data_loops[i].impl->loop->name);
		}
	}
	pw_log_info("created %d data-loops", impl->n_data_loops);
exit:
	pw_properties_free(pr);
	return res;
}

static int data_loop_start(struct impl *impl, struct data_loop *loop)
{
	int res;
	if (loop->started || loop->impl == NULL)
		return 0;

	pw_log_info("starting data loop %s", loop->impl->loop->name);
	if ((res = pw_data_loop_start(loop->impl)) < 0)
		return res;

	pw_data_loop_invoke(loop->impl, do_data_loop_setup, 0, NULL, 0, false, &impl->this);
	loop->started = true;
	return 0;
}

static void data_loop_stop(struct impl *impl, struct data_loop *loop)
{
	if (!loop->started || loop->impl == NULL)
		return;
	pw_data_loop_stop(loop->impl);
	loop->started = false;
}

static int adjust_rlimit(int resource, const char *name, int value)
{
	struct rlimit rlim, highest, fixed;

	rlim = (struct rlimit) { .rlim_cur = value, .rlim_max = value, };

	if (setrlimit(resource, &rlim) >= 0) {
		pw_log_info("set rlimit %s to %d", name, value);
		return 0;
	}
	if (errno != EPERM)
		return -errno;

	/* So we failed to set the desired setrlimit, then let's try
	* to get as close as we can */
	if (getrlimit(resource, &highest) < 0)
		return -errno;

	/* If the hard limit is unbounded anyway, then the EPERM had other reasons,
	 * let's propagate the original EPERM then */
	if (highest.rlim_max == RLIM_INFINITY)
		return -EPERM;

        fixed = (struct rlimit) {
		.rlim_cur = SPA_MIN(rlim.rlim_cur, highest.rlim_max),
		.rlim_max = SPA_MIN(rlim.rlim_max, highest.rlim_max),
	};

	/* Shortcut things if we wouldn't change anything. */
	if (fixed.rlim_cur == highest.rlim_cur &&
	    fixed.rlim_max == highest.rlim_max)
		return 0;

	pw_log_info("set rlimit %s to %d/%d instead of %d", name,
			(int)fixed.rlim_cur, (int)fixed.rlim_max, value);
	if (setrlimit(resource, &fixed) < 0)
		return -errno;

	return 0;
}

static int adjust_rlimits(const struct spa_dict *dict)
{
	const struct spa_dict_item *it;
	static const char* rlimit_table[] = {
		[RLIMIT_AS]         = "as",
		[RLIMIT_CORE]       = "core",
		[RLIMIT_CPU]        = "cpu",
		[RLIMIT_DATA]       = "data",
		[RLIMIT_FSIZE]      = "fsize",
		[RLIMIT_LOCKS]      = "locks",
		[RLIMIT_MEMLOCK]    = "memlock",
		[RLIMIT_MSGQUEUE]   = "msgqueue",
		[RLIMIT_NICE]       = "nice",
		[RLIMIT_NOFILE]     = "nofile",
		[RLIMIT_NPROC]      = "nproc",
		[RLIMIT_RSS]        = "rss",
		[RLIMIT_RTPRIO]     = "rtprio",
		[RLIMIT_RTTIME]     = "rttime",
		[RLIMIT_SIGPENDING] = "sigpending",
		[RLIMIT_STACK]      = "stack",
	};
	int res;
	spa_dict_for_each(it, dict) {
		if (!spa_strstartswith(it->key, "rlimit."))
			continue;
		for (size_t i = 0; i < SPA_N_ELEMENTS(rlimit_table); i++) {
			const char *name = rlimit_table[i];
			int64_t val;
			if (!spa_streq(it->key+7, name))
				continue;
			if (!spa_atoi64(it->value, &val, 0)) {
				pw_log_warn("invalid number %s", it->value);
			} else if ((res = adjust_rlimit(i, name, val)) < 0)
				pw_log_warn("can't set rlimit %s to %s: %s",
						name, it->value, spa_strerror(res));
			break;
		}
	}
	return 0;
}

/** Create a new context object
 *
 * \param main_loop the main loop to use
 * \param properties extra properties for the context, ownership it taken
 *
 * \return a newly allocated context object
 */
SPA_EXPORT
struct pw_context *pw_context_new(struct pw_loop *main_loop,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_context *this;
	const char *lib, *str;
	void *dbus_iface = NULL;
	uint32_t i, n_support, vm_type;
	struct pw_properties *conf;
	struct spa_cpu *cpu;
	int res = 0;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL) {
		pw_properties_free(properties);
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;

	pw_log_debug("%p: new", this);

	if (user_data_size > 0)
		this->user_data = SPA_PTROFF(impl, sizeof(struct impl), void);

	pw_array_init(&this->factory_lib, 32);
	pw_array_init(&this->objects, 32);
	pw_map_init(&this->globals, 128, 32);

	spa_list_init(&this->core_impl_list);
	spa_list_init(&this->protocol_list);
	spa_list_init(&this->core_list);
	spa_list_init(&this->registry_resource_list);
	spa_list_init(&this->global_list);
	spa_list_init(&this->module_list);
	spa_list_init(&this->device_list);
	spa_list_init(&this->client_list);
	spa_list_init(&this->node_list);
	spa_list_init(&this->factory_list);
	spa_list_init(&this->metadata_list);
	spa_list_init(&this->link_list);
	spa_list_init(&this->control_list[0]);
	spa_list_init(&this->control_list[1]);
	spa_list_init(&this->export_list);
	spa_list_init(&this->driver_list);
	spa_hook_list_init(&this->listener_list);
	spa_hook_list_init(&this->driver_listener_list);

	this->sc_pagesize = sysconf(_SC_PAGESIZE);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_free;
	}
	this->properties = properties;

	fill_properties(this);

	conf = pw_properties_new(NULL, NULL);
	if (conf == NULL) {
		res = -errno;
		goto error_free;
	}
	this->conf = conf;
	if ((res = pw_conf_load_conf_for_context (properties, conf)) < 0)
		goto error_free;

	n_support = pw_get_support(this->support, SPA_N_ELEMENTS(this->support) - 6);
	cpu = spa_support_find(this->support, n_support, SPA_TYPE_INTERFACE_CPU);

	vm_type = SPA_CPU_VM_NONE;
	if (cpu != NULL && (vm_type = spa_cpu_get_vm_type(cpu)) != SPA_CPU_VM_NONE)
		pw_properties_set(properties, "cpu.vm.name", spa_cpu_vm_type_to_string(vm_type));

	res = pw_context_conf_update_props(this, "context.properties", properties);
	pw_log_info("%p: parsed %d context.properties items", this, res);

	if ((str = getenv("PIPEWIRE_CORE"))) {
		pw_log_info("using core.name from environment: %s", str);
		pw_properties_set(properties, PW_KEY_CORE_NAME, str);
	}

	if ((str = pw_properties_get(properties, "vm.overrides")) != NULL) {
		pw_log_warn("vm.overrides in context.properties are deprecated, "
				"use context.properties.rules instead");
		if (vm_type != SPA_CPU_VM_NONE)
			pw_properties_update_string(properties, str, strlen(str));
		pw_properties_set(properties, "vm.overrides", NULL);
	}
	if (cpu != NULL) {
		if (pw_properties_get(properties, PW_KEY_CPU_MAX_ALIGN) == NULL)
			pw_properties_setf(properties, PW_KEY_CPU_MAX_ALIGN,
				"%u", spa_cpu_get_max_align(cpu));
		impl->cpu_count = spa_cpu_get_count(cpu);
	}

	if (getenv("PIPEWIRE_DEBUG") == NULL &&
			(str = pw_properties_get(properties, "log.level")) != NULL) {
		if (pw_log_set_level_string(str) < 0)
			pw_log_warn("%p: invalid log.level in context properties", this);
	}

	if (pw_properties_get_bool(properties, "mem.mlock-all", false)) {
		if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
			pw_log_warn("%p: could not mlockall; %m", impl);
		else
			pw_log_info("%p: mlockall succeeded", impl);
	}
	adjust_rlimits(&properties->dict);

	pw_settings_init(this);
	this->settings = this->defaults;

	if ((res = setup_data_loops(impl)) < 0)
		goto error_free;

	this->pool = pw_mempool_new(NULL);
	if (this->pool == NULL) {
		res = -errno;
		goto error_free;
	}

	this->main_loop = main_loop;
	this->work_queue = pw_work_queue_new(this->main_loop);
	if (this->work_queue == NULL) {
		res = -errno;
		goto error_free;
	}

	init_plugin_loader(impl);

	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_System, this->main_loop->system);
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Loop, this->main_loop->loop);
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_LoopUtils, this->main_loop->utils);
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_PluginLoader, &impl->plugin_loader);

	if ((str = pw_properties_get(properties, "support.dbus")) == NULL ||
	    pw_properties_parse_bool(str)) {
		lib = pw_properties_get(properties, PW_KEY_LIBRARY_NAME_DBUS);
		if (lib == NULL)
			lib = "support/libspa-dbus";

		impl->dbus_handle = pw_load_spa_handle(lib,
				SPA_NAME_SUPPORT_DBUS, NULL,
				n_support, this->support);

		if (impl->dbus_handle == NULL) {
			pw_log_warn("%p: can't load dbus library: %s", this, lib);
		} else if ((res = spa_handle_get_interface(impl->dbus_handle,
							   SPA_TYPE_INTERFACE_DBus, &dbus_iface)) < 0) {
			pw_log_warn("%p: can't load dbus interface: %s", this, spa_strerror(res));
		} else {
			this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DBus, dbus_iface);
		}
	}
	this->n_support = n_support;
	spa_assert(n_support <= SPA_N_ELEMENTS(this->support));

	this->core = pw_context_create_core(this, pw_properties_copy(properties), 0);
	if (this->core == NULL) {
		res = -errno;
		goto error_free;
	}
	pw_impl_core_register(this->core, NULL);

	fill_core_properties(this);

	if ((res = pw_context_parse_conf_section(this, conf, "context.spa-libs")) < 0)
		goto error_free;
	pw_log_info("%p: parsed %d context.spa-libs items", this, res);
	if ((res = pw_context_parse_conf_section(this, conf, "context.modules")) < 0)
		goto error_free;
	if (res > 0 || pw_properties_get_bool(properties, "context.modules.allow-empty", false))
		pw_log_info("%p: parsed %d context.modules items", this, res);
	else
		pw_log_warn("%p: no modules loaded from context.modules", this);
	if ((res = pw_context_parse_conf_section(this, conf, "context.objects")) < 0)
		goto error_free;
	pw_log_info("%p: parsed %d context.objects items", this, res);
	if ((res = pw_context_parse_conf_section(this, conf, "context.exec")) < 0)
		goto error_free;
	pw_log_info("%p: parsed %d context.exec items", this, res);

	for (i = 0; i < impl->n_data_loops; i++) {
		struct data_loop *dl = &impl->data_loops[i];
		if (!dl->autostart)
			continue;
		if ((res = data_loop_start(impl, dl)) < 0)
			goto error_free;
	}

	pw_settings_expose(this);

	pw_log_debug("%p: created", this);

	return this;

error_free:
	pw_context_destroy(this);
error_cleanup:
	errno = -res;
	return NULL;
}

/** Destroy a context object
 *
 * \param context a context to destroy
 */
SPA_EXPORT
void pw_context_destroy(struct pw_context *context)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	struct pw_global *global;
	struct pw_impl_client *client;
	struct pw_impl_module *module;
	struct pw_impl_device *device;
	struct pw_core *core;
	struct pw_resource *resource;
	struct pw_impl_node *node;
	struct factory_entry *entry;
	struct pw_impl_metadata *metadata;
	struct pw_impl_core *core_impl;
	uint32_t i;

	pw_log_debug("%p: destroy", context);
	pw_context_emit_destroy(context);

	spa_list_consume(core, &context->core_list, link)
		pw_core_disconnect(core);

	spa_list_consume(client, &context->client_list, link)
		pw_impl_client_destroy(client);

	spa_list_consume(node, &context->node_list, link)
		pw_impl_node_destroy(node);

	spa_list_consume(device, &context->device_list, link)
		pw_impl_device_destroy(device);

	spa_list_consume(resource, &context->registry_resource_list, link)
		pw_resource_destroy(resource);

	for (i = 0; i < impl->n_data_loops; i++)
		data_loop_stop(impl, &impl->data_loops[i]);

	spa_list_consume(module, &context->module_list, link)
		pw_impl_module_destroy(module);

	spa_list_consume(global, &context->global_list, link)
		pw_global_destroy(global);

	spa_list_consume(metadata, &context->metadata_list, link)
		pw_impl_metadata_destroy(metadata);

	spa_list_consume(core_impl, &context->core_impl_list, link)
		pw_impl_core_destroy(core_impl);

	pw_log_debug("%p: free", context);
	pw_context_emit_free(context);

	for (i = 0; i < impl->n_data_loops; i++) {
		if (impl->data_loops[i].impl)
			pw_data_loop_destroy(impl->data_loops[i].impl);

	}

	if (context->pool)
		pw_mempool_destroy(context->pool);

	if (context->work_queue)
		pw_work_queue_destroy(context->work_queue);
	if (context->timer_queue)
		pw_timer_queue_destroy(context->timer_queue);

	pw_properties_free(context->properties);
	pw_properties_free(context->conf);

	pw_settings_clean(context);

	if (impl->dbus_handle)
		pw_unload_spa_handle(impl->dbus_handle);

	pw_array_for_each(entry, &context->factory_lib) {
		regfree(&entry->regex);
		free(entry->lib);
	}
	pw_array_clear(&context->factory_lib);

	pw_array_clear(&context->objects);

	pw_map_clear(&context->globals);

	spa_hook_list_clean(&context->listener_list);
	spa_hook_list_clean(&context->driver_listener_list);

	free(context);
}

SPA_EXPORT
void *pw_context_get_user_data(struct pw_context *context)
{
	return context->user_data;
}

SPA_EXPORT
void pw_context_add_listener(struct pw_context *context,
			  struct spa_hook *listener,
			  const struct pw_context_events *events,
			  void *data)
{
	spa_hook_list_append(&context->listener_list, listener, events, data);
}

const struct spa_support *context_get_support(struct pw_context *context, uint32_t *n_support,
		const struct spa_dict *info)
{
	uint32_t n = context->n_support;
	struct pw_loop *loop;

	loop = pw_context_acquire_loop(context, info);
	if (loop != NULL) {
		context->support[n++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataSystem, loop->system);
		context->support[n++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, loop->loop);
	}
	*n_support = n;
	return context->support;
}

SPA_EXPORT
const struct spa_support *pw_context_get_support(struct pw_context *context, uint32_t *n_support)
{
	return context_get_support(context, n_support, NULL);
}

SPA_EXPORT
struct pw_loop *pw_context_get_main_loop(struct pw_context *context)
{
	return context->main_loop;
}

static struct pw_data_loop *acquire_data_loop(struct impl *impl, const char *name, const char *klass)
{
	uint32_t i, j;
	struct data_loop *best_loop = NULL;
	int best_score = 0, res;

	for (i = 0; i < impl->n_data_loops; i++) {
		struct data_loop *l = &impl->data_loops[i];
		const char *ln = l->impl->loop->name;
		int score = 0;

		if (klass == NULL)
			klass = l->impl->class;

		if (name && ln && fnmatch(name, ln, FNM_EXTMATCH) == 0)
			score += 2;
		if (klass && l->impl->classes) {
			for (j = 0; l->impl->classes[j]; j++) {
				if (fnmatch(klass, l->impl->classes[j], FNM_EXTMATCH) == 0) {
					score += 1;
					break;
				}
			}
		}

		pw_log_debug("%d: name:'%s' class:'%s' score:%d last_used:%"PRIu64, i,
				ln, l->impl->class, score, l->last_used);

		if ((best_loop == NULL) ||
		    (score > best_score) ||
		    (score == best_score && l->last_used < best_loop->last_used)) {
			best_loop = l;
			best_score = score;
		}
	}
	if (best_loop == NULL)
		return NULL;

	best_loop->last_used = get_time_ns(impl->this.main_loop->system);
	if ((res = data_loop_start(impl, best_loop)) < 0) {
		errno = -res;
		return NULL;
	}

	pw_log_info("%p: using name:'%s' class:'%s' last_used:%"PRIu64, impl,
			best_loop->impl->loop->name,
			best_loop->impl->class, best_loop->last_used);

	return best_loop->impl;
}

SPA_EXPORT
struct pw_data_loop *pw_context_get_data_loop(struct pw_context *context)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	return acquire_data_loop(impl, NULL, NULL);
}

SPA_EXPORT
struct pw_loop *pw_context_acquire_loop(struct pw_context *context, const struct spa_dict *props)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	const char *name, *klass;
	struct pw_data_loop *loop;

	name = props ? spa_dict_lookup(props, PW_KEY_NODE_LOOP_NAME) : NULL;
	klass = props ? spa_dict_lookup(props, PW_KEY_NODE_LOOP_CLASS) : NULL;

	pw_log_info("%p: looking for name:'%s' class:'%s'", context, name, klass);

	if ((impl->n_data_loops == 0) ||
	    (name && fnmatch(name, context->main_loop->name, FNM_EXTMATCH) == 0) ||
	    (klass && fnmatch(klass, "main", FNM_EXTMATCH) == 0)) {
		pw_log_info("%p: using main loop num-data-loops:%d", context, impl->n_data_loops);
		return context->main_loop;
	}

	loop = acquire_data_loop(impl, name, klass);
	return loop ? loop->loop : NULL;
}

SPA_EXPORT
void pw_context_release_loop(struct pw_context *context, struct pw_loop *loop)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	uint32_t i;

	for (i = 0; i < impl->n_data_loops; i++) {
		struct data_loop *l = &impl->data_loops[i];
		if (l->impl->loop == loop) {
			pw_log_info("release name:'%s' class:'%s' last_used:%"PRIu64,
					l->impl->loop->name, l->impl->class, l->last_used);
			return;
		}
	}
}

SPA_EXPORT
struct pw_work_queue *pw_context_get_work_queue(struct pw_context *context)
{
	return context->work_queue;
}

SPA_EXPORT
struct pw_timer_queue *pw_context_get_timer_queue(struct pw_context *context)
{
	if (context->timer_queue == NULL)
		context->timer_queue =  pw_timer_queue_new(context->main_loop);
	return context->timer_queue;
}

SPA_EXPORT
struct pw_mempool *pw_context_get_mempool(struct pw_context *context)
{
	return context->pool;
}

SPA_EXPORT
const struct pw_properties *pw_context_get_properties(struct pw_context *context)
{
	return context->properties;
}

SPA_EXPORT
const char *pw_context_get_conf_section(struct pw_context *context, const char *section)
{
	return pw_properties_get(context->conf, section);
}

/** Update context properties
 *
 * \param context a context
 * \param dict properties to update
 *
 * Update the context object with the given properties
 */
SPA_EXPORT
int pw_context_update_properties(struct pw_context *context, const struct spa_dict *dict)
{
	int changed;

	changed = pw_properties_update(context->properties, dict);
	pw_log_debug("%p: updated %d properties", context, changed);

	return changed;
}

static bool global_can_read(struct pw_context *context, struct pw_global *global)
{
	if (context->current_client &&
	    !PW_PERM_IS_R(pw_global_get_permissions(global, context->current_client)))
		return false;
	return true;
}

static bool global_is_stale(struct pw_context *context, struct pw_global *global)
{
	struct pw_impl_client *client = context->current_client;

	if (!client)
		return false;

	if (client->recv_generation != 0 && global->generation > client->recv_generation)
		return true;

	return false;
}

SPA_EXPORT
int pw_context_for_each_global(struct pw_context *context,
			    int (*callback) (void *data, struct pw_global *global),
			    void *data)
{
	struct pw_global *g, *t;
	int res;

	spa_list_for_each_safe(g, t, &context->global_list, link) {
		if (!global_can_read(context, g) || global_is_stale(context, g))
			continue;
		if ((res = callback(data, g)) != 0)
			return res;
	}
	return 0;
}

SPA_EXPORT
struct pw_global *pw_context_find_global(struct pw_context *context, uint32_t id)
{
	struct pw_global *global;

	global = pw_map_lookup(&context->globals, id);
	if (global == NULL || !global->registered) {
		errno = ENOENT;
		return NULL;
	}

	if (global_is_stale(context, global)) {
		errno = global_can_read(context, global) ? ESTALE : ENOENT;
		return NULL;
	}

	if (!global_can_read(context, global)) {
		errno = EACCES;
		return NULL;
	}
	return global;
}

SPA_PRINTF_FUNC(7, 8) int pw_context_debug_port_params(struct pw_context *this,
		struct spa_node *node, enum spa_direction direction,
		uint32_t port_id, uint32_t id, int err, const char *debug, ...)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	uint32_t state;
	struct spa_pod *param;
	int res;
	va_list args;

	va_start(args, debug);
	vsnprintf((char*)buffer, sizeof(buffer), debug, args);
	va_end(args);

	pw_log_error("params %s: %d:%d %s (%s)",
			spa_debug_type_find_name(spa_type_param, id),
			direction, port_id, spa_strerror(err), buffer);

	if (err == -EBUSY)
		return 0;

        state = 0;
        while (true) {
                spa_pod_builder_init(&b, buffer, sizeof(buffer));
                res = spa_node_port_enum_params_sync(node,
                                       direction, port_id,
                                       id, &state,
                                       NULL, &param, &b);
                if (res != 1) {
			if (res < 0)
				pw_log_error("  error: %s", spa_strerror(res));
                        break;
		}
                pw_log_pod(SPA_LOG_LEVEL_ERROR, param);
        }
        return 0;
}

int pw_context_recalc_graph(struct pw_context *context, const char *reason)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);

	pw_log_info("%p: busy:%d reason:%s", context, impl->recalc, reason);

	if (impl->recalc) {
		impl->recalc_pending = true;
		return -EBUSY;
	}

again:
	impl->recalc = true;

	pw_context_emit_recalc_graph(context);

	impl->recalc = false;
	if (impl->recalc_pending) {
		impl->recalc_pending = false;
		goto again;
	}
	return 0;
}

SPA_EXPORT
int pw_context_add_spa_lib(struct pw_context *context,
		const char *factory_regexp, const char *lib)
{
	struct factory_entry *entry;
	int err;

	entry = pw_array_add(&context->factory_lib, sizeof(*entry));
	if (entry == NULL)
		return -errno;

	if ((err = regcomp(&entry->regex, factory_regexp, REG_EXTENDED | REG_NOSUB)) != 0) {
		char errbuf[1024];
		regerror(err, &entry->regex, errbuf, sizeof(errbuf));
		pw_log_error("%p: can compile regex: %s", context, errbuf);
		pw_array_remove(&context->factory_lib, entry);
		return -EINVAL;
	}

	entry->lib = strdup(lib);
	pw_log_debug("%p: map factory regex '%s' to '%s", context,
			factory_regexp, lib);
	return 0;
}

SPA_EXPORT
const char *pw_context_find_spa_lib(struct pw_context *context, const char *factory_name)
{
	struct factory_entry *entry;

	pw_array_for_each(entry, &context->factory_lib) {
		if (regexec(&entry->regex, factory_name, 0, NULL, 0) == 0)
			return entry->lib;
	}
	return NULL;
}

SPA_EXPORT
struct spa_handle *pw_context_load_spa_handle(struct pw_context *context,
		const char *factory_name,
		const struct spa_dict *info)
{
	const char *lib;
	const struct spa_support *support;
	uint32_t n_support;
	struct spa_handle *handle;

	pw_log_debug("%p: load factory %s", context, factory_name);

	lib = pw_context_find_spa_lib(context, factory_name);
	if (lib == NULL && info != NULL)
		lib = spa_dict_lookup(info, SPA_KEY_LIBRARY_NAME);
	if (lib == NULL) {
		errno = ENOENT;
		pw_log_warn("%p: no library for %s: %m",
				context, factory_name);
		return NULL;
	}

	support = context_get_support(context, &n_support, info);

	handle = pw_load_spa_handle(lib, factory_name,
			info, n_support, support);

	return handle;
}

SPA_EXPORT
int pw_context_register_export_type(struct pw_context *context, struct pw_export_type *type)
{
	if (pw_context_find_export_type(context, type->type)) {
		pw_log_warn("context %p: duplicate export type %s", context, type->type);
		return -EEXIST;
	}
	pw_log_debug("context %p: Add export type %s to context", context, type->type);
	spa_list_append(&context->export_list, &type->link);
	return 0;
}

SPA_EXPORT
const struct pw_export_type *pw_context_find_export_type(struct pw_context *context, const char *type)
{
	const struct pw_export_type *t;
	spa_list_for_each(t, &context->export_list, link) {
		if (spa_streq(t->type, type))
			return t;
	}
	return NULL;
}

struct object_entry {
	const char *type;
	void *value;
};

static struct object_entry *find_object(struct pw_context *context, const char *type)
{
	struct object_entry *entry;
	pw_array_for_each(entry, &context->objects) {
		if (spa_streq(entry->type, type))
			return entry;
	}
	return NULL;
}

SPA_EXPORT
int pw_context_set_object(struct pw_context *context, const char *type, void *value)
{
	struct object_entry *entry;
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);

	entry = find_object(context, type);

	if (value == NULL) {
		if (entry)
			pw_array_remove(&context->objects, entry);
	} else {
		if (entry == NULL) {
			entry = pw_array_add(&context->objects, sizeof(*entry));
			if (entry == NULL)
				return -errno;
			entry->type = type;
		}
		entry->value = value;
	}
	if (spa_streq(type, SPA_TYPE_INTERFACE_ThreadUtils)) {
		uint32_t i;

		context->thread_utils = value;

		for (i = 0; i < impl->n_data_loops; i++) {
			if (impl->data_loops[i].impl)
				pw_data_loop_set_thread_utils(impl->data_loops[i].impl,
						context->thread_utils);
		}
	}
	return 0;
}

SPA_EXPORT
void *pw_context_get_object(struct pw_context *context, const char *type)
{
	struct object_entry *entry;

	if ((entry = find_object(context, type)) != NULL)
		return entry->value;

	return NULL;
}
