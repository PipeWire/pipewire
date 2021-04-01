/* PipeWire
 *
 * Copyright © 2020 Georges Basile Stavracas Neto
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
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

struct module;

struct module_info {
	const char *name;
	struct module *(*create) (struct impl *impl, const char *args);
};

struct module_events {
#define VERSION_MODULE_EVENTS	0
	uint32_t version;

	void (*loaded) (void *data, int res);
};

#define module_emit_loaded(m,r) spa_hook_list_call(&m->hooks, struct module_events, loaded, 0, r)

struct module_methods {
#define VERSION_MODULE_METHODS	0
	uint32_t version;

	int (*load) (struct client *client, struct module *module);
	int (*unload) (struct client *client, struct module *module);
};

struct module {
	uint32_t idx;
	const char *name;
	const char *args;
	struct pw_properties *props;
	struct spa_list link;           /**< link in client modules */
	struct impl *impl;
	const struct module_methods *methods;
	struct spa_hook_list hooks;
	struct spa_source *unload;
	void *user_data;
};

static int module_unload(struct client *client, struct module *module);

static void on_module_unload(void *data, uint64_t count)
{
	struct module *module = data;

	module_unload(NULL, module);
}

static struct module *module_new(struct impl *impl, const struct module_methods *methods, size_t user_data)
{
	struct module *module;

	module = calloc(1, sizeof(struct module) + user_data);
	if (module == NULL)
		return NULL;

	module->impl = impl;
	module->methods = methods;
	spa_hook_list_init(&module->hooks);
	module->unload = pw_loop_add_event(impl->loop, on_module_unload, module);
	module->user_data = SPA_MEMBER(module, sizeof(struct module), void);

	return module;
}

static void module_add_listener(struct module *module,
		struct spa_hook *listener,
		const struct module_events *events, void *data)
{
	spa_hook_list_append(&module->hooks, listener, events, data);
}

static int module_load(struct client *client, struct module *module)
{
	pw_log_info("load module id:%u name:%s", module->idx, module->name);
	if (module->methods->load == NULL)
		return -ENOTSUP;
	/* subscription event is sent when the module does a
	 * module_emit_loaded() */
	return module->methods->load(client, module);
}

static void module_free(struct module *module)
{
	struct impl *impl = module->impl;
	if (module->idx != SPA_ID_INVALID)
		pw_map_remove(&impl->modules, module->idx & INDEX_MASK);

	free((char*)module->name);
	free((char*)module->args);
	if (module->props)
		pw_properties_free(module->props);
	pw_loop_destroy_source(impl->loop, module->unload);

	free(module);
}

static int module_unload(struct client *client, struct module *module)
{
	struct impl *impl = module->impl;
	uint32_t module_idx = module->idx;
	int res = 0;

	/* Note that client can be NULL (when the module is being unloaded
	 * internally and not by a client request */

	pw_log_info("unload module id:%u name:%s", module->idx, module->name);

	if (module->methods->unload)
		res = module->methods->unload(client, module);

	module_free(module);

	broadcast_subscribe_event(impl,
			SUBSCRIPTION_MASK_MODULE,
			SUBSCRIPTION_EVENT_REMOVE | SUBSCRIPTION_EVENT_MODULE,
			module_idx);

	return res;
}

/** utils */
static void add_props(struct pw_properties *props, const char *str)
{
	char *s = strdup(str), *p = s, *e, f;
	const char *k, *v;

	while (*p) {
		e = strchr(p, '=');
		if (e == NULL)
			break;
		*e = '\0';
		k = p;
		p = e+1;

		if (*p == '\"') {
			p++;
			f = '\"';
		} else {
			f = ' ';
		}
		v = p;
		e = strchr(p, f);
		if (e == NULL)
			e = strchr(p, '\0');
		if (e == NULL)
			break;
		p = e;
		if (*e != '\0')
			p++;
		*e = '\0';
		pw_properties_set(props, k, v);
	}
	free(s);
}

#include "module-null-sink.c"
#include "module-loopback.c"

static const struct module_info module_list[] = {
	{ "module-null-sink", create_module_null_sink, },
	{ "module-loopback", create_module_loopback, },
	{ NULL, }
};

static const struct module_info *find_module_info(const char *name)
{
	int i;
	for (i = 0; module_list[i].name != NULL; i++) {
		if (strcmp(module_list[i].name, name) == 0)
			return &module_list[i];
	}
	return NULL;
}

static struct module *create_module(struct client *client, const char *name, const char *args)
{
	struct impl *impl = client->impl;
	const struct module_info *info;
	struct module *module;

	info = find_module_info(name);
	if (info == NULL) {
		errno = ENOENT;
		return NULL;
	}
	module = info->create(impl, args);
	if (module == NULL)
		return NULL;

	module->idx = pw_map_insert_new(&impl->modules, module);
	if (module->idx == SPA_ID_INVALID) {
		module_unload(client, module);
		return NULL;
	}
	module->name = strdup(name);
	module->args = args ? strdup(args) : NULL;
	module->idx |= MODULE_FLAG;
	return module;
}
