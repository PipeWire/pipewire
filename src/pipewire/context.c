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

#define MAX_HOPS	64
#define MAX_SYNC	4u
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

static int context_set_freewheel(struct pw_context *context, bool freewheel)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	struct spa_thread *thr;
	uint32_t i;
	int res = 0;

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

/** Find a common format between two ports
 *
 * \param context a context object
 * \param output an output port
 * \param input an input port
 * \param props extra properties
 * \param n_format_filters number of format filters
 * \param format_filters array of format filters
 * \param[out] format the common format between the ports
 * \param builder builder to use for processing
 * \param[out] error an error when something is wrong
 * \return a common format of NULL on error
 *
 * Find a common format between the given ports. The format will
 * be restricted to a subset given with the format filters.
 */
int pw_context_find_format(struct pw_context *context,
			struct pw_impl_port *output,
			uint32_t output_mix,
			struct pw_impl_port *input,
			uint32_t input_mix,
			struct pw_properties *props,
			uint32_t n_format_filters,
			struct spa_pod **format_filters,
			struct spa_pod **format,
			struct spa_pod_builder *builder,
			char **error)
{
	uint32_t out_state, in_state;
	int res;
	uint32_t iidx = 0, oidx = 0;
	struct spa_pod_builder fb = { 0 };
	uint8_t fbuf[4096];
	struct spa_pod *filter;
	struct spa_node *in_node, *out_node;
	uint32_t in_port, out_port;

	out_state = output->state;
	in_state = input->state;

	if (output_mix == SPA_ID_INVALID) {
		out_node = output->node->node;
		out_port = output->port_id;
	} else {
		out_node = output->mix;
		out_port = output_mix;
	}
	if (input_mix == SPA_ID_INVALID) {
		in_node = input->node->node;
		in_port = input->port_id;
	} else {
		in_node = input->mix;
		in_port = input_mix;
	}

	pw_log_debug("%p: finding best format %d %d", context, out_state, in_state);

	/* when a port is configured but the node is idle, we can reconfigure with a different format */
	if (out_state > PW_IMPL_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE)
		out_state = PW_IMPL_PORT_STATE_CONFIGURE;
	if (in_state > PW_IMPL_PORT_STATE_CONFIGURE && input->node->info.state == PW_NODE_STATE_IDLE)
		in_state = PW_IMPL_PORT_STATE_CONFIGURE;

	pw_log_debug("%p: states %d %d", context, out_state, in_state);

	if (in_state == PW_IMPL_PORT_STATE_CONFIGURE && out_state > PW_IMPL_PORT_STATE_CONFIGURE) {
		/* only input needs format */
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(out_node,
						     output->direction, out_port,
						     SPA_PARAM_Format, &oidx,
						     NULL, &filter, &fb)) != 1) {
			if (res < 0)
				*error = spa_aprintf("error get output format: %s", spa_strerror(res));
			else
				*error = spa_aprintf("no output formats");
			goto error;
		}
		pw_log_debug("%p: Got output format:", context);
		pw_log_format(SPA_LOG_LEVEL_DEBUG, filter);

		if ((res = spa_node_port_enum_params_sync(in_node,
						     input->direction, in_port,
						     SPA_PARAM_EnumFormat, &iidx,
						     filter, format, builder)) <= 0) {
			if (res == -ENOENT || res == 0) {
				pw_log_debug("%p: no input format filter, using output format: %s",
						context, spa_strerror(res));

				uint32_t offset = builder->state.offset;
				res = spa_pod_builder_raw_padded(builder, filter, SPA_POD_SIZE(filter));
				if (res < 0) {
					*error = spa_aprintf("failed to add pod");
					goto error;
				}

				*format = spa_pod_builder_deref(builder, offset);
			} else {
				*error = spa_aprintf("error input enum formats: %s", spa_strerror(res));
				goto error;
			}
		}
	} else if (out_state >= PW_IMPL_PORT_STATE_CONFIGURE && in_state > PW_IMPL_PORT_STATE_CONFIGURE) {
		/* only output needs format */
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(in_node,
						     input->direction, in_port,
						     SPA_PARAM_Format, &iidx,
						     NULL, &filter, &fb)) != 1) {
			if (res < 0)
				*error = spa_aprintf("error get input format: %s", spa_strerror(res));
			else
				*error = spa_aprintf("no input format");
			goto error;
		}
		pw_log_debug("%p: Got input format:", context);
		pw_log_format(SPA_LOG_LEVEL_DEBUG, filter);

		if ((res = spa_node_port_enum_params_sync(out_node,
						     output->direction, out_port,
						     SPA_PARAM_EnumFormat, &oidx,
						     filter, format, builder)) <= 0) {
			if (res == -ENOENT || res == 0) {
				pw_log_debug("%p: no output format filter, using input format: %s",
						context, spa_strerror(res));

				uint32_t offset = builder->state.offset;
				res = spa_pod_builder_raw_padded(builder, filter, SPA_POD_SIZE(filter));
				if (res < 0) {
					*error = spa_aprintf("failed to add pod");
					goto error;
				}

				*format = spa_pod_builder_deref(builder, offset);
			} else {
				*error = spa_aprintf("error output enum formats: %s", spa_strerror(res));
				goto error;
			}
		}
	} else if (in_state == PW_IMPL_PORT_STATE_CONFIGURE && out_state == PW_IMPL_PORT_STATE_CONFIGURE) {
	      again:
		/* both ports need a format */
		pw_log_debug("%p: do enum input %d", context, iidx);
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(in_node,
						     input->direction, in_port,
						     SPA_PARAM_EnumFormat, &iidx,
						     NULL, &filter, &fb)) != 1) {
			if (res == -ENOENT) {
				pw_log_debug("%p: no input filter", context);
				filter = NULL;
			} else {
				if (res < 0)
					*error = spa_aprintf("error input enum formats: %s", spa_strerror(res));
				else
					*error = spa_aprintf("no more input formats");
				goto error;
			}
		}
		pw_log_debug("%p: enum output %d with filter: %p", context, oidx, filter);
		pw_log_format(SPA_LOG_LEVEL_DEBUG, filter);

		if ((res = spa_node_port_enum_params_sync(out_node,
						     output->direction, out_port,
						     SPA_PARAM_EnumFormat, &oidx,
						     filter, format, builder)) != 1) {
			if (res == 0 && filter != NULL) {
				oidx = 0;
				goto again;
			}
			*error = spa_aprintf("error output enum formats: %s", spa_strerror(res));
			goto error;
		}

		pw_log_debug("%p: Got filtered:", context);
		pw_log_format(SPA_LOG_LEVEL_DEBUG, *format);
	} else {
		res = -EBADF;
		*error = spa_aprintf("error bad node state");
		goto error;
	}
	return res;
error:
	if (res == 0)
		res = -EINVAL;
	return res;
}

static int ensure_state(struct pw_impl_node *node, bool running)
{
	enum pw_node_state state = node->info.state;
	if (node->active && node->runnable &&
	    !SPA_FLAG_IS_SET(node->spa_flags, SPA_NODE_FLAG_NEED_CONFIGURE) && running)
		state = PW_NODE_STATE_RUNNING;
	else if (state > PW_NODE_STATE_IDLE)
		state = PW_NODE_STATE_IDLE;
	return pw_impl_node_set_state(node, state);
}

/* From a node (that is runnable) follow all prepared links in the given direction
 * and groups to active nodes and make them recursively runnable as well.
 */
static inline int run_nodes(struct pw_context *context, struct pw_impl_node *node,
		struct spa_list *nodes, enum pw_direction direction, int hop)
{
	struct pw_impl_node *t;
	struct pw_impl_port *p;
	struct pw_impl_link *l;

	if (hop == MAX_HOPS) {
		pw_log_warn("exceeded hops (%d)", hop);
		return -EIO;
	}

	pw_log_debug("node %p: '%s' direction:%s", node, node->name,
			pw_direction_as_string(direction));

	SPA_FLAG_SET(node->checked, 1u<<direction);

	if (direction == PW_DIRECTION_INPUT) {
		spa_list_for_each(p, &node->input_ports, link) {
			spa_list_for_each(l, &p->links, input_link) {
				t = l->output->node;

				if (!t->active || !l->prepared ||
				    (!t->driving && SPA_FLAG_IS_SET(t->checked, 1u<<direction)))
					continue;
				if (t->driving && p->node == t)
					continue;

				pw_log_debug("  peer %p: '%s'", t, t->name);
				t->runnable = true;
				run_nodes(context, t, nodes, direction, hop + 1);
			}
		}
	} else {
		spa_list_for_each(p, &node->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				t = l->input->node;

				if (!t->active || !l->prepared ||
				    (!t->driving && SPA_FLAG_IS_SET(t->checked, 1u<<direction)))
					continue;
				if (t->driving && p->node == t)
					continue;

				pw_log_debug("  peer %p: '%s'", t, t->name);
				t->runnable = true;
				run_nodes(context, t, nodes, direction, hop + 1);
			}
		}
	}
	/* now go through all the nodes that have the same link group and
	 * that are not yet visited. Note how nodes with the same group
	 * don't get included here. They were added to the same driver but
	 * need to otherwise stay idle unless some non-passive link activates
	 * them. */
	if (node->link_groups != NULL) {
		spa_list_for_each(t, nodes, sort_link) {
			if (t->exported || !t->active ||
			    SPA_FLAG_IS_SET(t->checked, 1u<<direction))
				continue;
			if (pw_strv_find_common(t->link_groups, node->link_groups) < 0)
				continue;

			pw_log_debug("  group %p: '%s'", t, t->name);
			t->runnable = true;
			if (!t->driving)
				run_nodes(context, t, nodes, direction, hop + 1);
		}
	}
	return 0;
}

/* Follow all prepared links and groups from node, activate the links.
 * If a non-passive link is found, we set the peer runnable flag.
 *
 * After this is done, we end up with a list of nodes in collect that are all
 * linked to node.
 * Some of the nodes have the runnable flag set. We then start from those nodes
 * and make all linked nodes and groups runnable as well. (see run_nodes).
 *
 * This ensures that we only activate the paths from the runnable nodes to the
 * driver nodes and leave the other nodes idle.
 */
static int collect_nodes(struct pw_context *context, struct pw_impl_node *node, struct spa_list *collect)
{
	struct spa_list queue;
	struct pw_impl_node *n, *t;
	struct pw_impl_port *p;
	struct pw_impl_link *l;
	uint32_t n_sync;
	char *sync[MAX_SYNC+1];

	pw_log_debug("node %p: '%s'", node, node->name);

	/* start with node in the queue */
	spa_list_init(&queue);
	spa_list_append(&queue, &node->sort_link);
	node->visited = true;

	n_sync = 0;
	sync[0] = NULL;

	/* now follow all the links from the nodes in the queue
	 * and add the peers to the queue. */
	spa_list_consume(n, &queue, sort_link) {
		spa_list_remove(&n->sort_link);
		spa_list_append(collect, &n->sort_link);

		pw_log_debug(" next node %p: '%s' runnable:%u active:%d",
				n, n->name, n->runnable, n->active);

		if (!n->active)
			continue;

		if (n->sync) {
			for (uint32_t i = 0; n->sync_groups[i]; i++) {
				if (n_sync >= MAX_SYNC)
					break;
				if (pw_strv_find(sync, n->sync_groups[i]) >= 0)
					continue;
				sync[n_sync++] = n->sync_groups[i];
				sync[n_sync] = NULL;
			}
		}

		spa_list_for_each(p, &n->input_ports, link) {
			spa_list_for_each(l, &p->links, input_link) {
				t = l->output->node;

				if (!t->active)
					continue;

				pw_impl_link_prepare(l);

				if (!l->prepared)
					continue;

				if (!l->passive)
					t->runnable = true;

				if (!t->visited) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
		spa_list_for_each(p, &n->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				t = l->input->node;

				if (!t->active)
					continue;

				pw_impl_link_prepare(l);

				if (!l->prepared)
					continue;

				if (!l->passive)
					t->runnable = true;

				if (!t->visited) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
		/* now go through all the nodes that have the same group and
		 * that are not yet visited */
		if (n->groups != NULL || n->link_groups != NULL || sync[0] != NULL) {
			spa_list_for_each(t, &context->node_list, link) {
				if (t->exported || !t->active || t->visited)
					continue;
				/* the other node will be scheduled with this one if it's in
				 * the same group or link group */
				if (pw_strv_find_common(t->groups, n->groups) < 0 &&
				    pw_strv_find_common(t->link_groups, n->link_groups) < 0 &&
				    pw_strv_find_common(t->sync_groups, sync) < 0)
					continue;

				pw_log_debug("%p: %s join group of %s",
						t, t->name, n->name);
				t->visited = true;
				spa_list_append(&queue, &t->sort_link);
			}
		}
		pw_log_debug(" next node %p: '%s' runnable:%u %p %p %p", n, n->name, n->runnable,
				n->groups, n->link_groups, sync);
	}
	spa_list_for_each(n, collect, sort_link)
		if (!n->driving && n->runnable) {
			run_nodes(context, n, collect, PW_DIRECTION_OUTPUT, 0);
			run_nodes(context, n, collect, PW_DIRECTION_INPUT, 0);
		}

	return 0;
}

static void move_to_driver(struct pw_context *context, struct spa_list *nodes,
		struct pw_impl_node *driver)
{
	struct pw_impl_node *n;
	pw_log_debug("driver: %p %s runnable:%u", driver, driver->name, driver->runnable);
	spa_list_consume(n, nodes, sort_link) {
		spa_list_remove(&n->sort_link);

		driver->runnable |= n->runnable;

		pw_log_debug(" follower: %p %s runnable:%u driver-runnable:%u", n, n->name,
				n->runnable, driver->runnable);
		pw_impl_node_set_driver(n, driver);
	}
}
static void remove_from_driver(struct pw_context *context, struct spa_list *nodes)
{
	struct pw_impl_node *n;
	spa_list_consume(n, nodes, sort_link) {
		spa_list_remove(&n->sort_link);
		pw_impl_node_set_driver(n, NULL);
		ensure_state(n, false);
	}
}

static inline void get_quantums(struct pw_context *context, uint32_t *def,
		uint32_t *min, uint32_t *max, uint32_t *rate, uint32_t *floor, uint32_t *ceil)
{
	struct settings *s = &context->settings;
	if (s->clock_force_quantum != 0) {
		*def = *min = *max = s->clock_force_quantum;
		*rate = 0;
	} else {
		*def = s->clock_quantum;
		*min = s->clock_min_quantum;
		*max = s->clock_max_quantum;
		*rate = s->clock_rate;
	}
	*floor = s->clock_quantum_floor;
	*ceil = s->clock_quantum_limit;
}

static inline const uint32_t *get_rates(struct pw_context *context, uint32_t *def, uint32_t *n_rates,
		bool *force)
{
	struct settings *s = &context->settings;
	if (s->clock_force_rate != 0) {
		*force = true;
		*n_rates = 1;
		*def = s->clock_force_rate;
		return &s->clock_force_rate;
	} else {
		*force = false;
		*n_rates = s->n_clock_rates;
		*def = s->clock_rate;
		return s->clock_rates;
	}
}
static void reconfigure_driver(struct pw_context *context, struct pw_impl_node *n)
{
	struct pw_impl_node *s;

	spa_list_for_each(s, &n->follower_list, follower_link) {
		if (s == n)
			continue;
		pw_log_debug("%p: follower %p: '%s' suspend",
				context, s, s->name);
		pw_impl_node_set_state(s, PW_NODE_STATE_SUSPENDED);
	}
	pw_log_debug("%p: driver %p: '%s' suspend",
			context, n, n->name);

	if (n->info.state >= PW_NODE_STATE_IDLE)
		n->need_resume = !n->pause_on_idle;
	pw_impl_node_set_state(n, PW_NODE_STATE_SUSPENDED);
}

/* find smaller power of 2 */
static uint32_t flp2(uint32_t x)
{
	x = x | (x >> 1);
	x = x | (x >> 2);
	x = x | (x >> 4);
	x = x | (x >> 8);
	x = x | (x >> 16);
	return x - (x >> 1);
}

/* cmp fractions, avoiding overflows */
static int fraction_compare(const struct spa_fraction *a, const struct spa_fraction *b)
{
	uint64_t fa = (uint64_t)a->num * (uint64_t)b->denom;
	uint64_t fb = (uint64_t)b->num * (uint64_t)a->denom;
	return fa < fb ? -1 : (fa > fb ? 1 : 0);
}

static inline uint32_t calc_gcd(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t temp = a;
		a = b;
		b = temp % b;
	}
	return a;
}

struct rate_info {
	uint32_t rate;
	uint32_t gcd;
	uint32_t diff;
};

static inline void update_highest_rate(struct rate_info *best, struct rate_info *current)
{
	/* find highest rate */
	if (best->rate == 0 || best->rate < current->rate)
		*best = *current;
}

static inline void update_nearest_gcd(struct rate_info *best, struct rate_info *current)
{
	/* find nearest GCD */
	if (best->rate == 0 ||
	    (best->gcd < current->gcd) ||
	    (best->gcd == current->gcd && best->diff > current->diff))
		*best = *current;
}
static inline void update_nearest_rate(struct rate_info *best, struct rate_info *current)
{
	/* find nearest rate */
	if (best->rate == 0 || best->diff > current->diff)
		*best = *current;
}

static uint32_t find_best_rate(const uint32_t *rates, uint32_t n_rates, uint32_t rate, uint32_t def)
{
	uint32_t i, limit;
	struct rate_info best;
	struct rate_info info[n_rates];

	for (i = 0; i < n_rates; i++) {
		info[i].rate = rates[i];
		info[i].gcd = calc_gcd(rate, rates[i]);
		info[i].diff = SPA_ABS((int32_t)rate - (int32_t)rates[i]);
	}

	/* first find higher nearest GCD. This tries to find next bigest rate that
	 * requires the least amount of resample filter banks. Usually these are
	 * rates that are multiples of each other or multiples of a common rate.
	 *
	 * 44100 and [ 32000 56000 88200 96000 ]  -> 88200
	 * 48000 and [ 32000 56000 88200 96000 ]  -> 96000
	 * 88200 and [ 44100 48000 96000 192000 ]  -> 96000
	 * 32000 and [ 44100 192000 ] -> 44100
	 * 8000 and [ 44100 48000 ] -> 48000
	 * 8000 and [ 44100 192000 ] -> 44100
	 * 11025 and [ 44100 48000 ] -> 44100
	 * 44100 and [ 48000 176400 ] -> 48000
	 * 144 and [ 44100 48000 88200 96000] -> 48000
	 */
	spa_zero(best);
	/* Don't try to do excessive upsampling by limiting the max rate
	 * for desired < default to default*2. For other rates allow
	 * a x3 upsample rate max. For values lower than half of the default,
	 * limit to the default.  */
	limit = rate < def/2 ? def : rate < def ? def*2 : rate*3;
	for (i = 0; i < n_rates; i++) {
		if (info[i].rate >= rate && info[i].rate <= limit)
			update_nearest_gcd(&best, &info[i]);
	}
	if (best.rate != 0)
		return best.rate;

	/* we would need excessive upsampling, pick a nearest higher rate */
	spa_zero(best);
	for (i = 0; i < n_rates; i++) {
		if (info[i].rate >= rate)
			update_nearest_rate(&best, &info[i]);
	}
	if (best.rate != 0)
		return best.rate;

	/* There is nothing above the rate, we need to downsample. Try to downsample
	 * but only to something that is from a common rate family. Also don't
	 * try to downsample to something that will sound worse (< 44100).
	 *
	 * 88200 and [ 22050 44100 48000 ] -> 44100
	 * 88200 and [ 22050 48000 ] -> 48000
	 */
	spa_zero(best);
	for (i = 0; i < n_rates; i++) {
		if (info[i].rate >= 44100)
			update_nearest_gcd(&best, &info[i]);
	}
	if (best.rate != 0)
		return best.rate;

	/* There is nothing to downsample above our threshold. Downsample to whatever
	 * is the highest rate then. */
	spa_zero(best);
	for (i = 0; i < n_rates; i++)
		update_highest_rate(&best, &info[i]);
	if (best.rate != 0)
		return best.rate;

	return def;
}

/* here we evaluate the complete state of the graph.
 *
 * It roughly operates in 3 stages:
 *
 * 1. go over all drivers and collect the nodes that need to be scheduled with the
 *    driver. This include all nodes that have an active link with the driver or
 *    with a node already scheduled with the driver.
 *
 * 2. go over all nodes that are not assigned to a driver. The ones that require
 *    a driver are moved to some random active driver found in step 1.
 *
 * 3. go over all drivers again, collect the quantum/rate of all followers, select
 *    the desired final value and activate the followers and then the driver.
 *
 * A complete graph evaluation is performed for each change that is made to the
 * graph, such as making/destroying links, adding/removing nodes, property changes such
 * as quantum/rate changes or metadata changes.
 */
int pw_context_recalc_graph(struct pw_context *context, const char *reason)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	struct settings *settings = &context->settings;
	struct pw_impl_node *n, *s, *target, *fallback;
	const uint32_t *rates;
	uint32_t max_quantum, min_quantum, def_quantum, rate_quantum, floor_quantum, ceil_quantum;
	uint32_t n_rates, def_rate, transport;
	bool freewheel, global_force_rate, global_force_quantum;
	struct spa_list collect;

	pw_log_info("%p: busy:%d reason:%s", context, impl->recalc, reason);

	if (impl->recalc) {
		impl->recalc_pending = true;
		return -EBUSY;
	}

again:
	impl->recalc = true;
	freewheel = false;

	/* clean up the flags first */
	spa_list_for_each(n, &context->node_list, link) {
		n->visited = false;
		n->checked = 0;
		n->runnable = n->always_process && n->active;
	}

	get_quantums(context, &def_quantum, &min_quantum, &max_quantum, &rate_quantum,
			&floor_quantum, &ceil_quantum);
	rates = get_rates(context, &def_rate, &n_rates, &global_force_rate);

	global_force_quantum = rate_quantum == 0;

	/* start from all drivers and group all nodes that are linked
	 * to it. Some nodes are not (yet) linked to anything and they
	 * will end up 'unassigned' to a driver. Other nodes are drivers
	 * and if they have active followers, we can use them to schedule
	 * the unassigned nodes. */
	target = fallback = NULL;
	spa_list_for_each(n, &context->driver_list, driver_link) {
		if (n->exported)
			continue;

		if (!n->visited) {
			spa_list_init(&collect);
			collect_nodes(context, n, &collect);
			move_to_driver(context, &collect, n);
		}
		/* from now on we are only interested in active driving nodes
		 * with a driver_priority. We're going to see if there are
		 * active followers. */
		if (!n->driving || !n->active || n->priority_driver <= 0)
			continue;

		/* first active driving node is fallback */
		if (fallback == NULL)
			fallback = n;

		if (!n->runnable)
			continue;

		spa_list_for_each(s, &n->follower_list, follower_link) {
			pw_log_debug("%p: driver %p: follower %p %s: active:%d",
					context, n, s, s->name, s->active);
			if (s != n && s->active) {
				/* if the driving node has active followers, it
				 * is a target for our unassigned nodes */
				if (target == NULL)
					target = n;
				if (n->freewheel)
					freewheel = true;
				break;
			}
		}
	}
	/* no active node, use fallback driving node */
	if (target == NULL)
		target = fallback;

	/* update the freewheel status */
	if (context->freewheeling != freewheel)
		context_set_freewheel(context, freewheel);

	/* now go through all available nodes. The ones we didn't visit
	 * in collect_nodes() are not linked to any driver. We assign them
	 * to either an active driver or the first driver if they are in a
	 * group that needs a driver. Else we remove them from a driver
	 * and stop them. */
	spa_list_for_each(n, &context->node_list, link) {
		struct pw_impl_node *t, *driver;

		if (n->exported || n->visited)
			continue;

		pw_log_debug("%p: unassigned node %p: '%s' active:%d want_driver:%d target:%p",
				context, n, n->name, n->active, n->want_driver, target);

		/* collect all nodes in this group */
		spa_list_init(&collect);
		collect_nodes(context, n, &collect);

		driver = NULL;
		spa_list_for_each(t, &collect, sort_link) {
			/* is any active and want a driver */
			if ((t->want_driver && t->active && t->runnable) ||
			    t->always_process) {
				driver = target;
				break;
			}
		}
		if (driver != NULL) {
			driver->runnable = true;
			/* driver needed for this group */
			move_to_driver(context, &collect, driver);
		} else {
			/* no driver, make sure the nodes stop */
			remove_from_driver(context, &collect);
		}
	}

	/* assign final quantum and set state for followers and drivers */
	spa_list_for_each(n, &context->driver_list, driver_link) {
		bool running = false, lock_quantum = false, lock_rate = false;
		struct spa_fraction latency = SPA_FRACTION(0, 0);
		struct spa_fraction max_latency = SPA_FRACTION(0, 0);
		struct spa_fraction rate = SPA_FRACTION(0, 0);
		uint32_t target_quantum, target_rate, current_rate, current_quantum;
		uint64_t quantum_stamp = 0, rate_stamp = 0;
		bool force_rate, force_quantum, restore_rate = false, restore_quantum = false;
		bool do_reconfigure = false, need_resume, was_target_pending;
		bool have_request = false;
		const uint32_t *node_rates;
		uint32_t node_n_rates, node_def_rate;
		uint32_t node_max_quantum, node_min_quantum, node_def_quantum, node_rate_quantum;

		if (!n->driving || n->exported)
			continue;

		node_def_quantum = def_quantum;
		node_min_quantum = min_quantum;
		node_max_quantum = max_quantum;
		node_rate_quantum = rate_quantum;
		force_quantum = global_force_quantum;

		node_def_rate = def_rate;
		node_n_rates = n_rates;
		node_rates = rates;
		force_rate = global_force_rate;

		/* collect quantum and rate */
		spa_list_for_each(s, &n->follower_list, follower_link) {

			if (!s->moved) {
				/* We only try to enforce the lock flags for nodes that
				 * are not recently moved between drivers. The nodes that
				 * are moved should try to enforce their quantum on the
				 * new driver. */
				lock_quantum |= s->lock_quantum;
				lock_rate |= s->lock_rate;
			}
			if (!global_force_quantum && s->force_quantum > 0 &&
			    s->stamp > quantum_stamp) {
				node_def_quantum = node_min_quantum = node_max_quantum = s->force_quantum;
				node_rate_quantum = 0;
				quantum_stamp = s->stamp;
				force_quantum = true;
			}
			if (!global_force_rate && s->force_rate > 0 &&
			    s->stamp > rate_stamp) {
				node_def_rate = s->force_rate;
				node_n_rates = 1;
				node_rates = &s->force_rate;
				force_rate = true;
				rate_stamp = s->stamp;
			}

			/* smallest latencies */
			if (latency.denom == 0 ||
			    (s->latency.denom > 0 &&
			     fraction_compare(&s->latency, &latency) < 0))
				latency = s->latency;
			if (max_latency.denom == 0 ||
			    (s->max_latency.denom > 0 &&
			     fraction_compare(&s->max_latency, &max_latency) < 0))
				max_latency = s->max_latency;

			/* largest rate, which is in fact the smallest fraction */
			if (rate.denom == 0 ||
			    (s->rate.denom > 0 &&
			     fraction_compare(&s->rate, &rate) < 0))
				rate = s->rate;

			if (s->active)
				running = n->runnable;

			pw_log_debug("%p: follower %p running:%d runnable:%d rate:%u/%u latency %u/%u '%s'",
				context, s, running, s->runnable, rate.num, rate.denom,
				latency.num, latency.denom, s->name);

			if (running && s != n && s->supports_request > 0)
				have_request = true;

			s->moved = false;
		}

		if (n->forced_rate && !force_rate && n->runnable) {
			/* A node that was forced to a rate but is no longer being
			 * forced can restore its rate */
			pw_log_info("(%s-%u) restore rate", n->name, n->info.id);
			restore_rate = true;
		}
		if (n->forced_quantum && !force_quantum && n->runnable) {
			/* A node that was forced to a quantum but is no longer being
			 * forced can restore its quantum */
			pw_log_info("(%s-%u) restore quantum", n->name, n->info.id);
			restore_quantum = true;
		}

		if (force_quantum)
			lock_quantum = false;
		if (force_rate)
			lock_rate = false;

		need_resume = n->need_resume;
		if (need_resume) {
			running = true;
			n->need_resume = false;
		}

		current_rate = n->target_rate.denom;
		if (!restore_rate &&
		   (lock_rate || need_resume || !running ||
		    (!force_rate && (n->info.state > PW_NODE_STATE_IDLE)))) {
			pw_log_debug("%p: keep rate:1/%u restore:%u lock:%u resume:%u "
					"running:%u force:%u state:%s", context,
					current_rate, restore_rate, lock_rate, need_resume,
					running, force_rate,
					pw_node_state_as_string(n->info.state));

			/* when we don't need to restore or rate and
			 * when someone wants us to lock the rate of this driver or
			 * when we are in the process of reconfiguring the driver or
			 * when we are not running any followers or
			 * when the driver is busy and we don't need to force a rate,
			 * keep the current rate */
			target_rate = current_rate;
		}
		else {
			/* Here we are allowed to change the rate of the driver.
			 * Start with the default rate. If the desired rate is
			 * allowed, switch to it */
			if (rate.denom != 0 && rate.num == 1)
				target_rate = rate.denom;
			else
				target_rate = node_def_rate;

			target_rate = find_best_rate(node_rates, node_n_rates,
						target_rate, node_def_rate);

			pw_log_debug("%p: def_rate:%d target_rate:%d rate:%d/%d", context,
					node_def_rate, target_rate, rate.num, rate.denom);
		}

		was_target_pending = n->target_pending;

		if (target_rate != current_rate) {
			/* we doing a rate switch */
			pw_log_info("(%s-%u) state:%s new rate:%u/(%u)->%u",
					n->name, n->info.id,
					pw_node_state_as_string(n->info.state),
					n->target_rate.denom, current_rate,
					target_rate);

			if (force_rate) {
				if (settings->clock_rate_update_mode == CLOCK_RATE_UPDATE_MODE_HARD)
					do_reconfigure |= !was_target_pending;
			} else {
				if (n->info.state >= PW_NODE_STATE_SUSPENDED)
					do_reconfigure |= !was_target_pending;
			}
			/* we're setting the pending rate. This will become the new
			 * current rate in the next iteration of the graph. */
			n->target_rate = SPA_FRACTION(1, target_rate);
			n->forced_rate = force_rate;
			n->target_pending = true;
			current_rate = target_rate;
		}

		if (node_rate_quantum != 0 && current_rate != node_rate_quantum) {
			/* the quantum values are scaled with the current rate */
			node_def_quantum = SPA_SCALE32(node_def_quantum, current_rate, node_rate_quantum);
			node_min_quantum = SPA_SCALE32(node_min_quantum, current_rate, node_rate_quantum);
			node_max_quantum = SPA_SCALE32(node_max_quantum, current_rate, node_rate_quantum);
		}

		/* calculate desired quantum. Don't limit to the max_latency when we are
		 * going to force a quantum or rate and reconfigure the nodes. */
		if (max_latency.denom != 0 && !force_quantum && !force_rate) {
			uint32_t tmp = SPA_SCALE32(max_latency.num, current_rate, max_latency.denom);
			if (tmp < node_max_quantum)
				node_max_quantum = tmp;
		}

		current_quantum = n->target_quantum;
		if (!restore_quantum && (lock_quantum || need_resume || !running)) {
			pw_log_debug("%p: keep quantum:%u restore:%u lock:%u resume:%u "
					"running:%u force:%u state:%s", context,
					current_quantum, restore_quantum, lock_quantum, need_resume,
					running, force_quantum,
					pw_node_state_as_string(n->info.state));
			target_quantum = current_quantum;
		}
		else {
			target_quantum = node_def_quantum;
			if (latency.denom != 0)
				target_quantum = SPA_SCALE32(latency.num, current_rate, latency.denom);
			target_quantum = SPA_CLAMP(target_quantum, node_min_quantum, node_max_quantum);
			target_quantum = SPA_CLAMP(target_quantum, floor_quantum, ceil_quantum);

			if (settings->clock_power_of_two_quantum && !force_quantum)
				target_quantum = flp2(target_quantum);
		}

		if (target_quantum != current_quantum) {
			pw_log_info("(%s-%u) new quantum:%"PRIu64"->%u",
					n->name, n->info.id,
					n->target_quantum,
					target_quantum);
			/* this is the new pending quantum */
			n->target_quantum = target_quantum;
			n->forced_quantum = force_quantum;
			n->target_pending = true;

			if (force_quantum)
				do_reconfigure |= !was_target_pending;
		}

		if (n->target_pending) {
			if (do_reconfigure) {
				reconfigure_driver(context, n);
				/* we might be suspended now and the links need to be prepared again */
				goto again;
			}
			/* we have a pending change. We place the new values in the
			 * pending fields so that they are picked up by the driver in
			 * the next cycle */
			pw_log_debug("%p: apply duration:%"PRIu64" rate:%u/%u", context,
					n->target_quantum, n->target_rate.num,
					n->target_rate.denom);
			SPA_SEQ_WRITE(n->rt.position->clock.target_seq);
			n->rt.position->clock.target_duration = n->target_quantum;
			n->rt.position->clock.target_rate = n->target_rate;
			SPA_SEQ_WRITE(n->rt.position->clock.target_seq);

			if (n->info.state < PW_NODE_STATE_RUNNING) {
				n->rt.position->clock.duration = n->target_quantum;
				n->rt.position->clock.rate = n->target_rate;
			}
			n->target_pending = false;
		} else {
			n->target_quantum = n->rt.position->clock.target_duration;
			n->target_rate = n->rt.position->clock.target_rate;
		}

		SPA_FLAG_UPDATE(n->rt.position->clock.flags,
				SPA_IO_CLOCK_FLAG_LAZY, have_request && n->supports_lazy > 0);

		pw_log_debug("%p: driver %p running:%d runnable:%d quantum:%u rate:%u (%"PRIu64"/%u)'%s'",
				context, n, running, n->runnable, target_quantum, target_rate,
				n->rt.position->clock.target_duration,
				n->rt.position->clock.target_rate.denom, n->name);

		transport = PW_NODE_ACTIVATION_COMMAND_NONE;

		/* first change the node states of the followers to the new target */
		spa_list_for_each(s, &n->follower_list, follower_link) {
			if (s->transport != PW_NODE_ACTIVATION_COMMAND_NONE) {
				transport = s->transport;
				s->transport = PW_NODE_ACTIVATION_COMMAND_NONE;
			}
			if (s == n)
				continue;
			pw_log_debug("%p: follower %p: active:%d '%s'",
					context, s, s->active, s->name);
			ensure_state(s, running);
		}

		if (transport != PW_NODE_ACTIVATION_COMMAND_NONE) {
			pw_log_info("%s: transport %d", n->name, transport);
			SPA_ATOMIC_STORE(n->rt.target.activation->command, transport);
		}

		/* now that all the followers are ready, start the driver */
		ensure_state(n, running);
	}
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
